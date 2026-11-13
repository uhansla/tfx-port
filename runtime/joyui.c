/* The controls window: tfx.exe --controls (controls.bat).
 *
 * Lists the game's controls from the manual; click one and press a button or
 * a hat direction on any connected device to bind it.  The axes (roll, pitch,
 * throttle, rudder) are set by moving them.  Save writes the joy... lines of
 * tfx.ini (joy.c reads them); everything else in the file is kept.
 *
 * An SDL window for the input (the same device list and numbers as in the
 * game), drawn with GDI for its fonts.
 *
 * Tests: TFX_CONTROLS_TEST="cmd;cmd;..." runs "click X Y" (window units),
 * "press DEV CTL" (as if that control were pressed; 1-based, hat = 33..36),
 * "key up|down|enter|del|esc", "save", "shot FILE.bmp", "quit", one a frame. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <windows.h>
#include "rt.h"
#include "settings.h"
#include "joy.h"

/* ---- the game's controls (the manual's key list) --------------------------- */
struct action { const char *group, *label, *value, *alt; };
static const struct action actions[] = {
    {"Weapons", "Fire (in the menus: click)", "button1", "Space"},
    {"Weapons", "Air-to-air weapons", "button3", "Return"},
    {"Weapons", "Air-to-ground weapons", "button2", "Backspace"},
    {"Weapons", "Cannons", "RCtrl", NULL},
    {"Weapons", "Next target", "button4", "C"},
    {"Weapons", "Break lock", "X", NULL},
    {"Weapons", "Laser target designator", "Z", NULL},
    {"Weapons", "Jettison tank and stores", "Alt+J", NULL},
    {"Defence", "Chaff", "Insert", NULL},
    {"Defence", "Flares", "Delete", NULL},
    {"Defence", "ECM", "E", NULL},
    {"Defence", "Stealth mode", "S", NULL},
    {"Engines", "Throttle up (hold)", "=", NULL},
    {"Engines", "Throttle down (hold)", "-", NULL},
    {"Engines", "Afterburner up a stage", "Keypad +", NULL},
    {"Engines", "Afterburner down a stage", "Keypad -", NULL},
    {"Engines", "Afterburner off", "Keypad /", NULL},
    {"Engines", "Engine 1 on/off", "[", NULL},
    {"Engines", "Engine 2 on/off", "]", NULL},
    {"Flight", "Air brake", "B", NULL},
    {"Flight", "Wheel brakes", "W", NULL},
    {"Flight", "Landing gear", "G", NULL},
    {"Flight", "Brake chute: deploy", "Alt+B", NULL},
    {"Flight", "Brake chute: release", "LShift+B", NULL},
    {"Flight", "Autopilot on/off", "A", NULL},
    {"Flight", "Auto land", "Alt+L", NULL},
    {"Flight", "Refuel access", "'", NULL},
    {"Look", "Look ahead", "look_front", NULL},
    {"Look", "Look left", "look_left", NULL},
    {"Look", "Look right", "look_right", NULL},
    {"Look", "Look behind", "look_back", NULL},
    {"Views", "Cockpit (F1)", "F1", NULL},
    {"Views", "Swing left (F2)", "F2", NULL},
    {"Views", "Swing right (F3)", "F3", NULL},
    {"Views", "Behind you (F4)", "F4", NULL},
    {"Views", "Forward, no instruments", "RShift+F1", NULL},
    {"Views", "Chase (F5)", "F5", NULL},
    {"Views", "Fly-past (F6)", "F6", NULL},
    {"Views", "You and the enemy (F7)", "F7", NULL},
    {"Views", "Weapon (F8)", "F8", NULL},
    {"Views", "Virtual cockpit (F10)", "F10", NULL},
    {"Views", "Cockpit up", "PageUp", NULL},
    {"Views", "Cockpit down", "PageDown", NULL},
    {"Displays", "Map", "M", NULL},
    {"Displays", "MFD 1", "1", NULL},
    {"Displays", "MFD 2", "2", NULL},
    {"Displays", "MFD 3", "3", NULL},
    {"Displays", "Last message", "L", NULL},
    {"Displays", "Flight details", "D", NULL},
    {"Displays", "Night sight", "LShift+N", NULL},
    {"Displays", "Weather report", "Alt+W", NULL},
    {"Other", "Pause", "P", NULL},
    {"Other", "Accelerated time", "T", NULL},
    {"Other", "Video recorder", "Alt+V", NULL},
    {"Other", "Eject (press twice)", "Escape", NULL},
    {"Other", "Quit the mission", "LShift+Q", NULL},
};
#define NACT ((int)(sizeof actions / sizeof actions[0]))

