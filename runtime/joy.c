/* Game controllers and joysticks (tfx.ini: joystick=, joy_*).
 *
 * The game reads an analogue joystick on the PC game port, 201h
 * (TFX_CODE.md 9.22): four one-shot timers for the axes, which it measures
 * by counting the polls until each bit drops, and four buttons.  One of the
 * sticks it knows is the CH Flightstick Pro: stick, throttle wheel on the
 * fourth axis, four buttons, and a hat that the stick reports as button
 * combinations.  This file presents the first SDL game controller or
 * joystick as that stick:
 *   - port 201h: the axis bits stay high for a number of polls that follows
 *     the stick position, so the game's own measuring, calibration, response
 *     curve and scale setting all apply;
 *   - rt_joy_frame(), a hook at the entry of the game's per-frame input
 *     routine 0x415DF4: switches the game to joystick control while a device
 *     is there, and to the keyboard when it isn't; keeps the calibration the
 *     emulated port needs in the game's variables (TFX.CFG isn't touched);
 *     writes the rudder, when an axis is assigned to it (joy_axes), where
 *     the game's rudder-pedal code would;
 *   - any button or hat direction can be bound to one of the game's four
 *     joystick buttons, a look direction (the game's hat) or a key press,
 *     with a modifier if wanted (Alt+L); joyui.c is the window for it;
 *   - outside flight the stick moves the menus' mouse pointer.
 * A virtual device for tests comes from the command file (J, vga.c). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <SDL.h>
#include "rt.h"
#include "cpu.h"
#include "settings.h"
#include "joy.h"

/* ---- bindings ------------------------------------------------------------- */
/* What a control (a button or a hat direction) does:
 *   - nothing;
 *   - one of the game's four joystick buttons (1 fire, 2 air-to-ground
 *     weapons, 3 air-to-air weapons, 4 next target: the game's handlers for
 *     Space, Backspace, Enter and C);
 *   - one direction of the game's hat (the look-around views);
 *   - a key: XT scan code in bits 0-6, BIND_EXT = E0-prefixed, and a
 *     modifier key held around it in bits 16-24 (the same two fields). */
enum { BIND_NONE = 0, BIND_BUTTON = 0x1000, BIND_KEY = 0x2000, BIND_LOOK = 0x4000, BIND_EXT = 0x100 };

/* A device's controls: buttons 1-32, then its first hat's up, right, down,
 * left.  A game controller that is the main device has named ones instead. */
#define MAXDEV JOY_MAXDEV
#define MAXBTN 32
#define NCTL JOY_NCTL
enum { PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_BACK, PB_START, PB_LS, PB_RS, PB_LT, PB_RT,
       PB_DU, PB_DR, PB_DD, PB_DL, PB_N };
static const char *pb_name[PB_N] = {"a", "b", "x", "y", "lb", "rb", "back", "start", "ls", "rs", "lt", "rt",
                                    "dpad_up", "dpad_right", "dpad_down", "dpad_left"};
static const char *pb_label[PB_N] = {"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "Left stick click",
                                     "Right stick click", "Left trigger", "Right trigger", "D-pad up",
                                     "D-pad right", "D-pad down", "D-pad left"};
static const char *pb_default[PB_N] = {"button1", "button2", "button3", "button4", "B", "RCtrl", "F1", "P", "G", "W",
                                       "-", "=", "look_front", "look_right", "look_back", "look_left"};
static const char *hat_name[4] = {"up", "right", "down", "left"};
static const char *look_name[4] = {"look_front", "look_right", "look_back", "look_left"};

/* joy_* lines of tfx.ini as written; looked up when the devices are opened */
static struct { char k[28], v[24]; } cfg[160];
static int n_cfg;
static char axes_cfg[64], device_cfg[64];
static int deadzone_pct = -1;               /* -1: 10 for a controller, 4 for a joystick */

int sdl_to_xt(SDL_Scancode s, int *ext);    /* vga.c */

/* a key by SDL's name ("Space", "Return", "F1", "Keypad +", "Right Ctrl") or
 * a short form: XT scan code | BIND_EXT, 0 = unknown */
