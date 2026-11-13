/* Port settings (tfx.ini), see settings.c */
#ifndef SETTINGS_H
#define SETTINGS_H

enum { ASPECT_4_3, ASPECT_SQUARE, ASPECT_FILL };
enum { SCALE_SHARP, SCALE_PIXEL, SCALE_SMOOTH };
enum { RENDERER_GAME, RENDERER_NATIVE, RENDERER_WIREFRAME };
enum { UPSCALER_OFF, UPSCALER_FSR, UPSCALER_DLSS, UPSCALER_TAA };
enum { PRESENTER_D3D12, PRESENTER_SDL };

struct port_settings {
    int window_w, window_h, fullscreen;
    int aspect, scaling;
    int framerate_high;
    int renderer;             /* game | native (GPU renderer, RENDERER.md) | wireframe (capture debug view) */
    int upscaler;             /* off | fsr | dlss (native renderer only) */
    int dlss5;
    int frame_gen;            /* DLSS Frame Generation: 0 off, else the multiplier (2 = on, 3x, 4x) */
    int presenter;            /* d3d12 (default, falls back to sdl) | sdl */
    int widescreen;           /* native renderer: 3D world across the whole window width (RENDERER.md, stage 6) */
    int widescreen_cockpit;   /* sides: 0 = the world (default), 1 = cockpit panel / status bar continued */
    int ui_filter;            /* native 3D views, the game's cockpit/UI layer: 0 = as `scaling`, 1 = xBR, 2 = MMPX pixel-art upscaling, 3 = MMPX + the art pack (art.h) */
    int render_scale;         /* upscaler render resolution, percent of the output (50..100) */
    int joystick;             /* 1 = use the first game controller or joystick (joy.c), 0 = off */
    int head_tracking;        /* 1 = the virtual cockpit follows a FreeTrack head tracker (track.c), 0 = off */
};
extern struct port_settings settings;
void settings_load(const char *dir);
void settings_set(const char *key, const char *value);
void settings_resolve(void);

#endif
