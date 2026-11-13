# Native renderer: design

**Goal:** replace TFX's 320×200 software-rendered 3D view with a GPU renderer that draws the same world at any resolution and aspect ratio (true widescreen), keeps the game's own cockpit and HUD, and produces everything an AI upscaler or neural renderer needs. That includes NVIDIA DLSS (Super Resolution, Frame Generation and DLSS 5's neural rendering), AMD FSR or Intel XeSS.

Status: **stages 1–7 done**: scene capture, the Direct3D 12 presenter, the world drawn natively (`renderer=native`) with flat, Gouraud and textured polygons, discs, lines and the sky, per-pixel motion vectors, widescreen, and the upscaler slot with the port's own temporal upscaler (`upscaler=taa`), **AMD FSR** (`upscaler=fsr`) and **NVIDIA DLSS Super Resolution** (`upscaler=dlss`, Streamline 2.14.16). **NVIDIA DLSS Frame Generation** (`frame_generation=on`) is integrated too. DLSS 5 is not in that SDK. See "Milestones" and "Stage 7 as built".

## What DLSS needs from a renderer

| Input | Needed by | How this renderer provides it |
|---|---|---|
| Scene colour at render resolution, **without HUD/UI** | DLSS 5 (neural rendering), Super Resolution, Frame Generation | The 3D world is drawn natively from captured geometry; the cockpit/HUD is a separate layer |
| **Motion vectors** (per pixel, screen space) | DLSS 5, SR, FG | Each vertex's previous-frame position, from per-object view transforms of consecutive frames |
| Depth buffer | SR, FG (not an input to DLSS 5 according to NVIDIA) | View-space Z is known for every vertex |
| Sub-pixel camera **jitter** and its offsets | SR (temporal upscaling) | The projection is ours, so a jitter sequence can be added |
| Camera data (view/projection), reset flags | FG, SR | Projection centre, focal length (256 px at 320 wide) and the game's camera matrix |
| UI layer (colour + alpha) | FG (composited after generation) | The game's own cockpit/HUD picture with a mask |
| A D3D12 (or Vulkan) device and resources | Streamline SDK | The renderer is built on **Direct3D 12** |

NVIDIA states that DLSS 5 takes each frame's colour and motion vectors as input ([TweakTown](https://www.tweaktown.com/news/110569/dlss-5-only-takes-2d-rendered-frames-and-motion-vectors-as-input-not-3d-game-engine-data-confirms-nvidia/index.html), [Guru3D](https://www.guru3d.com/story/nvidia-unveils-dlss-5-with-neural-rendering-for-more-realistic-games/)). Final requirements were not published when this was written; re-check NVIDIA's developer documentation before stage 7. The DLSS/Streamline SDK itself must be downloaded and its licence accepted by the owner of this project; nothing of it is in the repository.

## Settings (tfx.ini)

Upscaling and DLSS 5 are **switchable options** in `tfx.ini`, the port's settings file. CONFIG.EXE is the original program and is not changed.

```
renderer=game        ; game = the original 320x200 picture (default)
                     ; native = the GPU renderer (stage 3 onwards)
                     ; wireframe = debug view of the capture
upscaler=taa         ; off | taa | fsr | dlss   (native renderer only; fsr/dlss use taa until integrated)
render_scale=100     ; scene resolution, percent of the window (100 = anti-aliasing, 50..99 = upscaling)
frame_generation=off ; off | on (2x) | 3x | 4x: DLSS Frame Generation (RTX 40/50,
                     ; native renderer + an upscaler; restart to turn on)
dlss5=off            ; off | on: DLSS 5 neural rendering pass (needs upscaler=dlss,
                     ; a supported GeForce RTX GPU and the DLSS SDK)
presenter=d3d12      ; d3d12 (default; falls back to sdl) | sdl
```

When an option can't be honoured (no supported GPU, SDK missing, or the stage isn't implemented yet), the port falls back to the next available choice and says so in `runtime.log`. These switches are parsed and logged now; they take effect as stages 3 and 7 land.

## How TFX draws its world (TFX_CODE.md §9.7 and §9.11)

1. **Bytecode VM** (`lodsw; jmp [eax+0x44A814]`): per object, it sets an object-to-view rotation matrix (in the code segment at `0x447C6E`) and the object's view-space origin. The origin is patched into `0x448990` at `0x4489A3/A9/AE`.
2. **Vertex transform** (`0x44C528`, `0x44C594` and similar): delta-coded vertices are rotated into 20h-byte vertex records.
3. **Projection** `0x448990`:
   - view X/Y/Z at +0/+4/+8;
   - near-plane flag at +0Ch;
   - screen point at +0Eh/+10h: x = 256·X/Z + cx, y = 256·Y/Z + cy, with cx/cy patched at `0x4489D8/ED` (159/56 in the cockpit view).
   - Vertices in front of the near plane switch the primitive routines to clipping versions.
4. **Primitives** (ESI = list of record offsets from `0x46814C`, ECX = count):

   | Primitive | Kind | Near-clipping version |
   |---|---|---|
   | `0x44C904` | flat (colour `0x447CA8`) | `0x466EF4` |
   | `0x44CEB8` | Gouraud (per-vertex shade) | `0x4675D0` |
   | `0x44D194` | textured (per-vertex u, v) | `0x467B84` |
   | `0x44CBE8` | effects variant | — |

   Each primitive does a bounding-box reject against the clip rectangle and a back-face test, then the edge builders, the scan converter `0x46AB50` and a span filler.
5. **Ordering:** painter's algorithm, back to front, with no depth buffer.
6. **Frame composition:**
   - the 3D view is drawn into the back page (Mode X, 3 pages);
   - the cockpit template (at BB80h) is copied over it and the HUD drawn on top;
   - then the page flips.

## Architecture

```
game (recompiled) ──► capture (scene.c)          per frame: polygons, view-space vertices,
     │                                            colours/shades/uvs, objects, camera
     │
     ├──► the game's own 320x200 frame ──► UI layer: cockpit + HUD (+ mask of 3D pixels)
     │
     ▼
 GPU renderer (Direct3D 12)
   1. scene pass at render resolution: colour (HUD-less), depth, motion vectors
   2. upscaler slot: none | FSR | DLSS (Streamline): SR / FG / DLSS 5
   3. composite: upscaled scene + UI layer (cockpit, HUD) scaled to the output
   4. present (tfx.ini aspect/scaling rules apply to the UI layer)
```