/* ---- the model: what is being edited --------------------------------------- */
static char val[JOY_MAXDEV][JOY_NCTL][24];
static int vcode[JOY_MAXDEV][JOY_NCTL];
static int role_dev[4], role_axis[4];       /* roll, pitch, rudder, throttle (joy.c's order) */
static int dirty;
static char status[200] = "Click a control, then press the button or hat you want for it.";

/* rows of the list: group headings, the actions, and bindings to other keys */
struct row { int header; char label[40], value[24]; int code, alt; };
static struct row rows[200];
static int nrows, sel = -1, capture, scroll;
static int cap_axis = -1;
static float axis_base[JOY_MAXDEV][16];
static uint8_t prev_down[JOY_MAXDEV][JOY_NCTL];

static void set_val(int d, int c, const char *v) {
    snprintf(val[d][c], sizeof val[0][0], "%s", v);
    vcode[d][c] = joy_bind_code(v);
}

static int row_has(const struct row *r, int code) { return code > 0 && (code == r->code || code == r->alt); }

static void build_rows(void) {
    const char *g = "";
    nrows = 0;
    for (int i = 0; i < NACT; i++) {
        if (strcmp(g, actions[i].group)) {
            g = actions[i].group;
            memset(&rows[nrows], 0, sizeof rows[0]);
            rows[nrows].header = 1;
            snprintf(rows[nrows++].label, sizeof rows[0].label, "%s", g);
        }
        struct row *r = &rows[nrows++];
        memset(r, 0, sizeof *r);
        snprintf(r->label, sizeof r->label, "%s", actions[i].label);
        snprintf(r->value, sizeof r->value, "%s", actions[i].value);
        r->code = joy_bind_code(actions[i].value);
        r->alt = actions[i].alt ? joy_bind_code(actions[i].alt) : 0;
    }
    /* bindings from tfx.ini to keys that aren't in the list */
    int first = 1;
    for (int d = 0; d < joy_dev_count(); d++)
        for (int c = 0; c < JOY_NCTL; c++) {
            if (!joy_ctl_exists(d, c) || vcode[d][c] <= 0) continue;
            int known = 0;
            for (int i = 0; i < nrows && !known; i++) known = !rows[i].header && row_has(&rows[i], vcode[d][c]);
            if (known || nrows >= 198) continue;
            if (first) {
                memset(&rows[nrows], 0, sizeof rows[0]);
                rows[nrows].header = 1;
                snprintf(rows[nrows++].label, sizeof rows[0].label, "Other keys (from tfx.ini)");
                first = 0;
            }
            struct row *r = &rows[nrows++];
            memset(r, 0, sizeof *r);
            snprintf(r->label, sizeof r->label, "Key %s", val[d][c]);
            snprintf(r->value, sizeof r->value, "%s", val[d][c]);
            r->code = vcode[d][c];
        }
}

static void load_model(void) {
    memset(val, 0, sizeof val); memset(vcode, 0, sizeof vcode);
    for (int d = 0; d < joy_dev_count(); d++)
        for (int c = 0; c < JOY_NCTL; c++) {
            if (joy_ctl_exists(d, c)) set_val(d, c, joy_ctl_value(d, c));
            prev_down[d][c] = joy_ctl_exists(d, c) ? (uint8_t)joy_ctl_down(d, c) : 0;
        }
    for (int k = 0; k < 4; k++) joy_role_get(k, &role_dev[k], &role_axis[k]);
    build_rows();
    sel = -1; capture = 0; cap_axis = -1;
}

static void ctl_text(int d, int c, char *b, int n) {
    char l[32];
    joy_ctl_label(d, c, l, sizeof l);
    snprintf(b, n, "D%d %s", d + 1, l);
}

/* ---- saving ---------------------------------------------------------------- */
static int is_ctl_line(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    if (_strnicmp(p, "joy", 3) || !_strnicmp(p, "joystick", 8) || !_strnicmp(p, "joy_device", 10) ||
        !_strnicmp(p, "joy_deadzone", 12))
        return 0;
    return strchr(p, '=') != NULL;
}