static int key_code(const char *s) {
    static const char *alias[][2] = {
        {"alt", "Left Alt"}, {"lalt", "Left Alt"}, {"ralt", "Right Alt"}, {"shift", "Left Shift"},
        {"lshift", "Left Shift"}, {"rshift", "Right Shift"}, {"ctrl", "Left Ctrl"}, {"lctrl", "Left Ctrl"},
        {"rctrl", "Right Ctrl"}, {"enter", "Return"}, {"esc", "Escape"}, {"ins", "Insert"}, {"del", "Delete"},
        {"pgup", "PageUp"}, {"pgdn", "PageDown"}, {"kp+", "Keypad +"}, {"kp-", "Keypad -"}, {"kp/", "Keypad /"}};
    while (*s == ' ') s++;
    char b[40];
    snprintf(b, sizeof b, "%s", s);
    for (char *e = b + strlen(b); e > b && e[-1] == ' ';) *--e = 0;
    const char *n = b;
    for (size_t i = 0; i < sizeof alias / sizeof alias[0]; i++) if (!_stricmp(b, alias[i][0])) n = alias[i][1];
    SDL_Scancode sc = SDL_GetScancodeFromName(n);
    int ext = 0, xt = sc != SDL_SCANCODE_UNKNOWN ? sdl_to_xt(sc, &ext) : 0;
    return xt ? xt | (ext ? BIND_EXT : 0) : 0;
}

/* "none", "button1".."button4", "look_front|right|back|left", "KEY" or
 * "MODIFIER+KEY"; -1 = not understood */
int joy_bind_code(const char *s) {
    if (!s || !*s || !_stricmp(s, "none")) return BIND_NONE;
    if (!_strnicmp(s, "button", 6) && s[6] >= '1' && s[6] <= '4' && !s[7]) return BIND_BUTTON | (s[6] - '1');
    for (int i = 0; i < 4; i++) if (!_stricmp(s, look_name[i])) return BIND_LOOK | (i + 1);
    char b[48];
    snprintf(b, sizeof b, "%s", s);
    char *plus = strchr(b + 1, '+');                    /* not a key's own "+" ("Keypad +", "kp+") */
    int mod = 0, key;
    if (plus && plus[1]) { *plus = 0; mod = key_code(b); key = key_code(plus + 1); if (!mod) key = 0; }
    else key = key_code(b);
    return key ? BIND_KEY | key | (mod << 16) : -1;
}

static int parse_bind(const char *s) {
    int b = joy_bind_code(s);
    if (b < 0) { rt_log("joystick: unknown key \"%s\" in a joy binding", s); return BIND_NONE; }
    return b;
}

/* joy... lines of tfx.ini (settings.c passes them on) */
int joy_setting(const char *k, const char *v) {
    if (!_stricmp(k, "joy_deadzone")) { deadzone_pct = atoi(v); return 1; }
    if (!_stricmp(k, "joy_axes")) { snprintf(axes_cfg, sizeof axes_cfg, "%s", v); return 1; }
    if (!_stricmp(k, "joy_device")) { snprintf(device_cfg, sizeof device_cfg, "%s", v); return 1; }
    if (n_cfg >= 160) return 0;
    for (int i = 0; i < n_cfg; i++) if (!_stricmp(cfg[i].k, k)) { snprintf(cfg[i].v, sizeof cfg[0].v, "%s", v); return 1; }
    snprintf(cfg[n_cfg].k, sizeof cfg[0].k, "%s", k);
    snprintf(cfg[n_cfg].v, sizeof cfg[0].v, "%s", v);
    n_cfg++;
    return 1;
}

static const char *cfg_find(const char *k) {
    for (int i = 0; i < n_cfg; i++) if (!_stricmp(cfg[i].k, k)) return cfg[i].v;
    return NULL;
}

/* ---- devices -------------------------------------------------------------- */
/* Every connected device is opened (a HOTAS is often two: stick and
 * throttle).  The main device gives the stick and the default buttons and
 * hat; the throttle can come from another one.  The rudder has no default
 * axis: the game only steers with it on the ground. */
static struct dev {
    SDL_Joystick *js;
    char name[64];
    int naxes, nbuttons, nhats;
    char val[NCTL][24];                     /* each control's binding as text */
    int bind[NCTL];
    uint8_t down[NCTL], told[NCTL];
} devs[MAXDEV];
static int ndev, mainx = -1;                /* mainx: index of the main device */
static SDL_GameController *pad;             /* the main device, when it is a game controller */
static struct { int dev, axis; } role[4];   /* x, y, rudder, throttle: device index and axis number (0 none, <0 reversed) */
static int inited, virt;