**Decisions:**
- **Direct3D 12:** Windows is the target, and it is DLSS's primary integration path; Frame Generation does not support D3D11. SDL stays for the window, input and audio; the D3D12 swap chain is created on SDL's window handle.
- **Painter's order with depth writes:** polygons are drawn in the game's order with depth testing off but depth writes on. The picture then matches the game exactly, including decals such as runway markings that rely on drawing order. The depth buffer still holds the visible (last drawn) surface, which is what DLSS expects.
- **The UI layer** is the game's own frame. Where the game drew its 3D view this frame becomes transparent, so the cockpit, HUD and MFDs stay pixel-identical. The 3D view region can be found by snapshotting the back page at the end of the 3D pass and diffing at the flip.
- **Re-projection, not re-implementation:** the game keeps doing all scene logic (culling, LOD, ordering, colours). The renderer only re-draws what the game decided to draw.

## Stage 3 as built

- **Triangles** (`runtime/native.c`):
  - Clip space with w = view Z (scaled by 1/65536), which reproduces x = 256·X/Z + cx, y = 256·Y/Z + cy inside the destination rectangle.
  - Depth z = (Z − 16)·F/(F − 16), with the near plane at Z = 16 as in the game and F = 32768.
  - Fans keep the game's vertex order, and polygons keep the game's order.
- **Back faces:**
  - Polygons with every vertex projected use the game's exact test on its own integer points: (y2−y0)(x1−x0) − (x2−x0)(y1−y0) ≥ 0 keeps.
  - Polygons crossing the near plane use a 3D orientation test, whose sign is learned each frame from polygons where both tests apply (agreement measured: 503 of 511).
  - Effects polygons from `0x44BBBC` skip the test when that handler has patched it out (`EB 4A` at `0x44CCA2`).
- **Derived copies:** `0x44CBE8` called from the near-plane routines (return address ≠ `0x44BCEA`) is a clipped copy and isn't drawn; the GPU clips the original.
- **3D/UI mask** (`vga.c`): every VGA byte remembers whether its last writer was the flat polygon fill (`rt_eip` in 471F00h–4726FFh). The displayed page's mask (320×200) is uploaded with the picture.
- **Composition** (`d3d12.c`):
  1. The game's picture, scaled as configured. It is the fallback for anything not drawn natively yet: Gouraud/textured polygons, sprites, points.
  2. The native triangles: depth written with the test set to always pass, so the game's painter's order wins; scissored to the picture.
  3. The game's picture again through the mask, keeping its UI pixels (cockpit, HUD, MFDs) on top and discarding its 3D pixels.
- **Measured** (Arcade, cockpit view, RTX 4070 Laptop, 1600×900): about 760 polygons and 1,035 triangles a frame, 27 fps unchanged; the terrain, sky bands and haze render natively, with the cockpit and HUD pixel-identical.
- **Known gaps:** no anti-aliasing yet; the native scene is drawn straight into the back buffer (the separate HUD-less scene target for DLSS comes with stage 5/7).

## Stage 4 as built

Sources: TFX_CODE.md 9.12.

- **Capture** (`runtime/scene.c`). Each primitive now also records:
  - its projection centre and clip rectangle, since a frame can contain several 3D viewports;
  - its Gouraud shades or texture words;
  - its remap table and texture base;
  - whether its back-face test is live (the `js` bytes);
  - whether it is a screen-space polygon (return address in the `0x4493xx` sky/haze block).
  - Near-plane routines that find every vertex projected jump to the plain routine; one copy is kept.
  - Discs (`0x44F26A`, hooked like the primitives) record their centre vertex and exact radius.
- **Drawing** (`runtime/native.c`, `d3d12.c`). Vertices carry u/v, a mode word, and four clip distances (`SV_ClipDistance`) for the primitive's clip rectangle.
  - Each frame the referenced texture windows (256×256 bytes, up to 64) and remap tables (up to 64) are copied into an R8_UINT texture array and table texture. The palette is uploaded as well.
  - The pixel shader repeats the span fills' lookups, but per output pixel and perspective-correct:
    - Gouraud: palette[table[shade]];
    - textured: palette[table[texel]], discarding 0.
  - Discs are 48-segment fans, elliptical in output pixels because game pixels aren't square, with the unrounded radius.
- **Composition.** Pixel ownership has three states:
  - **UI:** kept on top.
  - **Native 3D:** discarded from the game picture.
  - **Background 3D:** the screen-space sky and haze polygons, kept from the game's picture behind the native scene.
  - Where a native-3D pixel overwrote a UI pixel drawn in the same frame (the arming screen copies its background, then draws the model), the picture behind the native scene shows that background instead of the coarse game pixel. Otherwise the game pixel stays.
- **Measured** (RTX 4070 Laptop):
  - Arming screen: ≈ 500 primitives, 150 Gouraud, 9 textured, 1 disc. Every viewport is placed correctly; the helmet and visor render.
  - Arcade flight, cockpit: ≈ 690 primitives, 32 screen-space. Chase view: 940 primitives (102 Gouraud, 8 textured, 1 disc). Tactical and rear views were checked too.
  - 27.3 fps, unchanged.
- **Still from the game's picture:** points, lines (e.g. rivers and roads on the map), particles, and the screen-space sky/haze. (Model lines, including those drawn through `0x454336`, the sky/haze, the VM's points (lights), distant objects drawn as dots, and discs in every fill style are native since stage 6 and the later sky, line and effects work.)
- **Debugging:** `O native_debug=N` (1 skip flat and discs, 2 skip Gouraud, 4 skip textured, 8 textured as magenta, 16 no culling); the `S` command logs the capture, the span fills in use, and a textured-polygon dump.

## Stage 5 as built: motion vectors

- **Identity across frames.** A primitive's vertex list is part of its model's bytecode, so its linear address names the same polygon every frame. Vertex k of that list is the same model point. No object matrices or VM state are needed, and camera motion, moving aircraft and moving parts are all covered (`runtime/native.c`).
  - Each scene frame stores, per primitive key, the view-space vertices and the projection centre. The frame being drawn is matched against the previous scene frame. Presenting the same scene frame again keeps the same pair.
  - Models drawn several times (terrain tiles, repeated objects) share a key. Among up to 64 previous instances, the one nearest to where the camera motion puts vertex 0 is taken. A candidate more than 5 units away (≈ 30 m) is rejected: the terrain set changed and a tile reused the list.