static int save_ini(const char *dir) {
    char path[600], tmp[600];
    snprintf(path, sizeof path, "%s\\tfx.ini", dir);
    snprintf(tmp, sizeof tmp, "%s\\tfx.ini.new", dir);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return 0; }
    /* the file without its control lines and the comments right above them */
    static char pend[64][256];
    int np = 0, blank = 0;
    char line[256];
    while (in && fgets(line, sizeof line, in)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '#') { if (np < 64) snprintf(pend[np++], sizeof pend[0], "%s", line); continue; }
        if (is_ctl_line(p)) { np = 0; continue; }
        for (int i = 0; i < np; i++) fputs(pend[i], out);
        np = 0;
        if (*p == '\n' || *p == '\r' || !*p) { if (blank) continue; blank = 1; fputs("\n", out); continue; }
        blank = 0;
        fputs(line, out);
        if (!strchr(line, '\n')) fputs("\n", out);
    }
    for (int i = 0; i < np; i++) fputs(pend[i], out);
    if (in) fclose(in);
    if (!blank) fputs("\n", out);
    fputs("; joystick controls, written by the controls window (controls.bat):\n"
          "; axes = roll, pitch, rudder, throttle as device.axis (0 none, - reversed)\n", out);
    fputs("joy_axes=", out);
    for (int k = 0; k < 4; k++) {
        if (!role_axis[k]) fputs("0", out);
        else fprintf(out, "%s%d.%d", role_axis[k] < 0 ? "-" : "", role_dev[k] + 1, abs(role_axis[k]));
        fputs(k < 3 ? "," : "\n", out);
    }
    for (int d = 0; d < joy_dev_count(); d++) {
        fprintf(out, "; device %d: %s\n", d + 1, joy_dev_name(d));
        for (int c = 0; c < JOY_NCTL; c++) {
            if (!joy_ctl_exists(d, c)) continue;
            const char *v = val[d][c][0] ? val[d][c] : "none";
            if (!_stricmp(v, "none") && !_stricmp(joy_ctl_default(d, c), "none")) continue;
            char k[32];
            joy_ctl_key(d, c, k, sizeof k);
            fprintf(out, "%s=%s\n", k, v);
        }
    }
    fclose(out);
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { remove(tmp); return 0; }
    return 1;
}

/* ---- drawing --------------------------------------------------------------- */
#define WIN_W 1180
#define WIN_H 800
static float S = 1.0f;                      /* pixels per window unit (the monitor's scaling) */
static HDC mem;
static HFONT f_norm, f_bold, f_small;
#define PX(v) ((int)lrintf((float)(v) * S))

enum { C_BG = 0x231e1b, C_PANEL = 0x2f2824, C_TEXT = 0xe6e6e6, C_DIM = 0x9c918a, C_ACC = 0xffc24c, C_SEL = 0x4c3b2f,
       C_CAP = 0x0a6aa3, C_BTN = 0x4a4038, C_ON = 0x50d060, C_LINE = 0x4a4038, C_WARN = 0x40b0ff };   /* 0xBBGGRR */

static void fill(int x, int y, int w, int h, COLORREF c) {
    RECT r = {PX(x), PX(y), PX(x + w), PX(y + h)};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(mem, &r, b);
    DeleteObject(b);
}
static void frame(int x, int y, int w, int h, COLORREF c) {
    RECT r = {PX(x), PX(y), PX(x + w), PX(y + h)};
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(mem, &r, b);
    DeleteObject(b);
}
/* text in a box: align 0 left, 1 centre, 2 right; clipped, with an ellipsis */
static void text(int x, int y, int w, int h, const char *s, COLORREF c, HFONT f, int align) {
    RECT r = {PX(x), PX(y), PX(x + w), PX(y + h)};
    SelectObject(mem, f);
    SetTextColor(mem, c);
    DrawTextA(mem, s, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS |
                                  (align == 1 ? DT_CENTER : align == 2 ? DT_RIGHT : DT_LEFT));
}
static int inside(int mx, int my, int x, int y, int w, int h) { return mx >= x && mx < x + w && my >= y && my < y + h; }

/* layout, window units */
enum { LIST_X = 16, LIST_Y = 60, LIST_W = 660, ROW_H = 24, SIDE_X = 692, SIDE_W = 472, BAR_H = 56,
       AX_Y = 60, AX_ROW = 30, DEV_Y = AX_Y + 30 + 4 * AX_ROW + 14 };
#define LIST_H (WIN_H - LIST_Y - BAR_H)
static const char *role_label[4] = {"Roll", "Pitch", "Rudder", "Throttle"};
static const int role_order[4] = {0, 1, 3, 2};      /* shown: roll, pitch, throttle, rudder */
static int ptr_x, ptr_y;

static void button(int x, int y, int w, int h, const char *s, int enabled) {
    int hot = enabled && inside(ptr_x, ptr_y, x, y, w, h);
    fill(x, y, w, h, hot ? C_SEL : C_BTN);
    text(x, y, w, h, s, enabled ? C_TEXT : C_DIM, f_norm, 1);
}

static float role_value(int k) {
    if (!role_axis[k]) return 0;
    float v = joy_dev_axis(role_dev[k], abs(role_axis[k]) - 1);
    return role_axis[k] < 0 ? -v : v;
}