static struct {
    float x, y, rudder, throttle;           /* -1..1 (right, back, right positive); throttle 0..1 */
    int has_rudder, has_throttle;
    unsigned buttons;                       /* the game's buttons 1-4, bits 0-3 */
    int hat;                                /* 0 none, 1 up, 2 right, 3 down, 4 left */
} st;
static int newest = -1;                     /* the game button pressed last (the stick reports one at a time) */

static int present(void) { return virt || mainx >= 0; }

static void push_key(int code, int down) {
    if (code & BIND_EXT) kbd_push_scancode(0xe0);
    kbd_push_scancode((uint8_t)((code & 0x7f) | (down ? 0 : 0x80)));
}

static void act(int bind, int down) {
    if (bind & BIND_KEY) {
        int mod = (bind >> 16) & 0x1ff;
        if (mod && down) push_key(mod, 1);
        push_key(bind, down);
        if (mod && !down) push_key(mod, 0);
    } else if (bind & BIND_BUTTON) {
        int b = bind & 3;
        if (down) { st.buttons |= 1u << b; newest = b; }
        else {
            st.buttons &= ~(1u << b);
            if (newest == b) { newest = -1; for (int k = 0; k < 4; k++) if (st.buttons & (1u << k)) newest = k; }
        }
    } else if (bind & BIND_LOOK) {
        if (down) st.hat = bind & 7;
        else if (st.hat == (bind & 7)) st.hat = 0;
    }
}

/* ---- controls: what a device has, their names, state and settings --------- */
int joy_dev_count(void) { return ndev; }
int joy_dev_main(void) { return mainx; }
int joy_dev_is_pad(int d) { return pad && d == mainx; }
const char *joy_dev_name(int d) { return devs[d].name; }
int joy_dev_naxes(int d) { return devs[d].naxes; }
float joy_dev_axis(int d, int a) { return SDL_JoystickGetAxis(devs[d].js, a) / 32767.0f; }

int joy_ctl_exists(int d, int c) {
    if (d < 0 || d >= ndev || c < 0 || c >= NCTL) return 0;
    if (joy_dev_is_pad(d)) return c < PB_N;
    return c < MAXBTN ? c < devs[d].nbuttons : devs[d].nhats > 0;
}

void joy_ctl_label(int d, int c, char *b, int n) {
    if (joy_dev_is_pad(d)) snprintf(b, n, "%s", pb_label[c]);
    else if (c < MAXBTN) snprintf(b, n, "Button %d", c + 1);
    else snprintf(b, n, "Hat %s", hat_name[c - MAXBTN]);
}

/* the control's key in tfx.ini; other = 1: the main device's second form, with its number */
static void ctl_key(int d, int c, int other, char *b, int n) {
    char dn[8] = "";
    if (d != mainx || other) snprintf(dn, sizeof dn, "%d", d + 1);
    if (joy_dev_is_pad(d)) snprintf(b, n, "joy_%s", pb_name[c]);
    else if (c < MAXBTN) snprintf(b, n, "joy%s_button%d", dn, c + 1);
    else snprintf(b, n, "joy%s_hat_%s", dn, hat_name[c - MAXBTN]);
}
void joy_ctl_key(int d, int c, char *b, int n) { ctl_key(d, c, 0, b, n); }

const char *joy_ctl_default(int d, int c) {
    if (joy_dev_is_pad(d)) return pb_default[c];
    if (d != mainx) return "none";
    static const char *b4[4] = {"button1", "button2", "button3", "button4"};
    return c < 4 ? b4[c] : c >= MAXBTN ? look_name[c - MAXBTN] : "none";
}
const char *joy_ctl_value(int d, int c) { return devs[d].val[c]; }

int joy_ctl_down(int d, int c) {
    struct dev *v = &devs[d];
    if (joy_dev_is_pad(d)) {
        static const SDL_GameControllerButton sb[PB_N] = {
            SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_BACK,
            SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
            SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_DPAD_UP,
            SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT};
        if (c == PB_LT) return SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000;
        if (c == PB_RT) return SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
        return SDL_GameControllerGetButton(pad, sb[c]) != 0;
    }
    if (c < MAXBTN) return SDL_JoystickGetButton(v->js, c) != 0;
    static const Uint8 hm[4] = {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT};
    return (SDL_JoystickGetHat(v->js, 0) & hm[c - MAXBTN]) != 0;
}

