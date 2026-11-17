/* Port settings: tfx.ini next to the exe (written with defaults and comments
 * on first run).  These are enhancements of the port, not game options; the
 * game's own options stay in TFX.CFG (CONFIG.EXE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rt.h"
#include "settings.h"

struct port_settings settings = {
    .window_w = 1280, .window_h = 960, .fullscreen = 0,
    .aspect = ASPECT_4_3, .scaling = SCALE_SHARP, .framerate_high = 1, .joystick = 1, .head_tracking = 1,
    .widescreen = 1, .render_scale = 100,
};

static const char *default_ini =
    "; TFX port settings (this file is read at start-up; the game's own\n"
    "; options are in TFX.CFG, set with CONFIG.EXE / tfxconfig.exe).\n"
    "; tfx.exe --settings (controls.bat, Settings page) sets them with a click\n"
    "\n"
    "; window size when not fullscreen\n"
    "window=1280x960\n"
    "; 1 = start fullscreen (Alt+Enter toggles at any time)\n"
    "fullscreen=0\n"
    "\n"
    "; picture shape: 4:3 = as on a 1993 monitor (the game's 320x200 pixels\n"
    ";                       are taller than wide), recommended\n"
    ";                square = square pixels (16:10, slightly squashed)\n"
    ";                fill = stretch to the whole window/screen (e.g. 16:9)\n"
    "aspect=4:3\n"
    "\n"
    "; scaling: sharp = crisp pixels without uneven sizes (recommended)\n"
    ";          pixel = whole-number scaling only (may leave borders)\n"
    ";          smooth = soft bilinear filtering\n"
    "scaling=sharp\n"
    "\n"
    "; framerate: high = 54.6 fps, a frame every tick of the game clock (the\n"
    ";            game's time step is per tick, so flight speed and every\n"
    ";            timer stay correct)\n"
    ";            normal = the original's limit, 27 fps\n"
    "framerate=high\n"
    "\n"
    "; renderer: game = the original 320x200 picture\n"
    ";           native = the new GPU renderer (in development, see RENDERER.md):\n"
    ";                    polygons and sprites drawn at full resolution, the rest\n"
    ";                    still from the game's picture; needs presenter=d3d12\n"
    ";           wireframe = debug view of the native renderer's scene capture\n"
    "renderer=game\n"
    "\n"
    "; widescreen (native renderer): on = the 3D world fills the whole window\n"
    ";             width; the cockpit and HUD stay 4:3 in the middle.  off = 4:3\n"
    "widescreen=on\n"
    "; widescreen_sides: world = the 3D world at the sides, also below the\n"
    ";                   cockpit panel's level; cockpit = the cockpit panel and\n"
    ";                   status bar continued sideways\n"
    "widescreen_sides=world\n"
    "; ui_filter (native renderer, 3D views): off = the cockpit, MFDs and\n"
    ";            other game pixels scaled as set by scaling; mmpx = pixel-art\n"
    ";            upscaling (MMPX, 4x): smooth slopes, text kept as drawn;\n"
    ";            xbr = xBR: smoother, rounder (rounds small text);\n"
    ";            remacri = MMPX, plus the static cockpit art drawn from\n"
    ";            art\\remacri (AI-upscaled with Remacri; see README)\n"
    "ui_filter=off\n"
    "\n"
    "; upscaler (native renderer only): off | taa | fsr | dlss\n"
    ";   taa = the port's own temporal upscaler / anti-aliasing (jittered\n"
    ";         scene, motion-vector history)\n"
    ";   fsr = AMD FSR (FidelityFX DLLs next to tfx.exe; FSR 4 on Radeon RX 9000,\n"
    ";         FSR 3.1 elsewhere); taa if they can't be loaded\n"
    ";   dlss = NVIDIA DLSS Super Resolution (Streamline DLLs next to tfx.exe,\n"
    ";          GeForce RTX); taa if it can't be used.  Changing to or from\n"
    ";          dlss needs a restart\n"
    "upscaler=taa\n"
    "; render_scale: scene resolution for the upscaler, percent of the window\n"
    ";   (100 = anti-aliasing only, 50..99 = upscaling)\n"
    "render_scale=100\n"
    "\n"
    "; frame_generation: NVIDIA DLSS Frame Generation: off | on (2x) | 3x | 4x\n"
    ";   (GeForce RTX 40/50; 3x/4x on RTX 50 only, else 2x).  Needs the native\n"
    ";   renderer, an upscaler and Windows' hardware-accelerated GPU\n"
    ";   scheduling.  The game keeps its own frame rate; DLSS adds frames in\n"
    ";   between in flight (not in menus).  Turning it on needs a restart\n"
    "frame_generation=off\n"
    "\n"
    "; DLSS 5 neural rendering: off | on (needs upscaler=dlss, a supported\n"
    "; GeForce RTX GPU and NVIDIA's DLSS SDK; otherwise it falls back to off)\n"
    "dlss5=off\n"
    "\n"
    "; presenter: d3d12 = Direct3D 12 (default; needs a DirectX 12 GPU and falls\n"
    ";            back to sdl automatically) | sdl = SDL's renderer\n"
    "presenter=d3d12\n"
    "\n"
    "; joystick: auto = the first game controller or joystick found flies the\n"
    ";           aircraft (the game sees a CH Flightstick Pro and is switched\n"
    ";           to joystick control while one is connected); off = keyboard\n"
    ";   The game's joystick buttons: button1 = fire, button2 = air-to-ground\n"
    ";   weapons, button3 = air-to-air weapons, button4 = next target.\n"
    ";   Joystick: stick, throttle lever (also a separate throttle unit), hat\n"
    ";   = look around, buttons 1-4 = button1..button4.  Other buttons do\n"
    ";   nothing until bound: start tfx.exe --controls for a window that\n"
    ";   does it (it writes the joy lines of this file), or write them below.\n"
    ";   Controller: left stick, d-pad = look around, A/B/X/Y = button1..4,\n"
    ";   triggers = throttle down/up, LB = air brake, RB = cannons, Back =\n"
    ";   cockpit view, Start = pause, stick clicks = gear, wheel brakes.\n"
    ";   In the menus the stick moves the pointer and button1 clicks.\n"
    "joystick=auto\n"
    "; joy_device: the main device (stick, hat, default buttons) when several\n"
    ";   are connected: its number in the log or part of its name\n"
    "; joy_deadzone: percent of the stick's travel ignored around the centre\n"
    ";   (default: 10 for a controller, 4 for a joystick)\n"
    "; joy_buttonN and joy_hat_up|right|down|left (main device), joyK_buttonN\n"
    "; and joyK_hat_up... (device K of the log), and for a controller joy_a,\n"
    "; joy_b, joy_x, joy_y, joy_lb, joy_rb, joy_lt, joy_rt, joy_back, joy_start,\n"
    "; joy_ls, joy_rs, joy_dpad_up|right|down|left:\n"
    ";   button1..button4, look_front|look_right|look_back|look_left (the\n"
    ";   game's hat), none, or a key as in the manual, with a modifier\n"
    ";   if needed: Space, Return, Backspace, C, F1, Insert, Keypad +, RCtrl,\n"
    ";   Alt+L, LShift+B, RShift+F1.  Example: joy_button5=B\n"
    "; head_tracking: auto = in the virtual cockpit (F10) the view follows a\n"
    ";   head tracker that speaks FreeTrack (opentrack: output \"freetrack\");\n"
    ";   yaw and pitch, all the way round.  Without a tracker running nothing\n"
    ";   changes.  off = never\n"
    "head_tracking=auto\n"
    "; joy_axes: x,y,rudder,throttle as [device.]axis; 0 = none, negative =\n"
    ";   reversed.  Example: joy_axes=1,2,0,-2.4 (throttle: device 2, axis 4,\n"
    ";   reversed).  The rudder has no axis unless given here\n";

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* one key=value setting (tfx.ini, or the live command O) */
void settings_set(const char *k, const char *v) {
    if (!_stricmp(k, "window")) {
        int w, h;
        if (sscanf(v, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200) { settings.window_w = w; settings.window_h = h; }
    } else if (!_stricmp(k, "fullscreen")) settings.fullscreen = atoi(v) != 0;
    else if (!_stricmp(k, "aspect")) {
        if (!_stricmp(v, "square")) settings.aspect = ASPECT_SQUARE;
        else if (!_stricmp(v, "fill")) settings.aspect = ASPECT_FILL;
        else settings.aspect = ASPECT_4_3;
    } else if (!_stricmp(k, "scaling")) {
        if (!_stricmp(v, "pixel")) settings.scaling = SCALE_PIXEL;
        else if (!_stricmp(v, "smooth")) settings.scaling = SCALE_SMOOTH;
        else settings.scaling = SCALE_SHARP;
    } else if (!_stricmp(k, "framerate")) settings.framerate_high = !_stricmp(v, "high");
    else if (!_stricmp(k, "renderer"))
        settings.renderer = !_stricmp(v, "wireframe") ? RENDERER_WIREFRAME : !_stricmp(v, "native") ? RENDERER_NATIVE : RENDERER_GAME;
    else if (!_stricmp(k, "upscaler"))
        settings.upscaler = !_stricmp(v, "dlss") ? UPSCALER_DLSS : !_stricmp(v, "fsr") ? UPSCALER_FSR :
                            !_stricmp(v, "taa") ? UPSCALER_TAA : UPSCALER_OFF;
    else if (!_stricmp(k, "render_scale")) {
        int r = atoi(v);
        settings.render_scale = r < 50 ? 50 : r > 100 ? 100 : r;
    }
    else if (!_stricmp(k, "dlss5")) settings.dlss5 = !_stricmp(v, "on") || !_stricmp(v, "1");
    else if (!_stricmp(k, "frame_generation"))
        settings.frame_gen = !_stricmp(v, "on") || !_stricmp(v, "2x") || !_stricmp(v, "1") ? 2 :
                             !_stricmp(v, "3x") ? 3 : !_stricmp(v, "4x") ? 4 : 0;
    else if (!_stricmp(k, "presenter")) settings.presenter = !_stricmp(v, "sdl") ? PRESENTER_SDL : PRESENTER_D3D12;
    else if (!_stricmp(k, "widescreen")) settings.widescreen = !_stricmp(v, "on") || !_stricmp(v, "1");
    else if (!_stricmp(k, "widescreen_sides")) settings.widescreen_cockpit = !_stricmp(v, "cockpit");
    else if (!_stricmp(k, "ui_filter")) settings.ui_filter = !_stricmp(v, "remacri") ? 3 : !_stricmp(v, "mmpx") ? 2 : !_stricmp(v, "xbr") ? 1 : 0;
    else if (!_stricmp(k, "joystick")) settings.joystick = _stricmp(v, "off") && _stricmp(v, "0");
    else if (!_stricmp(k, "head_tracking")) settings.head_tracking = _stricmp(v, "off") && _stricmp(v, "0");
    else if (!_strnicmp(k, "joy", 3)) { if (!joy_setting(k, v)) rt_log("settings: too many joystick settings (%s)", k); }
    else if (!_stricmp(k, "native_debug")) { extern int native_debug; native_debug = atoi(v); }   /* undocumented */
}

/* Apply fallbacks for options that this build or this PC can't honour yet,
 * and say so (RENDERER.md: stages 3 and 7 enable them). */
void settings_resolve(void) {
    static const char *rn[] = {"game", "native", "wireframe"}, *un[] = {"off", "fsr", "dlss", "taa"};
    int r = settings.renderer, u = settings.upscaler, d = settings.dlss5;
    if (settings.renderer != RENDERER_NATIVE) settings.upscaler = UPSCALER_OFF;
    /* FSR 2/3 and DLSS (Streamline) plug into the same slot as taa once their
     * SDKs are in the build (RENDERER.md, stage 7); until then: taa */
    /* fsr / dlss: AMD's and NVIDIA's signed DLLs next to the exe (x64 build);
     * taa at run time when they can't be used (d3d12.c).  DLSS 5 isn't in
     * the Streamline SDK the port is built with (2.14.16) */
    settings.dlss5 = 0;
    rt_log("settings: renderer=%s upscaler=%s render_scale=%d%% dlss5=%s (requested renderer=%s upscaler=%s dlss5=%s)",
           rn[settings.renderer], un[settings.upscaler], settings.render_scale, settings.dlss5 ? "on" : "off",
           rn[r], un[u], d ? "on" : "off");
    if (u != settings.upscaler || d != settings.dlss5)
        rt_log("settings: DLSS 5 is not in the Streamline SDK this build uses (2.14.16); dlss5 is off");
    /* frame generation needs the upscaler slot's depth and motion */
    if (settings.frame_gen && settings.upscaler == UPSCALER_OFF) {
        settings.frame_gen = 0;
        rt_log("settings: frame_generation needs renderer=native and an upscaler; it is off");
    }
    if (settings.frame_gen) rt_log("settings: frame_generation=%dx (DLSS Frame Generation)", settings.frame_gen);
}

void settings_load(const char *dir) {
    char path[600], line[256];
    snprintf(path, sizeof path, "%s\\tfx.ini", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) { fputs(default_ini, f); fclose(f); }
        rt_log("settings: wrote defaults to %s", path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == ';' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(p), *v = trim(eq + 1);
        settings_set(k, v);
    }
    fclose(f);
    rt_log("settings: window %dx%d fullscreen %d aspect %d scaling %d framerate %s", settings.window_w,
           settings.window_h, settings.fullscreen, settings.aspect, settings.scaling,
           settings.framerate_high ? "high" : "normal");
    settings_resolve();
}