/* the squares of a device's controls: position of control c, 0 when it has none */
static int ctl_box(int d, int c, int y0, int *x, int *y) {
    if (!joy_ctl_exists(d, c)) return 0;
    int i = c;
    if (!joy_dev_is_pad(d) && c >= 32) {    /* the hat follows the buttons */
        int nb = 0;
        while (nb < 32 && joy_ctl_exists(d, nb)) nb++;
        i = nb + (c - 32);
    }
    *x = SIDE_X + 12 + (i % 18) * 25;
    *y = y0 + (i / 18) * 25;
    return 1;
}
static int dev_rows(int d) {
    int n = 0;
    for (int c = 0; c < JOY_NCTL; c++) n += joy_ctl_exists(d, c);
    return (n + 17) / 18;
}
static int dev_height(int d) { return 22 + 20 + dev_rows(d) * 25 + 12; }

static void draw(HDC wdc, int pw, int ph) {
    fill(0, 0, WIN_W, WIN_H, C_BG);
    SetBkMode(mem, TRANSPARENT);
    text(16, 10, 400, 26, "TFX controls", C_TEXT, f_bold, 0);
    text(16, 34, 900, 20, capture ? "Press the button or hat direction for the highlighted control.   Esc = cancel"
                         : cap_axis >= 0 ? "Move the axis as described below.   Esc = cancel"
                         : "Click a control to bind it.   Del = remove its buttons.   Pressing a button shows what it does.",
         capture || cap_axis >= 0 ? C_WARN : C_DIM, f_norm, 0);

    /* the list */
    fill(LIST_X, LIST_Y, LIST_W, LIST_H, C_PANEL);
    HRGN clip = CreateRectRgn(PX(LIST_X), PX(LIST_Y), PX(LIST_X + LIST_W), PX(LIST_Y + LIST_H));
    SelectClipRgn(mem, clip);
    for (int i = 0; i < nrows; i++) {
        int y = LIST_Y + 4 + i * ROW_H - scroll;
        if (y + ROW_H < LIST_Y || y > LIST_Y + LIST_H) continue;
        const struct row *r = &rows[i];
        if (r->header) { text(LIST_X + 10, y + 2, 300, ROW_H, r->label, C_ACC, f_bold, 0); continue; }
        if (i == sel) fill(LIST_X + 2, y, LIST_W - 4, ROW_H, capture ? C_CAP : C_SEL);
        text(LIST_X + 22, y, 226, ROW_H, r->label, C_TEXT, f_norm, 0);
        text(LIST_X + 250, y, 92, ROW_H, !_strnicmp(r->value, "button", 6) || !_strnicmp(r->value, "look_", 5) ? "" : r->value,
             C_DIM, f_small, 0);
        char b[200] = "";
        for (int d = 0; d < joy_dev_count(); d++)
            for (int c = 0; c < JOY_NCTL; c++)
                if (joy_ctl_exists(d, c) && row_has(r, vcode[d][c]) && strlen(b) < 150) {
                    char t[48];
                    ctl_text(d, c, t, sizeof t);
                    if (b[0]) strcat(b, ",  ");
                    strcat(b, t);
                }
        text(LIST_X + 346, y, LIST_W - 356, ROW_H, b[0] ? b : (i == sel && capture ? "press a button..." : "-"),
             b[0] ? C_ON : C_DIM, f_norm, 0);
    }
    SelectClipRgn(mem, NULL);
    DeleteObject(clip);

    /* the axes */
    fill(SIDE_X, AX_Y, SIDE_W, 30 + 4 * AX_ROW + 6, C_PANEL);
    text(SIDE_X + 10, AX_Y + 4, 200, 24, "Axes", C_ACC, f_bold, 0);
    int padmain = joy_dev_main() >= 0 && joy_dev_is_pad(joy_dev_main());
    for (int n = 0; n < 4; n++) {
        int k = role_order[n], y = AX_Y + 30 + n * AX_ROW;
        char b[64];
        if (cap_axis == k) fill(SIDE_X + 2, y, SIDE_W - 4, AX_ROW - 2, C_CAP);
        text(SIDE_X + 10, y, 70, AX_ROW - 2, role_label[k], C_TEXT, f_norm, 0);
        if (cap_axis == k)
            snprintf(b, sizeof b, "%s", k == 0 ? "move the stick right" : k == 1 ? "pull the stick back" :
                                        k == 3 ? "move it, hold at full" : "push the right pedal");
        else if (padmain && k < 2) snprintf(b, sizeof b, "left stick");
        else if (!role_axis[k]) snprintf(b, sizeof b, "-");
        else snprintf(b, sizeof b, "D%d axis %d%s", role_dev[k] + 1, abs(role_axis[k]), role_axis[k] < 0 ? ", reversed" : "");
        text(SIDE_X + 82, y, 150, AX_ROW - 2, b, cap_axis == k ? C_TEXT : role_axis[k] ? C_ON : C_DIM, f_norm, 0);
        int bx = SIDE_X + 236, bw = 96, by = y + 9, bh = 10;
        fill(bx, by, bw, bh, C_BTN);
        if (role_axis[k] && !(padmain && k < 2)) {
            float v = role_value(k);
            if (k == 3) fill(bx, by, (int)(bw * (1.0f - v) * 0.5f), bh, C_ACC);
            else {
                int m = bx + bw / 2, e = m + (int)(v * bw / 2);
                fill(e < m ? e : m, by, abs(e - m) + 1, bh, C_ACC);
            }
        }
        int can = !(padmain && k < 2);
        button(SIDE_X + 340, y + 3, 44, AX_ROW - 8, "Set", can);
        button(SIDE_X + 388, y + 3, 44, AX_ROW - 8, "Rev", can && role_axis[k]);
        button(SIDE_X + 436, y + 3, 28, AX_ROW - 8, "X", k >= 2 && role_axis[k]);
    }

    /* the devices */
    int y = DEV_Y;
    if (!joy_dev_count()) text(SIDE_X + 10, y, SIDE_W - 20, 24, "No joystick or controller is connected.", C_WARN, f_norm, 0);
    for (int d = 0; d < joy_dev_count(); d++) {
        int h = dev_height(d);
        char b[120];
        fill(SIDE_X, y, SIDE_W, h - 6, C_PANEL);
        snprintf(b, sizeof b, "D%d   %s%s", d + 1, joy_dev_name(d), d == joy_dev_main() ? "   (main)" : "");
        text(SIDE_X + 10, y + 2, SIDE_W - 20, 22, b, C_TEXT, f_bold, 0);
        for (int a = 0; a < joy_dev_naxes(d) && a < 8; a++) {       /* the axes, live */
            int bx = SIDE_X + 12 + a * 56, by = y + 30, bw = 36;
            float v = joy_dev_axis(d, a);
            snprintf(b, sizeof b, "%d", a + 1);
            text(bx - 2, by - 5, 12, 16, b, C_DIM, f_small, 0);
            fill(bx + 10, by, bw, 6, C_BTN);
            int m = bx + 10 + bw / 2, e = m + (int)(v * bw / 2);
            fill(e < m ? e : m, by, abs(e - m) + 1, 6, C_ACC);
        }
        for (int c = 0; c < JOY_NCTL; c++) {
            int bx, by;
            if (!ctl_box(d, c, y + 44, &bx, &by)) continue;
            int down = joy_ctl_down(d, c), bound = vcode[d][c] > 0;
            fill(bx, by, 22, 22, down ? C_ON : C_BTN);
            if (bound) frame(bx, by, 22, 22, C_ACC);
            if (joy_dev_is_pad(d)) {
                static const char *sh[16] = {"A", "B", "X", "Y", "LB", "RB", "Bk", "St", "LS", "RS", "LT", "RT", "^", ">", "v", "<"};
                snprintf(b, sizeof b, "%s", sh[c]);
            } else if (c < 32) snprintf(b, sizeof b, "%d", c + 1);
            else snprintf(b, sizeof b, "%s", c == 32 ? "^" : c == 33 ? ">" : c == 34 ? "v" : "<");
            text(bx, by, 22, 22, b, down ? 0x000000 : bound ? C_TEXT : C_DIM, f_small, 1);
        }
        y += h;
    }

    /* the bottom bar */
    fill(0, WIN_H - BAR_H, WIN_W, BAR_H, C_PANEL);
    text(16, WIN_H - BAR_H, 760, BAR_H, status, C_TEXT, f_norm, 0);
    button(WIN_W - 396, WIN_H - 44, 120, 32, dirty ? "Save *" : "Save", 1);
    button(WIN_W - 268, WIN_H - 44, 120, 32, "Default buttons", 1);
    button(WIN_W - 140, WIN_H - 44, 120, 32, "Close", 1);
    BitBlt(wdc, 0, 0, pw, ph, mem, 0, 0, SRCCOPY);
}

