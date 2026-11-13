/* Internal runtime interfaces (not used by generated code). */
#pragma once
#include <stdint.h>
#include "cpu.h"

/* ---- memory layout -------------------------------------------------------- */
#define BDA_BASE      0x00400u      /* BIOS data area */
#define ENV_SEG       0x0700u       /* environment block, real-mode segment */
#define PSP_SEG       0x0800u       /* PSP, real-mode segment */
#define DOS_POOL_LO   0x01000u      /* conventional memory pool (paragraphs) */
#define DOS_POOL_HI   0x09F00u
#define HEAP_LO       0x02000000u   /* extended memory heap start (32 MB) */

/* ---- selectors ------------------------------------------------------------ */
#define SEL_FLAT_CS   0x08
#define SEL_FLAT_DS   0x10
#define SEL_PSP       0x40
#define SEL_ENV       0x48
#define SEL_BIOS      0x50          /* base 0x400 (BDA) */
#define SEL_VGA       0x58          /* base 0xA0000 */
#define SEL_FIRST_DYN 0x80
#define NSEL          1024

struct descr { uint32_t base, limit; uint8_t used, is32, code; };
extern struct descr ldt[NSEL];
uint16_t sel_alloc(uint32_t base, uint32_t limit);
void sel_free(uint16_t sel);
uint32_t sel_base(uint16_t sel);

/* ---- utilities ------------------------------------------------------------- */
void rt_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_dump_regs(void);
int rt_magic_call(uint32_t target);
void rt_interpret(uint32_t start, uint32_t expected_ret);
extern uint32_t code_lo, code_hi;

/* magic code addresses: "default" handlers that live in the runtime */
#define MAGIC_BASE    0xFFFF0000u
#define MAGIC_INT(n)  (MAGIC_BASE + (n))     /* original handler of vector n */
#define MAGIC_RET     (MAGIC_BASE + 0x1000)  /* return-to-runtime marker */

/* ---- interrupts ------------------------------------------------------------- */
struct pm_vec { uint16_t sel; uint32_t off; };
extern struct pm_vec pm_vectors[256];
void irq_raise(int irq);
void dos_init(int argc, char **argv);
void dos_int21(void);
void dpmi_int31(void);
void bios_int10(void);
void bios_int16(void);
void bios_int1a(void);
void mouse_int33(void);
void mux_int2f(void);
uint32_t heap_alloc(uint32_t size);
void heap_free(uint32_t addr);
uint32_t heap_resize(uint32_t addr, uint32_t size);
void call_guest_int(int vec);

/* ---- hardware --------------------------------------------------------------- */
void hw_init(void);
void vga_init(void);
void vga_set_mode(int mode);
int  vga_get_mode(void);
uint8_t vga_in(uint16_t port);
void vga_out(uint16_t port, uint8_t v);
void video_init(void);
void video_present(void);
void video_pump(void);
uint64_t host_ticks_us(void);
void pit_out(uint16_t port, uint8_t v);
uint8_t pit_in(uint16_t port);
void timer_service(void);
void kbd_push_scancode(uint8_t sc);
/* joy.c: controllers and joysticks as the game port's Flightstick Pro */
void joy_init(void);
void joy_poll(void);
void joy_port_write(void);
uint8_t joy_port_read(void);
int joy_setting(const char *k, const char *v);
void joy_virtual(const char *args);
void joy_log(void);
uint8_t kbd_read_data(void);
int kbd_bios_get(int remove, uint16_t *key);
void mouse_update(int x, int y, int buttons);
void mouse_relative(int dx, int dy, int buttons);
extern int mouse_x, mouse_y, mouse_buttons;

/* ---- CD-ROM ------------------------------------------------------------------ */
int cd_open(const char *cue_path);
int cd_present(void);
int mscdex_int2f(uint32_t regs);

/* ---- paths ------------------------------------------------------------------ */
extern char game_dir[512];
extern char cd_dir[512];
int dos_map_path(const char *dos, char *host, int hostlen);