void joy_role_get(int k, int *dev, int *axis) { *dev = role[k].dev; *axis = role[k].axis; }

static void close_all(void) {
    for (int d = 0; d < ndev; d++) {
        for (int i = 0; i < NCTL; i++) if (devs[d].down[i]) act(devs[d].bind[i], 0);
        SDL_JoystickClose(devs[d].js);
    }
    if (pad) SDL_GameControllerClose(pad);
    pad = NULL; ndev = 0; mainx = -1;
    memset(devs, 0, sizeof devs);
    memset(&st, 0, sizeof st);
    newest = -1;
}

static int has_word(const char *name, const char *w) {
    size_t n = strlen(w);
    for (; *name; name++) if (!_strnicmp(name, w, n)) return 1;
    return 0;
}
static int is_throttle(const struct dev *d) { return has_word(d->name, "throttle"); }
static int is_pedals(const struct dev *d) { return has_word(d->name, "pedal") || has_word(d->name, "rudder"); }

/* "[-][device.]axis": device as numbered in the log, default the main one */
static void parse_axis(const char *t, int k) {
    int rev = 0, a, d = 0;
    while (*t == ' ') t++;
    if (*t == '-') { rev = 1; t++; }
    a = atoi(t);
    const char *dot = strchr(t, '.'), *comma = strchr(t, ',');
    if (dot && (!comma || dot < comma)) { d = a; a = atoi(dot + 1); }
    role[k].dev = d >= 1 && d <= ndev ? d - 1 : mainx;
    role[k].axis = a < 0 || a > devs[role[k].dev].naxes ? 0 : rev ? -a : a;
}

static void open_all(void) {
    close_all();
    if (getenv("TFX_JOY_NODEV")) return;                /* tests: as if nothing were connected */
    int n = SDL_NumJoysticks(), sdl_index[MAXDEV];
    for (int i = 0; i < n && ndev < MAXDEV; i++) {
        SDL_Joystick *js = SDL_JoystickOpen(i);
        if (!js) continue;
        struct dev *d = &devs[ndev];
        d->js = js;
        snprintf(d->name, sizeof d->name, "%s", SDL_JoystickName(js) ? SDL_JoystickName(js) : "?");
        d->naxes = SDL_JoystickNumAxes(js); d->nbuttons = SDL_JoystickNumButtons(js); d->nhats = SDL_JoystickNumHats(js);
        if (d->nbuttons > MAXBTN) d->nbuttons = MAXBTN;
        sdl_index[ndev++] = i;
    }
    if (!ndev) return;
    /* the main device: joy_device (number or part of the name); else the
     * first joystick that isn't a throttle or pedals; else the first device */
    if (device_cfg[0]) {
        int k = atoi(device_cfg);
        if (k >= 1 && k <= ndev) mainx = k - 1;
        else for (int i = 0; i < ndev && mainx < 0; i++) if (has_word(devs[i].name, device_cfg)) mainx = i;
        if (mainx < 0) rt_log("joystick: joy_device=%s matches no device", device_cfg);
    }
    for (int i = 0; i < ndev && mainx < 0; i++)
        if (!SDL_IsGameController(sdl_index[i]) && !is_throttle(&devs[i]) && !is_pedals(&devs[i])) mainx = i;
    for (int i = 0; i < ndev && mainx < 0; i++) if (!is_throttle(&devs[i]) && !is_pedals(&devs[i])) mainx = i;
    if (mainx < 0) mainx = 0;
    if (SDL_IsGameController(sdl_index[mainx])) pad = SDL_GameControllerOpen(sdl_index[mainx]);
    /* axes */
    struct dev *m = &devs[mainx];
    memset(role, 0, sizeof role);
    for (int k = 0; k < 4; k++) role[k].dev = mainx;
    if (axes_cfg[0] && _stricmp(axes_cfg, "auto")) {
        const char *t = axes_cfg;
        for (int k = 0; k < 4 && t; k++) { parse_axis(t, k); t = strchr(t, ','); if (t) t++; }
    } else {
        role[0].axis = 1; role[1].axis = 2;
        if (!pad) role[3].axis = m->naxes >= 4 ? 4 : m->naxes == 3 ? 3 : 0;    /* the stick's own lever */
        for (int i = 0; i < ndev; i++) {                /* a separate throttle unit: its third axis (after a mini-stick's two) */
            if (i == mainx || !is_throttle(&devs[i]) || !devs[i].naxes) continue;
            role[3].dev = i; role[3].axis = devs[i].naxes >= 3 ? 3 : devs[i].naxes;
        }
    }
    /* controls: tfx.ini, else the defaults */
    for (int d = 0; d < ndev; d++)
        for (int c = 0; c < NCTL; c++) {
            if (!joy_ctl_exists(d, c)) continue;
            char k[28];
            ctl_key(d, c, 0, k, sizeof k);
            const char *v = cfg_find(k);
            if (!v && d == mainx) { ctl_key(d, c, 1, k, sizeof k); v = cfg_find(k); }
            snprintf(devs[d].val[c], sizeof devs[0].val[0], "%s", v ? v : joy_ctl_default(d, c));
            devs[d].bind[c] = parse_bind(devs[d].val[c]);
        }
    for (int i = 0; i < ndev; i++)
        rt_log("joystick: device %d%s: \"%s\", %d axes, %d buttons, %d hats", i + 1, i == mainx ? " (main)" : "",
               devs[i].name, devs[i].naxes, devs[i].nbuttons, devs[i].nhats);
    if (pad) rt_log("joystick: controller layout: left stick = pitch/roll, d-pad = look around, triggers = throttle keys");
    if (!pad || role[2].axis || role[3].axis)
        rt_log("joystick: axes (device.axis): x %d.%d, y %d.%d, rudder %d.%d, throttle %d.%d (0 = none; joy_axes in tfx.ini changes them)",
               role[0].dev + 1, role[0].axis, role[1].dev + 1, role[1].axis, role[2].dev + 1, role[2].axis,
               role[3].dev + 1, role[3].axis);
}

