/* Startup: load the relocated LE image, set up DOS/4GW-like state, run. */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <string.h>
#include <windows.h>
#include <SDL.h>
#include "rt.h"
#include "settings.h"

/* set by tools/build.py: the original executable this build was translated from */
#ifndef PROGRAM_FILE
#define PROGRAM_FILE "TFX.DAT"
#endif
#ifndef PROGRAM_HASH
#define PROGRAM_HASH 0ull
#endif
#ifndef DEFAULT_ENV                  /* START.EXE sets DID=<cd drive>: for TFX.DAT */
#define DEFAULT_ENV ""
#endif
#ifndef DEFAULT_ARGS                 /* TFX.BAT passes the flow name */
#define DEFAULT_ARGS ""
#endif
#ifndef REQUIRE_CD
#define REQUIRE_CD 0
#endif
#ifndef MUSIC_DRIVER
#define MUSIC_DRIVER 0
#endif
#ifndef TFX_GAME
#define TFX_GAME 0
#endif

void dpmi_init(void);
static char cd_image[512];
static FILE *rt_logfile;

void rt_log(const char *fmt, ...) {
    va_list ap;
    if (!rt_logfile) return;
    va_start(ap, fmt);
    vfprintf(rt_logfile, fmt, ap);
    va_end(ap);
    fputc('\n', rt_logfile);
    fflush(rt_logfile);
}

void rt_dump_regs(void) {
    rt_log("eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
           eax, ebx, ecx, edx, esi, edi, ebp, esp);
    rt_log("last eip=%08x", rt_eip);
    rt_log("cs=%04x ds=%04x es=%04x ss=%04x fs=%04x gs=%04x eflags=%08x",
           cs, ds, es, ss, fs, gs, rt_get_eflags());
    rt_log("stack: %08x %08x %08x %08x %08x %08x %08x %08x",
           RD(ss_base + esp), RD(ss_base + esp + 4), RD(ss_base + esp + 8), RD(ss_base + esp + 12),
           RD(ss_base + esp + 16), RD(ss_base + esp + 20), RD(ss_base + esp + 24), RD(ss_base + esp + 28));
}

void rt_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s", buf);
    rt_dump_regs();
    rt_dump_ftrace(200);
    { extern void rt_dump_smc(void); rt_dump_smc(); }
    {   /* TFX_DUMP="addr:len,..." (hex): guest memory to log on a crash */
        const char *d = getenv("TFX_DUMP");
        while (d && *d) {
            char *e; uint32_t a = (uint32_t)strtoul(d, &e, 16), n = 64;
            if (*e == ':') n = (uint32_t)strtoul(e + 1, &e, 16);
            for (uint32_t o = 0; o < n && a + o + 16 <= MEM_SIZE; o += 16) {
                char hex[16 * 5 + 1]; int len = 0;
                for (int k = 0; k < 16; k += 2) len += snprintf(hex + len, sizeof hex - len, "%04x ", RW(a + o + k));
                rt_log("dump %08x: %s", a + o, hex);
            }
            d = *e == ',' ? e + 1 : e;
            if (*d && *d != ',' && !isxdigit((unsigned char)*d)) break;
        }
    }
    { extern void rt_dump_iring(void); rt_dump_iring(); }
    fprintf(stderr, "FATAL: %s\n", buf);
    if (!getenv("TFX_CMDFILE") && !getenv("TFX_SCRIPT"))
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TFX runtime error", buf, NULL);
    exit(1);
}

void rt_host_sleep_ms(int ms) { Sleep((DWORD)ms); }

static void exe_dir(char *out, int n) {
    GetModuleFileNameA(NULL, out, n);
    char *s = strrchr(out, '\\');
    if (s) *s = 0;
}

uint8_t *code_pristine;

int le_load(const char *path, uint64_t expect_hash, uint32_t *entry, uint32_t *stack,
            void (*placed)(int idx, uint32_t base, uint32_t size, uint32_t flags), char *err, int errlen);

static void object_placed(int i, uint32_t base, uint32_t size, uint32_t flags) {
    if ((flags & 0x2004) == 0x2004 && !code_hi) {
        code_lo = base; code_hi = base + size;
        code_pristine = malloc(size);              /* for the `c` code-diff command */
        if (code_pristine) memcpy(code_pristine, MEM + base, size);
    }
    /* 16-bit objects get their own selector (matches tools/le.py selector_for) */
    if (!(flags & 0x2000)) {
        int idx = (0x18 + 8 * i) >> 3;
        ldt[idx].used = 1; ldt[idx].base = base; ldt[idx].limit = size - 1;
        ldt[idx].code = (flags & 4) != 0;
    }
    rt_log("object %u: base %08x size %08x flags %04x", i + 1, base, size, flags);
}

/* ---- finding the game -------------------------------------------------------- */
static void user_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("ERROR: %s", buf);
    if (!getenv("TFX_CMDFILE") && !getenv("TFX_SCRIPT"))
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TFX", buf, NULL);
    exit(1);
}