- **Camera delta.** The rigid motion of the static world between the two frames (prev ≈ R·cur + t) is used:
  - to choose between instances;
  - for primitives with no match, which are assumed to be static scenery.

  It is fitted on uniquely matched vertices:
  - RANSAC over random triples, tolerance 1 unit, refitted with Horn's quaternion method.
  - A second RANSAC on the remaining pairs finds a second rigid group if there is one. In chase views the player's aircraft rides with the camera and forms a large group of its own. The group whose points are farther away is taken as the world.
- **Per pixel.** Every vertex carries its previous clip position (x, y, w with the previous projection centre). The rasterizer interpolates it perspective-correctly; the pixel shader divides and writes previous minus current position, in output pixels, to an R16G16_FLOAT target the size of the output (`d3d12.c`, second render target of the scene pass, cleared to 0).
  - Discs move with their centre.
  - Pixels not drawn natively have 0: the UI, and the screen-space sky/haze.
  - Points behind the camera in the previous frame are marked invalid.
- **Debug view:** `O native_debug=32` colours the scene by motion. Grey is still; red and green are +x and +y (±32 px saturates); dark blue has no valid previous position.
- **Measured** (Arcade, RTX 4070 Laptop), from the `S` log:

  | Case | Matched | Camera rotation / frame | Fit residual | On-screen motion |
  |---|---|---|---|---|
  | Cockpit, level flight | 563 of 563 | 0.000° (move 1.12 units) | 0.000 | mean 0.16 px, max 1.3 px |
  | Cockpit, rolling | 468 of 468 | 3.17° | 0.029 | mean 16 px, max 40 px |
  | Chase view, diving | ≈ 1,000 | 0.35°–0.70° | 0.03–0.08 (world group, about a third of the pairs; the rest is the aircraft) | mean 5–12 px |

  The frame rate is unchanged (27.3 fps).