/* setup: open the devices whatever joystick= says; returns how many */
int joy_open(void) {
    if (!inited) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
            rt_log("joystick: SDL: %s", SDL_GetError());
            return 0;
        }
        inited = 1;
    }
    open_all();
    return ndev;
}

void joy_init(void) {
    if (!settings.joystick || inited) return;
    if (!joy_open()) rt_log("joystick: none connected (one plugged in later is picked up)");
}

/* called for every SDL event (vga.c): a device was plugged in or removed.
 * Returns 1 when the devices were opened again. */
int joy_event(const SDL_Event *ev) {
    if (!inited || (ev->type != SDL_JOYDEVICEADDED && ev->type != SDL_JOYDEVICEREMOVED)) return 0;
    int same = SDL_NumJoysticks() == ndev;
    if (ev->type == SDL_JOYDEVICEADDED && same) {       /* the devices found at start-up announce themselves too */
        for (int i = 0; i < ndev && same; i++) same = SDL_JoystickGetAttached(devs[i].js) == SDL_TRUE;
        if (same) return 0;
    }
    rt_log("joystick: devices changed (%d connected)", SDL_NumJoysticks());
    open_all();
    return 1;
}

static float dead(float v, float dz) {
    float a = fabsf(v);
    if (a <= dz) return 0;
    a = (a - dz) / (1.0f - dz);
    return v < 0 ? -fminf(a, 1.0f) : fminf(a, 1.0f);
}

static float axis_of(int k) {
    int a = role[k].axis;
    if (!a) return 0;
    float v = SDL_JoystickGetAxis(devs[role[k].dev].js, abs(a) - 1) / 32767.0f;
    return a < 0 ? -v : v;
}

/* Flight frames call rt_joy_frame; without one for a while the game is in
 * its menus, where the stick moves the mouse pointer. */
static Uint32 flight_ms;
static int in_flight(void) { return flight_ms && SDL_GetTicks() - flight_ms < 400; }

/* called from the event pump (vga.c): read the devices, press and release
 * what the controls are bound to, drive the menus' pointer */