static int file_exists(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* first *.cue in dir (any name: usually TFX.cue) */
static int find_cue(const char *dir, char *out, int n) {
    char pat[600];
    snprintf(pat, sizeof pat, "%s\\*.cue", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    snprintf(out, n, "%s\\%s", dir, fd.cFileName);
    FindClose(h);
    return 1;
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        uintptr_t fa = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
        long long guest = (long long)fa - (long long)(uintptr_t)MEM;
        rt_log("HOST EXCEPTION %08lx at host pc %p, data address %p (guest %08llx)", code,
               ep->ExceptionRecord->ExceptionAddress, (void *)fa, guest);
        {   /* module-relative: llvm-addr2line -e build/<name>/<name>.exe 0x<image base + rva> */
            HMODULE m = GetModuleHandleA(NULL);
            rt_log("host pc = exe base %p + rva %#llx", (void *)m,
                   (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)m));
        }
        rt_dump_regs();
        rt_dump_ftrace(200);
    { extern void rt_dump_smc(void); rt_dump_smc(); }
    {   /* TFX_DUMP="addr:len,..." (hex): guest memory to log on a crash */
        const char *d = getenv("TFX_DUMP");
        while (d && *d) {
            char *e; uint32_t a = (uint32_t)strtoul(d, &e, 16), n = 64;
            if (*e == ':') n = (uint32_t)strtoul(e + 1, &e, 16);
            for (uint32_t o = 0; o < n && a + o + 16 <= MEM_SIZE; o += 16) {
                char hex[16 * 5 + 1]; int len = 0;
                for (int k = 0; k < 16; k += 2) len += snprintf(hex + len, sizeof hex - len, "%04x ", RW(a + o + k));
                rt_log("dump %08x: %s", a + o, hex);
            }
            d = *e == ',' ? e + 1 : e;
            if (*d && *d != ',' && !isxdigit((unsigned char)*d)) break;
        }
    }
        { extern void rt_dump_iring(void); rt_dump_iring(); }
        if (rt_logfile) fflush(rt_logfile);
        ExitProcess(3);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The last thing at exit (registered first, so it runs after every other
 * atexit handler: MIDI reset, WAV files closed): end the process without
 * unloading the DLLs.  After "Quit to DOS" from a flight with DLSS Frame
 * Generation, the normal exit crashed inside a DLL while it was being
 * unloaded (an access violation at exit, DEBUG_LOG 124); files and the log
 * are flushed first. */
int rt_exit_status;
static void final_exit(void) {
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), (UINT)rt_exit_status);
}