- **Background motion** (2026-11-11). The sky/haze bands (and the wide view's far fill) are infinitely far away, so they move with the camera's rotation only.
  - A game pixel (gx, gy) is the view direction ((gx − cx) / 256, (gy − cy) / 256, 1); the fitted camera delta takes it to the previous frame's view space, where it projects again (`screen_prev` in `native.c`).
  - It is written as homogeneous clip coordinates, which are linear in (gx, gy), so interpolating them over a band gives the exact previous position of every pixel.
  - Lines known only in 2D (an end near-clipped) get the same rotation-only motion: exact in a turn for anything far away, nothing in straight flight.
  - Before, these had no motion vectors: DLSS (and frame generation) saw a still sky while the camera rolled, and showed the bands and the horizon glow as overlapping copies at earlier angles, and ghost outlines of those lines (DEBUG_LOG 115). The motion view (`O native_debug=32`) now shows one smooth field across the horizon and no primitive without motion.
- **Not yet:** jitter (it comes with the upscaler, stage 7). Lines (stage 6) move with the camera delta. Motion is measured between consecutive *scene* frames; the upscaler will run once per new scene frame.

## Stage 6 as built: widescreen

- **When.** `widescreen=on` (the default) applies with `renderer=native`, a window wider than the 4:3 picture, and a full-screen 3D view.
  - A full-screen 3D view means a primitive clipped to the whole 320 width in a frame that is still current.
  - The arming screen's viewports, menus and the in-flight map stay 4:3.
  - The view stops being current when no 3D has been drawn for 2 page flips, so 2D screens such as the map, with no 3D drawn, aren't mistaken for flight.
- **Geometry.** The game's culling already leaves a margin, enough for 16:9; for wider views its object visibility test is widened (see "Any ratio, and fullscreen").
  - Survey with the `S` log (Arcade, level flight): 186 polygons lie wholly outside the screen sides, 95 of them within 80 game pixels of the edge.
  - A 16:9 window needs 53 more game pixels per side at the game's vertical field of view.
  - The native renderer widens the clip rectangle of full-width primitives by that margin (`ext`, game pixels per side). The projection is unchanged, so the vertical field of view and the 4:3 centre stay as in the original.
- **Sky and haze** (screen-space polygons, TFX_CODE.md 9.12) are now drawn natively.
  - Each is a strip or half-plane clipped to the screen rectangle. Axis-aligned edges on the screen sides (x = 0 or 319, y = 0 or 200) are clipping artefacts; the others are the true boundary lines.
  - Testing against the 3D clip rectangle instead mistook a horizon below the cockpit panel's line for an artefact (DEBUG_LOG 82).
  - An edge on a screen side is still a true boundary when a neighbouring band of the chain (up to 4 before or after, ignoring zero-height ones) has the same edge and lies on its other side. Boundaries can fall on rows 199/200, and duplicate bands share every edge from the same side.
  - Zero-height bands are skipped (DEBUG_LOG 83–84).
  - The game's back-face test applies to the bands too: a band whose far corners went beyond the 16-bit range has its winding flipped, and the game skips it (DEBUG_LOG 91, TFX_CODE.md 9.14).
  - A band wholly outside the view rectangle (including the widescreen sides) is skipped: it covers nothing in the original either, and its rounded corners can make a false boundary (DEBUG_LOG 92).
- **Clouds** (Simulator cloud and storm weather, TFX_CODE.md 9.15) are the game's discs:
  - discs the game rejects (r ≥ `[0x466EA0]`, i.e. 100) aren't captured (DEBUG_LOG 90);
  - the puffs it draws with its stippled fills are drawn with that coverage as opacity: 50 % for the checkerboard fill `0x4726CE`, 25 % for `0x4727FF`. At the native resolution that gives soft, see-through cloud edges instead of the original's checkerboard (DEBUG_LOG 89). The scene pipeline blends colour by the primitive's alpha, which is 1 for everything else; motion vectors are written as they are.
- **Lights** (the VM's points, TFX_CODE.md 9.16) are captured at the plotters `0x4759A8`/`0x4759D4` when called from the point handlers, with their vertex (found through the handler's pushed bytecode pointer), so they have depth, identity and motion vectors.
  - Each is drawn through the disc path as a round dot with the area of the game's square (radius 0.564 × size game pixels), at the exact projection (half a pixel further for the 2×2).
  - Their pixels in the game's picture count as 3D now (only while a VM point is plotted), so the blocky originals don't show over the native scene.
  - At night the runway and approach lights resolve into separate dots where the original merges neighbours into a bar (DEBUG_LOG 94).
- The game's 25 % cloud fill (`0x4727FF`) lies past the range the pixel-ownership test took as 3D, so its checkerboard dots stayed in the UI layer on top of the native clouds; the range now ends at `0x472930` (DEBUG_LOG 95).
- **Art pack** (`ui_filter=remacri`, `runtime/art.c`, `tools/artpack.ps1`): MMPX, plus the cockpit's static art drawn from AI-upscaled images.
  - **Choosing the model.** Five 4× models were compared on the game's own cockpit art (Real-ESRGAN `x4plus` and `x4plus-anime`, UltraSharp, Remacri, NMKD Siax). Remacri was the only one that sharpened the panel shapes and stripes without garbling labels and numbers; Real-ESRGAN and UltraSharp turned "319" into "710", erased "GEAR/HOOK" and so on (DEBUG_LOG 110).
  - **Which art.** The panel strip, i.e. the template the game copies onto every frame from `BB80h` (TFX_CODE.md 9.20), and sprites in several colours of at least 8×8 (the MFD, radar and map pages). One-colour sprites are drawn by the sharp HUD; the 3×5 digits are too small for the model.
  - **Recognising it.** Each video-memory byte carries a provenance tag (`vga.c`):
    - a write-mode-1 copy whose latches were last loaded from the template records the template pixel it came from;
    - a sprite's pixels record the sprite, and once the sprite is finished (`scene.c`) the hash of its pixels (FNV-1a over its palette indices, transparent = 100h) is looked up in the pack;
    - any other write clears the tag, so text and values drawn later stay the game's.
    - The template is recognised by the hash of its first rows each frame.
  - **Drawing.** The displayed page's tags go to the GPU as a 320×200 R32_UINT texture (asset + 1 << 24, y << 12, x). In the UI pass, a tagged game pixel samples the asset's image in an atlas (2048 wide, 2-texel replicated border, per-asset origin and scale in a small table texture) at the same sub-pixel position, over the MMPX picture by its alpha. The descriptor table now has six SRVs (t3 tags, t4 atlas, t5 table).
  - **Pack** (`<exe dir>\art\remacri`): `pack.txt` lines `template|sprite <hash> <file> <game width> <game height>`, and `.tfxa` images (`TFXA`, width, height, BGRA rows). `tools/artpack.ps1` makes it from a cmdfile `A` dump: `prepare` (PNGs of the chosen art), `upscale` (Real-ESRGAN ncnn-vulkan with a 4× model), `pack`.
  - **Checked** in Arcade (paused, `off`/`mmpx`/`remacri` switched live): the pack loads (4 assets, atlas 2048×224); the panel strip and the MFD and radar pages are drawn from the Remacri images, aligned with the game's pixels; the values on top stay the game's (MMPX). A page whose content changes (the moving map) falls back to MMPX when its hash no longer matches.
- **Cockpit upscaling, MMPX** (`ui_filter=mmpx`, `d3d12.c` `ps_mmpx`): McGuire and Gagiu's MMPX, rule-based 2× magnification on exact colours, run twice: the 320×200 frame with the 3D and HUD-overlay pixels transparent (from the mask) into a 640×400 target, then into 1280×800.
  - The UI pass samples that bilinearly (premultiplied, alpha = coverage, as for xBR).
  - Chosen over xBR after an offline comparison on the same game frame (`tools/uifilters.py`: nearest, EPX, MMPX, xBR, xBR without blending). MMPX keeps text and short marks exactly as drawn and still turns slopes into clean lines; xBR rounded "BRG" into "8RG" and pitch-scale dashes into dots.
  - **Checked** in Arcade (paused, `off`/`mmpx`/`xbr` switched live): cockpit view and chase-view status bar.
- **Cockpit upscaling** (`ui_filter=xbr`, `d3d12.c` `xbr_ui`). The game's own pixels that stay in the UI layer (cockpit art, MFD pages, panel digits, the status bar) are upscaled in the masked UI pass with xBR level 2 (Hyllian's single-pass edge-directed pixel-art filter, any scale), instead of the `scaling` setting's nearest/bilinear sampling.
  - It reads the game's 320×200 frame and the 3D mask directly (`Load` of the 5×5 neighbourhood minus corners). Pixels the 3D view owns, or the sharp HUD overlay, count as transparent (a luma of −64, unlike any colour).
  - So the filter also smooths the cockpit's own outline against the native world. It works on premultiplied colour, and the result's alpha is the coverage: the masked UI pass now blends (source alpha) and writes that coverage into the frame-generation UI alpha.
  - The anti-aliasing band is one output pixel wide: the scale comes from the screen-space derivative of the texture coordinate.
  - The widescreen sides (`widescreen_sides=cockpit`) keep the plain sampling.
  - **Checked** (Arcade, paused, `off` vs `xbr` switched live with `O ui_filter=`): the panel diagonals and canopy frame are smooth lines, the panel corners rounded; the MFD text, the 3×5 digits and the status-bar font stay readable and keep their style. No measurable cost at 1600×900.
  - Not applied to menus and other 2D screens (they use the plain blit).