void joy_poll(void) {
    static Uint32 last_ms;
    static double fx, fy;
    static int mouse_btn;
    if (!inited || !present()) return;
    Uint32 now = SDL_GetTicks();
    float dt = last_ms ? (now - last_ms) / 1000.0f : 0;
    last_ms = now;
    if (!virt) {
        float dz = (deadzone_pct >= 0 ? deadzone_pct : pad ? 10 : 4) / 100.0f;
        if (pad) {
            float x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
            float y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
            float r = sqrtf(x * x + y * y), k = r > dz ? fminf((r - dz) / (1.0f - dz), 1.0f) / r : 0;   /* round dead zone */
            st.x = x * k; st.y = y * k;
        } else {
            st.x = dead(axis_of(0), dz);
            st.y = dead(axis_of(1), dz);
        }
        st.has_rudder = role[2].axis != 0;
        st.rudder = dead(axis_of(2), dz < 0.08f ? 0.08f : dz);
        st.has_throttle = role[3].axis != 0;
        st.throttle = (1.0f - axis_of(3)) * 0.5f;       /* lever forward (the axis at its minimum) = full */
        for (int d = 0; d < ndev; d++) {
            struct dev *v = &devs[d];
            for (int c = 0; c < NCTL; c++) {
                if (!joy_ctl_exists(d, c)) continue;
                int down = joy_ctl_down(d, c);
                if (down == v->down[c]) continue;
                v->down[c] = (uint8_t)down;
                if (v->bind[c]) act(v->bind[c], down);
                else if (down && !v->told[c]) {
                    char k[28], l[32];
                    v->told[c] = 1;
                    joy_ctl_key(d, c, k, sizeof k); joy_ctl_label(d, c, l, sizeof l);
                    rt_log("joystick: device %d, %s pressed: not bound (tfx.ini: %s=..., or the controls window)", d + 1, l, k);
                }
            }
        }
    }
    /* menus: the stick is the mouse (one screen width a second at full
     * deflection), game buttons 1 and 2 its left and right button */
    if (!in_flight()) {
        int btn = ((st.buttons & 1) ? 1 : 0) | ((st.buttons & 2) ? 2 : 0);
        fx += st.x * fabsf(st.x) * 320.0 * dt;
        fy += st.y * fabsf(st.y) * 320.0 * dt;
        int dx = (int)fx, dy = (int)fy;
        fx -= dx; fy -= dy;
        if (dx || dy || btn != mouse_btn) { mouse_relative(dx, dy, btn); mouse_btn = btn; }
    } else if (mouse_btn) { mouse_relative(0, 0, 0); mouse_btn = 0; }
}

/* Command file: "J X Y RUDDER THROTTLE BUTTONS HAT" sets a virtual stick
 * (X, Y, RUDDER -1..1; THROTTLE 0..1 or -1 for none; BUTTONS = the game's
 * buttons 1-4 as bits; HAT 0-4), "J off" removes it. */
void joy_virtual(const char *args) {
    while (*args == ' ') args++;
    if (!_strnicmp(args, "off", 3)) { virt = 0; memset(&st, 0, sizeof st); newest = -1; rt_log("joystick: virtual stick off"); return; }
    float x = 0, y = 0, r = 0, t = -1;
    unsigned b = 0; int h = 0;
    sscanf(args, "%f %f %f %f %x %d", &x, &y, &r, &t, &b, &h);
    if (!virt) rt_log("joystick: virtual stick on");
    virt = 1;
    st.x = x; st.y = y; st.rudder = r; st.has_rudder = 1;
    st.has_throttle = t >= 0; st.throttle = t < 0 ? 0 : t > 1 ? 1 : t;
    st.buttons = b & 15; st.hat = h;
    newest = -1;
    for (int k = 0; k < 4; k++) if (st.buttons & (1u << k)) newest = k;
}

/* ---- the game port -------------------------------------------------------- */
/* An axis bit stays set for this many polls after the trigger write.  The
 * game adds up the polls of its last four reads and divides by 16, so its
 * position value is a quarter of the count: 400 at the centre, +-300.  Every
 * count stays below 4000, the poll limit of an uncalibrated TFX.CFG. */
#define AX_CENTRE 1600
#define AX_RANGE 1200
#define CAL_CENTRE (AX_CENTRE / 4)
#define CAL_RANGE (AX_RANGE / 4)
#define THR_MIN 200                         /* the game halves the fourth axis's count: 200 (full) .. 1200 (idle) */
#define THR_MAX 1200
static int rem[4];
static unsigned port_reads, port_writes;

void joy_port_write(void) {
    port_writes++;
    if (!present()) { rem[0] = rem[1] = rem[2] = rem[3] = 0; return; }
    /* +1: the game discards the first read after the trigger */
    rem[0] = AX_CENTRE + (int)lrintf(st.x * AX_RANGE) + 1;
    rem[1] = AX_CENTRE + (int)lrintf(st.y * AX_RANGE) + 1;
    rem[2] = 2;
    rem[3] = (st.has_throttle ? 2 * (THR_MIN + (int)lrintf((1.0f - st.throttle) * (THR_MAX - THR_MIN))) : 2) + 1;
}