static void save_shot(const char *path, int pw, int ph) {
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = pw; bi.bmiHeader.biHeight = ph;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
    int stride = (pw * 3 + 3) & ~3;
    uint8_t *px = malloc((size_t)stride * ph);
    HBITMAP bmp = (HBITMAP)GetCurrentObject(mem, OBJ_BITMAP);
    HDC sdc = GetDC(NULL);
    HBITMAP old = SelectObject(mem, CreateCompatibleBitmap(sdc, 1, 1));       /* GetDIBits wants it deselected */
    GetDIBits(sdc, bmp, 0, ph, px, &bi, DIB_RGB_COLORS);
    DeleteObject(SelectObject(mem, bmp));
    (void)old;
    ReleaseDC(NULL, sdc);
    BITMAPFILEHEADER fh = {0};
    fh.bfType = 0x4d42; fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader; fh.bfSize = fh.bfOffBits + stride * ph;
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(&fh, sizeof fh, 1, f); fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f); fwrite(px, stride, ph, f); fclose(f); }
    free(px);
}

/* ---- input ----------------------------------------------------------------- */
static const char *ini_dir;
static int running = 1, close_asked;

static void select_row(int i) {
    sel = i;
    int y = 4 + i * ROW_H;
    if (y < scroll + ROW_H) scroll = y - ROW_H;
    if (y + ROW_H > scroll + LIST_H - 4) scroll = y + ROW_H - LIST_H + 4;
    int max = 8 + nrows * ROW_H - LIST_H;
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
}