- **Sharp HUD** (`runtime/hud.c`, TFX_CODE.md 9.17). The HUD's text and symbols, the gun ring, the pitch ladder, the horizon bar and the heading tape are drawn again at the output resolution.
  - **Capture** (`scene.c`): every call of the 1-bit bitmap blit `0x45CFA4`, the HUD line rasteriser `0x454212` and the pixel plot `0x45658C` during a frame, with the bitmap's rows copied.
    - **Sprites** (TFX_CODE.md 9.19) are captured by the pixels the sprite blit `0x450454` writes, which `vga.c` passes to `scene.c`. They are owned by the HUD overlay until the sprite is finished.
    - A sprite in one colour, up to 254×254, becomes a 1-bit HUD bitmap: the speed/altitude brackets, the gun-cross markers, the heading tape ticks.
    - Any other sprite gives its pixels back to the UI layer: cockpit art, the MFD pages, the shaded panel digits.
    - The stats log (`S`) lists the next frame's sprites with their caller, size, colours and the decision. The second line rasteriser `0x454336` draws model lines, not the HUD (see "Model lines").
  - **Ownership** (`vga.c`): pixels these routines write while a captured item is drawn get owner 3, "HUD overlay": in the game's picture the value underneath shows instead (as under a 3D pixel), and the mask marks them 160 (3D 255, UI 0). The masked UI pass leaves them out (mask > 0.25).
  - **Drawing** (`d3d12.c`, after the masked UI pass, native 3D views only):
    - a bitmap goes into a per-frame R8 atlas (256×256, identical bitmaps shared). The pixel shader turns it into strokes: each set pixel is a point joined to its set neighbours (a diagonal only where neither shared side neighbour is set), drawn half a game pixel either side, and a corner of four set pixels is solid. One-pixel pixel-art lines and glyphs become smooth lines of the same weight, at the same place;
    - a line is a quad 0.7 game pixels wide between its end pixels' centres, with anti-aliased sides and ends;
    - colour from the palette, blended by coverage; nothing where the game's final pixel is UI, so cockpit art the game drew later still covers it;
    - with frame generation, the same pass adds its coverage to the UI alpha.
  - **Still the game's:** the MFD pages (whole multi-colour sprites, text and pictures together), the panel's shaded digits and labels, and the chase-view status bar (a two-colour font with a drop shadow). These are pixel art: split into one stroked bitmap per colour, the digits looked worse (DEBUG_LOG 106).
  - **Checked** (Arcade, paused, `renderer=game` vs `native`): every HUD element in place at the same weight; cockpit view, chase view; `taa`, and `dlss` with frame generation (status OK, 2 frames per game frame).
- **Checked against the original** (`dev/play/sky_case.sh`, `climb.sh`, `tools/compare.ps1`; the game paused with P and switched live between `renderer=game` and `native`):
  - runway starts in Yugoslavia at dawn, day clear/cloud/storm, night clear/cloud, Libya clear, Colombia dawn, Somalia storm and the carrier (ocean, day clear);
  - in cloud weather: take-off, inside the cloud layer, above it, banked climbs and dives over the cloud deck;
  - Arcade (Somalia, clear): level, rolled and nose-down.
  - All match, apart from 1–2 game pixels around the horizon haze at night. (A difference in the night lights seen earlier was the two pictures being taken a moment apart: compare with the game paused, DEBUG_LOG 96.)
  - The polygon is rebuilt as the widened rectangle cut by those lines (Sutherland–Hodgman), with Gouraud shades or texture coordinates extended by a plane fit in screen space. It is drawn first, at the far depth.
- **Effects** (TFX_CODE.md 9.18). Missile smoke and flames are discs and were already native.
  - **Fill styles.** Discs can use six more fill routines from the game's table at `0x4752C3`. The ownership test took only the solid, 50 % and 25 % ones (below `0x472930`) as 3D, so stippled pixels of other styles stayed on top of the native disc.
    - Now, while a captured disc is filled, writes from `0x472930`–`0x47431F` count as 3D too.
    - The native disc gets the style's share of pixels as alpha: `0x47292F` 1/16, `0x472C36` 1/32, `0x472F60`/`0x473ACD` 1/4 (read from the code; `0x47292F` also measured).
    - The stats log (`S`) gives, per style, the pixels written over the area of the unclipped discs (the solid fill reads 1.11 from pixel rounding).
  - **Distant objects as dots** (aircraft, debris, sparks: `0x44A57D`/`0x44A5F0`) are captured at the point plotters when called from there.
    - Their position comes from the game's object tables, with the same projection as the native discs: within half a game pixel of the game's dot.
    - They are drawn like the lights: round dots of equal area, keyed by object number, so they have motion vectors.
  - **Checked** (Arcade, paused, `renderer=game` vs `native`): cockpit view, and the missile view while the missile flies to its target. Smoke, sparks and debris match; the 2×2 orange and yellow blocks are round dots now.
  - Two single pixels per frame still come from `0x472F60`, which is called there for something other than a captured disc.
- **Model lines** (rivers, roads, edge lines on models: VM handler `0x44AFAC` → `0x44EA99`, TFX_CODE.md 9.14) are captured and drawn as thin quads at their 3D depth. They extend into the wide view, and their motion comes from the camera delta.
  - The VM's line handler `0x44B09C` draws its lines with a second rasteriser, `0x454336` (distant terrain detail on the horizon in Arcade). Those lines were captured, but their pixels in the game's picture stayed UI, so the 320×200 copy lay on top of the native line. Now, while `0x454336` is called from that handler (return `0x44B0AC`), its pixels count as 3D (DEBUG_LOG 101). The stats log (`S`) counts these lines: "126 lines (56 through 0x454336)".
- **Full height at the sides.** In the cockpit view the game clips its 3D view where the panel starts (about row 145).
  - In widescreen, full-width views are clipped to the whole screen height instead: native primitives, discs, lines, and the rebuilt sky/ground bands.
  - The world then fills the sides down to the bottom of the window. In the middle the cockpit still covers it.