int main(int argc, char **argv) {
    char dir[512], path[600];
    exe_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s\\runtime.log", dir);
    rt_logfile = fopen(path, "w");
    { extern void settings_load(const char *dir); settings_load(dir); }
    for (int i = 1; i < argc; i++)          /* the controls window instead of the game (joyui.c) */
        if (!strcmp(argv[i], "--controls")) { extern int joy_controls_window(const char *dir); return joy_controls_window(dir); }
    AddVectoredExceptionHandler(1, crash_handler);
    timeBeginPeriod(1);              /* 1 ms sleeps for the busy-wait relief */
    atexit(final_exit);

    int first_arg = 1, n_env_given = 0;
    for (; first_arg < argc; first_arg++) {
        if (!strcmp(argv[first_arg], "--game") && first_arg + 1 < argc) {
            snprintf(game_dir, sizeof game_dir, "%s", argv[++first_arg]);
        } else if (!strcmp(argv[first_arg], "--cdimage") && first_arg + 1 < argc) {
            snprintf(cd_image, sizeof cd_image, "%s", argv[++first_arg]);
        } else if (!strcmp(argv[first_arg], "--cd") && first_arg + 1 < argc) {
            snprintf(cd_dir, sizeof cd_dir, "%s", argv[++first_arg]);
        } else if (!strcmp(argv[first_arg], "--env") && first_arg + 1 < argc) {
            extern const char *extra_env[32];
            extern int n_extra_env;
            if (n_extra_env < 32) extra_env[n_extra_env++] = argv[++first_arg];
            else ++first_arg;
            n_env_given++;
        } else break;
    }
    /* game folder: --game, else the exe's own folder when the program is
     * there (tfx.exe copied into the TFX folder), else ./game */
    if (!game_dir[0]) {
        snprintf(path, sizeof path, "%s\\%s", dir, PROGRAM_FILE);
        if (file_exists(path)) snprintf(game_dir, sizeof game_dir, "%s", dir);
        else snprintf(game_dir, sizeof game_dir, "%s\\game", dir);
    }
    snprintf(path, sizeof path, "%s\\%s", game_dir, PROGRAM_FILE);
    if (!file_exists(path))
        user_error("%s was not found.\n\nCopy this program into your TFX folder (the one with %s) "
                   "and start it from there, or pass --game <folder>.", path, PROGRAM_FILE);
    /* CD: an extracted copy of the CD files (--cd, or a cd folder holding
     * DID.DAT), and the BIN/CUE image for CD audio and, without an extracted
     * copy, the data files too.  A common layout keeps it in ..\cd next to the game. */
    if (!cd_dir[0]) {
        snprintf(path, sizeof path, "%s\\cd\\DID.DAT", dir);
        if (file_exists(path)) snprintf(cd_dir, sizeof cd_dir, "%s\\cd", dir);
    }
    if (!cd_image[0]) {
        char cand[6][600];
        snprintf(cand[0], 600, "%s\\..\\cd", game_dir);
        snprintf(cand[1], 600, "%s\\cd", game_dir);
        snprintf(cand[2], 600, "%s\\cd", dir);
        snprintf(cand[3], 600, "%s", game_dir);
        snprintf(cand[4], 600, "%s", dir);
        snprintf(cand[5], 600, "%s\\..", game_dir);
        for (int i = 0; i < 6 && !cd_image[0]; i++) find_cue(cand[i], cd_image, sizeof cd_image);
    }
    if (REQUIRE_CD && !cd_image[0] && !cd_dir[0])
        user_error("The TFX CD image was not found.\n\nPut TFX.cue and TFX.bin in a 'cd' folder next to the "
                   "TFX folder, or pass --cdimage <file.cue>.");
    {
        extern const char *extra_env[32];
        extern int n_extra_env;
        if (!n_env_given && DEFAULT_ENV[0]) extra_env[n_extra_env++] = DEFAULT_ENV;
        /* the emulated Sound Blaster 16 (runtime/sb.c), as DOSBox announces it */
        int has_blaster = 0;
        for (int i = 0; i < n_extra_env; i++) if (!_strnicmp(extra_env[i], "BLASTER=", 8)) has_blaster = 1;
        if (!has_blaster && n_extra_env < 32) {
            extern const char *sb_blaster(void);
            static char bl[160];
            snprintf(bl, sizeof bl, "BLASTER=%s", sb_blaster());
            extra_env[n_extra_env++] = bl;
        }
    }
    rt_log("game dir: %s", game_dir);
    rt_log("cd dir:   %s", cd_dir[0] ? cd_dir : "(files read from the CD image)");
    rt_log("cd image: %s", cd_image[0] ? cd_image : "(none)");

    MEM = VirtualAlloc(NULL, MEM_SIZE + 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!MEM) rt_fatal("cannot allocate guest memory");

    dpmi_init();
    hw_init();
    uint32_t entry, stack;
    {
        char err[512];
        snprintf(path, sizeof path, "%s\\%s", game_dir, PROGRAM_FILE);
        if (le_load(path, PROGRAM_HASH, &entry, &stack, object_placed, err, sizeof err))
            user_error("%s", err);
    }
#if TFX_GAME
    {   /* tfx.ini framerate=high: the frame limiter 0x41B998 waits until
         * `cmp dx, 2` ticks of the 54.6 Hz driver clock have passed; lower the
         * minimum to 1.  The instruction runs with its live bytes (listed in
         * tools/smc/TFX.DAT.live.txt), and the game's time step is per tick. */
        extern struct port_settings settings;
        if (settings.framerate_high && RB(0x41b9b6) == 0x66 && RB(0x41b9b7) == 0x83 && RB(0x41b9b9) == 2) {
            WB(0x41b9b9, 1);
            rt_log("settings: frame limiter minimum set to 1 tick");
        }
    }
#endif

    /* program arguments: everything after our own options */
    char *pargv[64];
    int pargc = 0;
    pargv[pargc++] = argv[0];
    for (int i = first_arg; i < argc && pargc < 63; i++) pargv[pargc++] = argv[i];
    if (pargc == 1 && DEFAULT_ARGS[0]) pargv[pargc++] = (char *)DEFAULT_ARGS;
    dos_init(pargc, pargv);
#if MUSIC_DRIVER
    { extern void music_init(void); music_init(); }   /* START.EXE's resident music driver */
#endif

    video_init();
    vga_init();
    if (cd_image[0]) cd_open(cd_image);
    fpu_init();
    { extern void rt_interp_init(void); rt_interp_init(); }

    /* DOS/4GW entry state */
    rt_load_seg(1, SEL_FLAT_CS);
    rt_load_seg(3, SEL_FLAT_DS);
    rt_load_seg(2, SEL_FLAT_DS);
    rt_load_seg(0, SEL_PSP);
    rt_load_seg(4, 0);
    rt_load_seg(5, 0);
    esp = stack;
    eax = ebx = ecx = edx = esi = edi = ebp = 0;
    if_flag = 1;
    df = 0;

    rt_log("starting at %08x, stack %08x", entry, stack);
    PUSH32(MAGIC_RET);
    rt_call(entry, MAGIC_RET);
    rt_log("program returned from entry point (unwind=%08x)", rt_unwind);
    rt_dump_ftrace(300);
    return 0;
}