uint8_t joy_port_read(void) {
    /* The Flightstick Pro's hat arrives as button combinations, and the
     * game's table has an action for a single button only: of several held,
     * the one pressed last is reported. */
    static const uint8_t hat_bits[5] = {0, 0xf, 0xb, 0x7, 0x3};      /* up 1234, right 124, down 123, left 12 */
    port_reads++;
    if (!present()) return 0xf0;            /* no axis ever times: no stick */
    unsigned b = st.hat ? hat_bits[st.hat] : newest >= 0 ? 1u << newest : 0;
    uint8_t v = (uint8_t)((~b & 15) << 4);  /* pressed = 0 */
    for (int i = 0; i < 4; i++) if (rem[i] > 0) { v |= (uint8_t)(1 << i); rem[i]--; }
    return v;
}

/* ---- the game side -------------------------------------------------------- */
/* Hook at the entry of 0x415DF4, the flight loop's per-frame input routine.
 * Game variables (TFX_CODE.md 9.22): [4A3478h] control method (0 keyboard,
 * 1 joystick, 2 mouse) with its copy for the options screen [4A3AEDh];
 * [4A3267h] joystick type (1 = Flightstick Pro); [49AE6Ch] the reader's axis
 * mask; [49AE68h] its poll limit; [4B0C6Ah]/[4B0C68h] x centre and range,
 * [4B0C60h]/[4B0C66h] y; [4A3254h]/[4A3252h] the throttle's ends;
 * [4A48F0h] the rudder (double, -1..1), as the game's rudder-pedal code
 * writes it. */
static unsigned frames_on, switched_on, switched_off;

void rt_joy_frame(void) {
    static int applied, saved, rudder_live;
    flight_ms = SDL_GetTicks() | 1;
    if (!settings.joystick) return;
    if (!present()) {
        if (RB(0x4a3478) == 1) {            /* no stick can answer: the keyboard (or what it was before) */
            uint8_t c = applied && saved != 1 ? (uint8_t)saved : 0;
            WB(0x4a3478, c); WB(0x4a3aed, c);
            WW(0x49ac60, 0); WW(0x49ac62, 0);
            switched_off++;
            rt_log("joystick: none connected, flight controls back to %s", c == 2 ? "the mouse" : "the keyboard");
        }
        applied = 0;
        return;
    }
    if (!applied) {
        applied = 1;
        saved = RB(0x4a3478);
        WB(0x4a3478, 1); WB(0x4a3aed, 1);
        switched_on++;
        rt_log("joystick: flight controls set to the joystick (the game's options screen can change it)");
    }
    frames_on++;
    WB(0x4a3267, 1);                        /* a Flightstick Pro */
    WD(0x49ae6c, 0xb000);                   /* wait for axes 1, 2 and 4 */
    WD(0x49ae68, 8000);
    WW(0x4b0c6a, CAL_CENTRE); WW(0x4b0c68, CAL_RANGE);
    WW(0x4b0c60, CAL_CENTRE); WW(0x4b0c66, CAL_RANGE);
    WW(0x4a3254, st.has_throttle ? THR_MIN : 0);
    WW(0x4a3252, st.has_throttle ? THR_MAX : 0);
    /* rudder: only while the axis is off centre, so the keyboard's rudder
     * keys keep working */
    if (st.has_rudder && (st.rudder != 0 || rudder_live)) {
        WF64(0x4a48f0, (double)st.rudder);
        rudder_live = st.rudder != 0;
    }
}

void joy_log(void) {
    rt_log("joystick: %s; x %.2f y %.2f rudder %.2f throttle %.2f buttons %x hat %d; port 201h %u writes %u reads; "
           "game: control %u, stick x %d y %d, rudder %.2f, throttle %.2f; %u frames on, switched on %u off %u",
           virt ? "virtual" : pad ? "controller" : mainx >= 0 ? "joystick" : "none", st.x, st.y, st.rudder, st.throttle,
           st.buttons, st.hat, port_writes, port_reads, RB(0x4a3478), (int16_t)RW(0x49ac60), (int16_t)RW(0x49ac62),
           RF64(0x4a48f0), RF32(0x4a33f8), frames_on, switched_on, switched_off);
    port_writes = port_reads = 0;
}