static void clear_row(int i) {
    int n = 0;
    for (int d = 0; d < joy_dev_count(); d++)
        for (int c = 0; c < JOY_NCTL; c++)
            if (joy_ctl_exists(d, c) && row_has(&rows[i], vcode[d][c])) { set_val(d, c, "none"); n++; }
    if (n) dirty = 1;
    snprintf(status, sizeof status, n ? "%s: %d button(s) removed." : "%s has no buttons.", rows[i].label, n);
}

static void do_save(void) {
    if (save_ini(ini_dir)) { dirty = 0; snprintf(status, sizeof status, "Saved to tfx.ini. The game uses it the next time it starts."); }
    else snprintf(status, sizeof status, "Could not write tfx.ini.");
    close_asked = 0;
}

static int thr_d = -1, thr_a;                /* setting the throttle: the axis that moved, and how long it has been still */
static float thr_v;
static Uint32 thr_ms;

static void start_axis(int k) {
    capture = 0;
    cap_axis = k;
    thr_d = -1;
    for (int d = 0; d < joy_dev_count(); d++)
        for (int a = 0; a < joy_dev_naxes(d) && a < 16; a++) axis_base[d][a] = joy_dev_axis(d, a);
}

/* a control was pressed (d, c): bind it, or show what it does */
static void on_press(int d, int c) {
    char t[48];
    ctl_text(d, c, t, sizeof t);
    if (cap_axis >= 0) return;
    if (capture && sel >= 0) {
        set_val(d, c, rows[sel].value);
        dirty = 1; capture = 0;
        snprintf(status, sizeof status, "%s  =  %s", t, rows[sel].label);
        return;
    }
    for (int i = 0; i < nrows; i++)
        if (!rows[i].header && row_has(&rows[i], vcode[d][c])) {
            select_row(i);
            snprintf(status, sizeof status, "%s  =  %s", t, rows[i].label);
            return;
        }
    snprintf(status, sizeof status, "%s is not bound. Click a control, then press it.", t);
}

static void on_click(int mx, int my) {
    close_asked = 0;
    if (inside(mx, my, LIST_X, LIST_Y, LIST_W, LIST_H)) {
        int i = (my - LIST_Y - 4 + scroll) / ROW_H;
        cap_axis = -1;
        if (i >= 0 && i < nrows && !rows[i].header) {
            sel = i; capture = 1;
            snprintf(status, sizeof status, "%s: press a button or a hat direction on any device.", rows[i].label);
        } else capture = 0;
        return;
    }
    capture = 0;
    int padmain = joy_dev_main() >= 0 && joy_dev_is_pad(joy_dev_main());
    for (int n = 0; n < 4; n++) {
        int k = role_order[n], y = AX_Y + 30 + n * AX_ROW + 3, h = AX_ROW - 8;
        if (padmain && k < 2) continue;
        if (inside(mx, my, SIDE_X + 340, y, 44, h)) {
            if (!joy_dev_count()) { snprintf(status, sizeof status, "No device is connected."); return; }
            start_axis(k);
            snprintf(status, sizeof status, "%s: %s.", role_label[k], k == 0 ? "move the stick fully right" :
                     k == 1 ? "pull the stick fully back" : k == 3 ? "move the throttle, then hold it at full" :
                     "push the right pedal fully");
            return;
        }
        if (inside(mx, my, SIDE_X + 388, y, 44, h) && role_axis[k]) {
            role_axis[k] = -role_axis[k]; dirty = 1; cap_axis = -1;
            snprintf(status, sizeof status, "%s reversed.", role_label[k]);
            return;
        }
        if (inside(mx, my, SIDE_X + 436, y, 28, h) && k >= 2 && role_axis[k]) {
            role_axis[k] = 0; dirty = 1; cap_axis = -1;
            snprintf(status, sizeof status, "%s: no axis.", role_label[k]);
            return;
        }
    }
    cap_axis = -1;
    if (inside(mx, my, WIN_W - 396, WIN_H - 44, 120, 32)) do_save();
    else if (inside(mx, my, WIN_W - 268, WIN_H - 44, 120, 32)) {
        for (int d = 0; d < joy_dev_count(); d++)
            for (int c = 0; c < JOY_NCTL; c++) if (joy_ctl_exists(d, c)) set_val(d, c, joy_ctl_default(d, c));
        build_rows();
        dirty = 1; sel = -1;
        snprintf(status, sizeof status, "Buttons set to the defaults (not saved yet).");
    } else if (inside(mx, my, WIN_W - 140, WIN_H - 44, 120, 32)) {
        if (dirty && !close_asked) { close_asked = 1; snprintf(status, sizeof status, "Changes are not saved. Click Close again to drop them, or Save."); }
        else running = 0;
    }
}

