/* Controllers and joysticks (joy.c): what the controls window (joyui.c) needs. */
#ifndef JOY_H
#define JOY_H

#define JOY_MAXDEV 4
#define JOY_NCTL 36         /* a device's controls: buttons 1-32, then its hat's up, right, down, left */

int joy_open(void);                         /* (re)open every device; returns how many */
int joy_dev_count(void);
int joy_dev_main(void);                     /* index of the main device (stick, default buttons), -1 none */
int joy_dev_is_pad(int d);                  /* a game controller as the main device: named controls */
const char *joy_dev_name(int d);
int joy_dev_naxes(int d);
float joy_dev_axis(int d, int a);           /* axis a (0-based), -1..1 */

int joy_ctl_exists(int d, int c);
void joy_ctl_label(int d, int c, char *b, int n);   /* "Button 7", "Hat up", "Left trigger" */
void joy_ctl_key(int d, int c, char *b, int n);     /* its key in tfx.ini: joy_button7, joy3_hat_up, joy_lt */
const char *joy_ctl_default(int d, int c);
const char *joy_ctl_value(int d, int c);            /* its binding as configured */
int joy_ctl_down(int d, int c);

int joy_bind_code(const char *s);           /* a binding's value: 0 none, -1 not understood */
void joy_role_get(int k, int *dev, int *axis);      /* k: 0 x, 1 y, 2 rudder, 3 throttle; axis 1-based, 0 none, <0 reversed */

int joy_controls_window(const char *dir);   /* joyui.c: the window; dir holds tfx.ini */

#endif