- **Composition** (`d3d12.c`). The native scene is scissored to the whole window width. The game's UI layer stays at its 4:3 place.
  - `widescreen_sides=world` (default): the sides show only the world.
    - The cockpit art fills its outer edges with one flat dark colour (#242424 in the F-22 cockpit): columns 0–6 widening to 11 at the bottom on the left, from about 307 on the right.
    - In this mode a run of that colour touching the left or right edge becomes see-through (`vga.c`, mask), so the panel ends at its instrument housings and the world continues.
    - Only runs darker than #303030 are affected, so status bars stay.
    - The chase view's status bar background is itself a screen-space polygon and is rebuilt wide like the sky.
  - `widescreen_sides=cockpit`: the masked UI pass runs over the full width with clamped sampling, so rows continue sideways. A row continues only where the UI is solid for 4 columns in from the edge (the cockpit panel, the status bar); a thin line crossing the edge doesn't.
- **Measured** (1600×900, RTX 4070 Laptop):
  - Cockpit, roll, chase and tactical views fill 16:9 with terrain, sky and lines.
  - 185 model lines and 34 sky/haze polygons per frame in chase view.
  - 27.3 fps.
- **Any ratio, and fullscreen** (2026-11-11). The margin `ext` follows the window: 53 game pixels per side at 16:9, 130 at 21:9, 267 at 32:9 (a horizontal field of view of about 118°).
  - **Bands that slope into the wide view.** A band wholly outside the view used to be skipped by its bounding box (a guard against a rounding sliver, DEBUG_LOG 92). But a band is a strip between two parallel lines: in a roll the far ground band lay just above the game's 4:3 picture and came down into the wide view's corner, which stayed empty: black triangles in the corners for a few frames of every roll, already at 16:9. The band is now skipped only when its own direction (its longest edge that isn't on a screen side) can't bring it into the view.
  - **Far fill.** The bands end at some distance from the horizon; beyond that the picture relies on terrain polygons, which the game doesn't submit far outside its own view. With `ext > 0` the far end of each chain of bands (a chain: bands that each share an edge with the next; its far end: the thicker of its first and last band) is continued as a half-plane behind everything, in that band's colour. So missing terrain at the far sides shows ground colour, and the sky has no upper limit.
  - **Clear fill.** Before the bands the game fills its 3D view with one colour (the rectangle fill `0x458E34`), and looking steeply up or down it draws no bands at all: the 4:3 middle then shows that fill from the game's picture, and the sides had nothing (black bars beside a 4:3 picture, DEBUG_LOG 121). `vga_clear_index` finds the colour in the displayed picture (the commonest among the UI pixels that fill last wrote, when at least 200 are left), and the wide view starts with a rectangle of it behind everything. `native_debug` bits 64 and 128 leave out this fill and the far fill (for tests).
  - **Fullscreen** (`fullscreen=1`, Alt+Enter, or `O fullscreen=` live) is SDL's borderless desktop mode; the swap chain, DLSS and frame generation follow the size change. Checked at 5120×1440: 54.6 game frames a second, DLSS (DLAA) at that size, frame generation at 2 frames per game frame (the fullscreen window has the focus), back to a window and to fullscreen again.
  - **Checked** with `dev/play/roll_gaps.sh` (a roll, window captures about 0.2 s apart, uncovered pixels counted by their alpha): 16:9 0 of 40 frames with gaps (before: 4 of 24), 21:9 0 of 30, 32:9 0 of 50 (with the band fix alone: 1 of 40). 16:10, 4:3 and 5:4 windows by eye.
  - **The game's culling, widened** (TFX_CODE.md 9.21). The game keeps an object when its centre, less its radius, projects within 160 pixels of the view centre, and never beyond 45° (256 pixels). At 32:9 the view reaches 427 pixels, so terrain pieces and objects were missing at the far sides. With the wide view on, the port replaces that test for the full-width viewport: the same, with the horizontal limit at the window's real half width and no 45° cap. No other game code is changed; the extra objects go through the game's own drawing (clipped away there) and are captured for the native view.
    - Arcade, 32:9: about 1,030 primitives a frame instead of 630, the draw queue up to 115 of 348, still 54.6 fps. 16:9: about 6 more objects a frame. 4:3: the original test runs.
    - The stats log (`S`) reports the objects tested, those kept beyond the 4:3 limit and the fullest the draw queue got.
  - **Limits.** 7680×2160 itself wasn't available to test (the same 32:9 shape as 5120×1440). Parts of models that the game's bytecode skips by its own screen tests would still be missing at the sides; none was seen.
- **Not done:** a field-of-view setting. The vertical FOV stays the original's, and widening it would need the game's culling margin widened too. Objects entirely beyond the original culling margin (far beyond 80 game pixels at the sides) can be missing at the window edges.

## Stage 7 as built: the upscaler slot

- **Scene targets** (`d3d12.c`). With an upscaler on, the native scene is drawn into its own targets at the render resolution (`render_scale` × the window, 50–100 %), not into the back buffer:
  - colour, B8G8R8A8, premultiplied alpha: 0 where nothing native is drawn, so the HUD-less scene has coverage;
  - D32 depth;
  - R16G16_FLOAT motion.

  The clip-space vertices don't depend on the target size; only the viewport and scissor are scaled.
- **Jitter.** A Halton (2,3) sequence of 8 sub-pixel offsets (render pixels, −0.5…0.5) is added in the scene vertex shader (`pos.xy += jitter_ndc · w`).
  - Motion vectors are written without jitter (`previous − (SV_Position − jitter)`), in render pixels.
  - The jitter advances on every present. Between the game's 27 scene frames a second, re-presented frames carry motion 0 and a new jitter, so edges keep refining while the picture is still.
- **The port's temporal upscaler, `taa`** (`ps_taa`, one full-screen pass into an FP16 history at the output resolution, ping-pong):
  1. **Current colour:** the 3×3 jittered render samples around the output pixel, weighted by distance (Gaussian, σ ≈ 0.47 render px).
  2. **History:** fetched where the motion vector says this point was, and clamped to the 3×3 neighbourhood's min/max against ghosting.
  3. **Blend:** weight max(0.04, 0.12 × the centre sample's weight). History is discarded on a reset or when the reprojected point is off-screen.
  4. **Composite:** the history is copied over the game's picture (`ps_comp`), then the cockpit/HUD layer goes on top as before.
- **Backdrop.** Before the scene is drawn into the render-resolution target, the game's picture is drawn there as a backdrop.
  - The upscaler then gets an opaque image, and model edges are anti-aliased against the real background.
  - FSR has no alpha channel. Composing its colour with separately accumulated coverage gave white fringes on the arming screen, where models sit over 2D art.
  - The cockpit/HUD pass afterwards replaces every non-3D pixel with the crisp original.
- **Reset** (`vga.c`): a new scene frame where fewer than half the primitives found their previous self (a view change), more than one scene frame between the pair, a size change, or the upscaler just switched on.
- **The interface for other upscalers** (`present.h`, `struct upscale_params`).
  - Inputs: colour, depth and motion at render resolution (motion: previous − current, render pixels, no jitter), the jitter offset, render and output size, and a reset flag.
  - Output: the scene at the output resolution, composited under the UI. This matches what FSR 2/3 (`ffxFsr2ContextDispatch`) and DLSS through Streamline (`slSetTag` for colour/depth/motion/output, `slSetConstants` with jitter and reset, `slEvaluateFeature`) expect.
  - Each of them would replace step 1b–1c (the `ps_taa` pass) in `d3d_present`.
- **Settings.**
  - `upscaler=off|taa|fsr|dlss` (default `taa` in new `tfx.ini` files) and `render_scale=50..100` (default 100).
  - `fsr` falls back to `taa` when AMD's DLLs can't be loaded; `dlss` falls back to `taa` with a line in `runtime.log`; `dlss5` stays off. `upscaler` and `render_scale` can be changed live with the `O` command.
- **Measured** (chase view, 1600×900, RTX 4070 Laptop):
  - At 100 %, polygon edges, the canopy and model lines are anti-aliased without visible softening.
  - At 50 % (800×450 render) the picture is only slightly softer than at 100 %.
  - 27.3 fps in both cases.
- **AMD FSR** (`runtime/fsr.c`, `upscaler=fsr`):
  - **Source:** FidelityFX SDK v2.4.0 (GitHub `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`, 28 Oct 2026).
    - The C API headers (`ffx_api.h`, `ffx_api_types.h`, `dx12/ffx_api_dx12.h`, `ffx_upscale.h`).
    - The AMD-signed `amd_fidelityfx_loader_dx12.dll` (39 KB) and `amd_fidelityfx_upscaler_dx12.dll` (57.7 MB), with their licence.
    - All kept in `toolchain/fidelityfx-2.4.0/`, outside git. Authenticode signatures checked: valid, Advanced Micro Devices.
    - `tools/build.py` copies the DLLs and `AMD_FidelityFX_LICENSE.md` next to `tfx.exe` (binary redistribution is allowed with the licence notice).
  - **Loading:** the loader is loaded at run time from the exe's folder only (`LoadLibraryEx` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`), with `ffxCreateContext/ffxQuery/ffxDispatch/ffxDestroyContext` from `GetProcAddress`. Nothing links against AMD code.
  - **The DLLs are x64 only**, so the port is now built for **x86_64** (`tools/build.py`, `TARGET=i686` still builds 32-bit, without FSR). The 64-bit build needed one fix (DEBUG_LOG 80).
  - **Provider:** it picks FSR 4 (ML) on Radeon RX 9000 and FSR 3.1 elsewhere. On the RTX 4070 Laptop it reports provider 3.1.6.
  - **Context:** created for the output size (`maxRenderSize` = output, dynamic resolution), so `render_scale` can change without recreating it. Recreated, with the GPU idle, when the window size changes.
  - **Inputs:**
    - colour B8G8R8A8 gamma-encoded (`ENABLE_NON_LINEAR_COLORSPACE`, `NON_LINEAR_COLOR_SRGB`);
    - depth from an R32_TYPELESS target viewed as D32 (standard 0–1, near Z 16, far 32768);
    - motion R16G16_FLOAT in render pixels, current → previous, `motionVectorScale` 1. It is 0 on a re-presented scene frame.
    - **Jitter:** FSR's own sequence (`GETJITTERPHASECOUNT/GETJITTEROFFSET`), applied in clip space as (2 jx / W, −2 jy / H), as AMD's documentation prescribes.
    - **Camera:** vertical FOV 2·atan(100/256); frame time from the performance counter.
  - **Output:** an R16G16B16A16 UAV texture at the output size, copied opaquely over the scene region (thanks to the backdrop).
  - **Recording:** FSR records compute work into the port's command list, then the root signature, heaps and graphics state are set again.
  - **Debugging:** `TFX_FSR_DEBUG=1` enables FSR's API checks, logged as `fsr:` lines. Only two expected warnings appear: no exposure texture (the input isn't HDR), and zero motion scale on re-presents.
  - **Measured** (RTX 4070 Laptop, 1600×900): FSR at 100 % and 50 % (800×450) in chase, roll and widescreen cockpit views, and on the arming screen; 27.3 fps.
- **NVIDIA DLSS Super Resolution** (`runtime/dlss.cpp`, `upscaler=dlss`):
  - **Source:** NVIDIA Streamline SDK v2.14.16, downloaded by whoever builds the port, under NVIDIA's licence (`build.bat -Upscalers`; never committed).
    - Extracted to `toolchain/streamline-2.14.16/`: the headers, the production `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll` and `nvngx_dlss.dll`, the licences and the docs.
    - All four DLLs are Authenticode-signed by NVIDIA Corporation (checked).
    - `tools/build.py` copies them, with `NVIDIA_Streamline_LICENSE.txt` and `NVIDIA_DLSS_LICENSE.txt`, next to `tfx.exe`.
  - **Integration:** Streamline in **manual hooking** mode.
    - `slInit` (DLSS only; custom engine + project GUID, no NVIDIA application id) runs in `d3d_init` before the device when `upscaler=dlss`. Changing to or from `dlss` therefore needs a restart.
    - The interposer is loaded from the exe's folder after `sl::security::verifyEmbeddedSignature`. Its C entry points come from `GetProcAddress`, and `slGetFeatureFunction` is defined in the wrapper so the `sl_dlss.h` helpers work.
    - Order: device → `slSetD3DDevice` → queue created through the upgraded device → swap chain created through the upgraded DXGI factory. That way Streamline sees Present.
    - Over-the-air plug-in downloads are off. Frame-based tagging (`eUseFrameBasedResourceTagging`) is required for `slSetTagForFrame`.
  - **The wrapper is C++** (Streamline's headers are C++), built with the toolchain's `clang++` (`-std=c++17`, Windows 10 API level for `sl_security.h`); the exe is now linked with `clang++` and `-lwintrust -lcrypt32`. The structures shared with the MSVC-built DLLs all derive from a 32-byte `BaseStructure` without tail padding, so the GNU and MSVC layouts agree.
  - **Per present:**
    - `slGetNewFrameToken`, `slDLSSSetOptions` (mode from `render_scale`: 100 DLAA, ≥ 67 Quality, ≥ 58 Balanced, else Performance), and the render size from `slDLSSGetOptimalSettings`.
    - `slSetConstants`:
      - jitter in pixels; Halton (2,3) with 8·(output/render)² phases;
      - `mvecScale` 1/render size, or 0 on a re-presented scene frame;
      - camera motion included, depth not inverted;
      - matrices for the game's camera without jitter, `clipToPrevClip` = identity;
      - reset on cuts.
    - `slSetTagForFrame` for colour, depth, motion and the output (`fsr_out`, UAV), with their actual D3D12 states, then `slEvaluateFeature`.
    - The host then restores its root signature, heaps, viewport and topology, and the output is copied over the scene region as for FSR.
  - **Measured** (RTX 4070 Laptop, driver 621.14, 1600×900):
    - DLAA 1600×900 and Performance 800×450 → 1600×900, switched live, in chase and cockpit views; 27.3 fps.
    - One Streamline warning (it overrides an unusual resource alignment). No errors after the two fixes in DEBUG_LOG 85–86. Clean shutdown.
  - **Not done:** DLSS 5 (not in Streamline 2.14.16).
- **NVIDIA DLSS Frame Generation** (`frame_generation=on|3x|4x`, `runtime/dlss.cpp` + `d3d12.c`):
  - **DLLs:** `sl.dlss_g.dll`, `nvngx_dlssg.dll`, `sl.reflex.dll` and `sl.pcl.dll` from the same SDK, all Authenticode-signed by NVIDIA (checked), copied next to `tfx.exe` with `NVIDIA_Reflex_LICENSE.txt`.
  - **Initialisation:** when `frame_generation` is on, `slInit` also loads DLSS-G and Reflex (DLSS-G requires Reflex; PCL is always loaded). It works with any upscaler (`taa`, `fsr`, `dlss`), because it only needs the upscaler slot's depth and motion. Support is checked with `slIsFeatureSupported` for DLSS-G and Reflex; a missing Windows hardware-accelerated GPU scheduling is named in the log. Reflex is set to low-latency mode once.
  - **One Streamline frame per present:**
    - `dlss_frame_begin` gets the frame token, calls `slReflexSleep`, and sets the PCL simulation start/end markers.
    - PCL render-submit markers go around the command list; present markers go around `Present`.
    - The common constants are set exactly once per frame, in menus too (identity, reset).
    - DLSS SR's evaluation uses the same token.
  - **Inputs** (tagged `eValidUntilPresent`, in the states they have at `Present`):
    - depth (`up_depth`, `DEPTH_WRITE`) and motion (`up_motion`, `RENDER_TARGET`) at render size;
    - the **HUD-less colour**, a copy of the backbuffer taken after the upscaled scene is composited and before the cockpit/HUD pass;
    - the **UI alpha** (R8, backbuffer size), written as 1 by the cockpit/HUD pass itself (`ps_masked_ui`, a second render target), 0 elsewhere.
    - User-interface recomposition is on (HUD-less + UI alpha); `eRetainResourcesWhenOff` keeps the resources across menus.
  - **On/off:** on only for frames of the 3D view through the upscaler; in menus, maps and other 2D screens it is off and the tags are null. A window resize first presents one frame with it off, then resizes the swap chain.
  - **Pacing:** with frame generation on, the port presents only the game's own frames (a new scene frame). A re-present is skipped, except after 100 ms without a new frame (a paused game), or when a window capture (`W`) is pending (DEBUG_LOG 104). `rt_service` presents as soon as the game flips a page instead of on the next 14 ms tick, so the generated frames sit evenly between real ones. (Since `stage8-framerate` it does so without frame generation too, at most every 4 ms between new frames: DEBUG_LOG 107.)
  - **Measured** (RTX 4070 Laptop, driver 621.14, 1600×900, DLAA, Arcade flight):
    - the game makes 27.5 frames/s (55 per 2 s); the swap chain presents 110 per 2 s (55/s); DLSS-G status OK, 2 frames shown per game frame;
    - this GPU generates at most 1 frame per game frame (2x; 3x/4x need RTX 50), and VSync is supported;
    - desktop captures of the window: cockpit and HUD crisp in real and generated frames;
    - clean shutdown; DLSS SR without frame generation still runs as before.
  - **Needs the window in focus:** DLSS-G pauses itself while the window is not focused ("DLSS-G disabled: window not focused" in the log; DEBUG_LOG 87).
- **Known gaps:**
  - No sharpening pass yet.
  - Points that aren't lights or object dots still come from the game's picture.

## Milestones

| # | Stage | Done when |
|---|---|---|
| 1 | **Scene capture** (`runtime/scene.c`, recompiler entry hooks) | Every primitive with view-space vertices per frame; re-projection matches the game's own points; wireframe debug view (`renderer=wireframe`) lines up with the picture at full window resolution |
| 2 | D3D12 presenter (**done**, `runtime/d3d12.c`) | SDL_Renderer replaced: game frame texture, the tfx.ini aspect/scaling modes, window capture; identical output. Flip-model swap chain on SDL's HWND, tearing when available; shaders compiled at start-up with D3DCompile (SM 5.0); a full-screen-triangle blit (point/linear; "sharp" through an integer-scaled render target); a line pipeline for overlays; back-buffer readback for `W` captures; swap chain resized with the window. `presenter=sdl` or a missing D3D12 GPU falls back to SDL's renderer |
| 3 | Flat polygons natively (**done**, `runtime/native.c`, `d3d12.c`) | Flat (and effects) polygons drawn on the GPU in painter's order with near-plane clipping in 3D; UI layer separation; depth written. Details below |
| 4 | Shading and textures (**done**, see "Stage 4 as built") | Gouraud shades and textured polygons reproduced (attribute sources, remap tables, texture windows); discs; per-viewport projection and clipping. Points, lines and particles still come from the game's picture |
| 5 | Motion vectors (**done**, see "Stage 5 as built") | Per-primitive identity across frames (bytecode list address), camera delta for the rest, per-pixel motion target; a debug view of the vectors |
| 6 | Widescreen (**done**, see "Stage 6 as built") | Clip rectangle widened (the game's culling margin is enough at 16:9); sky/haze rebuilt wide; model lines native; cockpit composition for wide screens. FOV setting not done |
| 7 | Upscalers (**slot, `taa`, FSR and DLSS SR done**, see "Stage 7 as built") | Upscaler interface with jitter, HUD-less scene targets at render resolution, the port's temporal upscaler, AMD FSR (FidelityFX API, signed DLLs, x64 build), NVIDIA DLSS Super Resolution (Streamline 2.14.16, manual hooking) and DLSS Frame Generation (with Reflex); `upscaler=`, `render_scale=` and `frame_generation=` switch them. DLSS 5 not done (not in the SDK) |