static void on_key(const char *k) {
    if (!strcmp(k, "esc")) {
        if (capture || cap_axis >= 0) { capture = 0; cap_axis = -1; snprintf(status, sizeof status, "Cancelled."); }
        else if (dirty && !close_asked) { close_asked = 1; snprintf(status, sizeof status, "Changes are not saved. Esc again to drop them, or Ctrl+S to save."); }
        else running = 0;
        return;
    }
    close_asked = 0;
    if (!strcmp(k, "save")) do_save();
    else if (!strcmp(k, "up") || !strcmp(k, "down")) {
        int step = k[0] == 'u' ? -1 : 1, i = sel < 0 ? (step > 0 ? -1 : nrows) : sel;
        do i += step; while (i >= 0 && i < nrows && rows[i].header);
        if (i >= 0 && i < nrows) { capture = 0; select_row(i); }
    } else if (!strcmp(k, "enter") && sel >= 0) {
        capture = 1; cap_axis = -1;
        snprintf(status, sizeof status, "%s: press a button or a hat direction on any device.", rows[sel].label);
    } else if (!strcmp(k, "del") && sel >= 0) { capture = 0; clear_row(sel); }
}

static void poll_devices(void) {
    for (int d = 0; d < joy_dev_count(); d++)
        for (int c = 0; c < JOY_NCTL; c++) {
            if (!joy_ctl_exists(d, c)) continue;
            int down = joy_ctl_down(d, c);
            if (down && !prev_down[d][c]) on_press(d, c);
            prev_down[d][c] = (uint8_t)down;
        }
    if (cap_axis < 0) return;
    if (cap_axis == 3) {
        /* A throttle stays where it is put, so its start position says
         * nothing: the axis is the one that moves, and "full" is the end
         * where it is then held still for a second. */
        Uint32 now = SDL_GetTicks();
        if (thr_d < 0) {
            for (int d = 0; d < joy_dev_count() && thr_d < 0; d++)
                for (int a = 0; a < joy_dev_naxes(d) && a < 16; a++)
                    if (fabsf(joy_dev_axis(d, a) - axis_base[d][a]) >= 0.6f) { thr_d = d; thr_a = a; break; }
            if (thr_d >= 0) {
                thr_v = joy_dev_axis(thr_d, thr_a); thr_ms = now;
                snprintf(status, sizeof status, "Throttle: D%d axis %d. Now hold it at full for a second.", thr_d + 1, thr_a + 1);
            }
            return;
        }
        float v = joy_dev_axis(thr_d, thr_a);
        if (fabsf(v - thr_v) > 0.05f) { thr_v = v; thr_ms = now; return; }
        if (fabsf(v) < 0.8f || now - thr_ms < 1000) return;
        role_dev[3] = thr_d; role_axis[3] = v > 0 ? -(thr_a + 1) : thr_a + 1;    /* full is normally the axis's minimum */
        dirty = 1; cap_axis = -1;
        snprintf(status, sizeof status, "Throttle  =  D%d axis %d%s", thr_d + 1, thr_a + 1, v > 0 ? ", reversed" : "");
        return;
    }
    for (int d = 0; d < joy_dev_count(); d++)
        for (int a = 0; a < joy_dev_naxes(d) && a < 16; a++) {
            float dv = joy_dev_axis(d, a) - axis_base[d][a];
            if (fabsf(dv) < 0.6f) continue;
            int k = cap_axis;
            /* right, back and the right pedal raise the value; a throttle pushed to full lowers it */
            int rev = k == 3 ? dv > 0 : dv < 0;
            role_dev[k] = d; role_axis[k] = rev ? -(a + 1) : a + 1;
            dirty = 1; cap_axis = -1;
            snprintf(status, sizeof status, "%s  =  D%d axis %d%s", role_label[k], d + 1, a + 1, rev ? ", reversed" : "");
            return;
        }
}

int joy_controls_window(const char *dir) {
    ini_dir = dir;
    SDL_SetMainReady();
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) rt_fatal("SDL_Init: %s", SDL_GetError());
    SDL_Window *win = SDL_CreateWindow("TFX controls", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H,
                                       SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(win, &wm);
    HWND hwnd = wm.info.win.window;
    /* the monitor's scaling: the window and everything in it grow with it */
    UINT dpi = 96;
    HMODULE u32 = GetModuleHandleA("user32.dll");
    UINT (WINAPI *get_dpi)(HWND) = u32 ? (UINT (WINAPI *)(HWND))(void *)GetProcAddress(u32, "GetDpiForWindow") : NULL;
    if (get_dpi) dpi = get_dpi(hwnd);
    if (dpi < 96) dpi = 96;
    S = dpi / 96.0f;
    SDL_SetWindowSize(win, PX(WIN_W), PX(WIN_H));
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(win);
    int pw = PX(WIN_W), ph = PX(WIN_H);
    HDC wdc = GetDC(hwnd);
    mem = CreateCompatibleDC(wdc);
    SelectObject(mem, CreateCompatibleBitmap(wdc, pw, ph));
    f_norm = CreateFontA(-PX(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    f_bold = CreateFontA(-PX(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    f_small = CreateFontA(-PX(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    joy_open();
    load_model();
    rt_log("controls window: %d device(s), scale %.2f", joy_dev_count(), S);

    char test[1024] = "", *tp = test;
    if (getenv("TFX_CONTROLS_TEST")) snprintf(test, sizeof test, "%s", getenv("TFX_CONTROLS_TEST"));
    int frame_no = 0;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            extern int joy_event(const SDL_Event *);
            if (joy_event(&ev)) { load_model(); dirty = 0; snprintf(status, sizeof status, "The connected devices changed; the list was read again."); }
            switch (ev.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_MOUSEMOTION: ptr_x = (int)(ev.motion.x / S); ptr_y = (int)(ev.motion.y / S); break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) on_click((int)(ev.button.x / S), (int)(ev.button.y / S));
                break;
            case SDL_MOUSEWHEEL: {
                int max = 8 + nrows * ROW_H - LIST_H;
                scroll -= ev.wheel.y * ROW_H * 3;
                if (scroll > max) scroll = max;
                if (scroll < 0) scroll = 0;
                break;
            }
            case SDL_KEYDOWN: {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_ESCAPE) on_key("esc");
                else if (k == SDLK_UP) on_key("up");
                else if (k == SDLK_DOWN) on_key("down");
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) on_key("enter");
                else if (k == SDLK_DELETE || k == SDLK_BACKSPACE) on_key("del");
                else if (k == SDLK_s && (ev.key.keysym.mod & KMOD_CTRL)) on_key("save");
                break;
            }
            }
        }
        poll_devices();
        if (*tp && ++frame_no > 10) {           /* the test script, one command a frame */
            char cmd[300], *e = strchr(tp, ';');
            size_t n = e ? (size_t)(e - tp) : strlen(tp);
            snprintf(cmd, sizeof cmd, "%.*s", (int)(n < 299 ? n : 299), tp);
            tp += n + (e ? 1 : 0);
            int a = 0, b = 0;
            char arg[280] = "";
            if (sscanf(cmd, " click %d %d", &a, &b) == 2) { ptr_x = a; ptr_y = b; on_click(a, b); }
            else if (sscanf(cmd, " press %d %d", &a, &b) == 2) { if (joy_ctl_exists(a - 1, b - 1)) on_press(a - 1, b - 1); }
            else if (sscanf(cmd, " key %20s", arg) == 1) on_key(arg);
            else if (sscanf(cmd, " shot %279[^;]", arg) == 1) { draw(wdc, pw, ph); save_shot(arg, pw, ph); }
            else if (strstr(cmd, "save")) do_save();
            else if (strstr(cmd, "quit")) running = 0;
        }
        draw(wdc, pw, ph);
        SDL_Delay(16);
    }
    ReleaseDC(hwnd, wdc);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
