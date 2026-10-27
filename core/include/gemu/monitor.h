#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * GEMU monitor - QEMU-style interactive console.
 *
 * A background thread reads lines from the selected transport (stdio,
 * telnet, GMP/QMP) and queues them.  The emulator's run loop drains the
 * queue each frame via gemu_monitor_poll(), which parses and runs every
 * command on the emulator thread.
 *
 * Every command is declared exactly once, in the table in monitor_cmds.c,
 * so 'help' lists the same set on every machine.  A command is either:
 *   - universal and built in (break, info, x, screendump, ...),
 *   - universal but implemented by the machine (savevm, sendkey, ...), or
 *   - restricted to a list of machines (gamegenie, dipswitch, ...).
 * Machines provide implementations with gemu_monitor_bind() and a memory
 * view with gemu_monitor_set_memory(); anything a machine doesn't provide
 * reports "not supported on this machine" instead of "unknown command".
 */

typedef enum {
    GEMU_MON_NONE = 0,
    GEMU_MON_RESET,
    GEMU_MON_QUIT,
    GEMU_MON_STOP,  /* halt emulation  */
    GEMU_MON_CONT,  /* resume          */
    GEMU_MON_STEP,  /* one instruction */
} GemuMonCmd;

typedef struct GemuMonitor GemuMonitor;
typedef struct GemuVncServer GemuVncServer;

typedef enum {
    GEMU_MEDIA_OK = 0,
    GEMU_MEDIA_OK_RESET,
    GEMU_MEDIA_ERR,
} GemuMediaResult;

typedef struct GemuMediaDevice {
    const char *name;
    const char *kind;
    char        file[512];   /* current media path; empty string = no media loaded */
    void       *ud;

    GemuMediaResult (*change)(void *ud, const char *arg,
                              char *err, size_t err_len);
    GemuMediaResult (*eject)(void *ud, char *err, size_t err_len);
    GemuMediaResult (*flip)(void *ud, char *err, size_t err_len);
    void (*status)(void *ud, char *buf, size_t buf_len);
} GemuMediaDevice;

GemuMonitor *gemu_monitor_create(void);
void         gemu_monitor_destroy(GemuMonitor *mon);

/* Set the process-wide default monitor transport used by subsequently-created
 * monitors.  Supported forms: stdio, none, telnet:HOST:PORT,server,nowait,
 * gmp:HOST:PORT, gmp-stdio. */
void         gemu_monitor_set_default(const char *spec);

/* Make SIGINT/SIGTERM quit the emulator through gemu_monitor_poll() (call
 * once at startup, before SDL is initialized). */
void         gemu_monitor_install_signal_handlers(void);

/* Set the process-wide machine name (the -M argument).  Machine-restricted
 * commands are only accepted when this name is in their machine list. */
void         gemu_monitor_set_machine(const char *name);

/* Enqueue a reset or quit command from non-stdin sources (e.g. a GTK menu). */
void         gemu_monitor_enqueue_reset(GemuMonitor *mon);
void         gemu_monitor_enqueue_quit(GemuMonitor *mon);
/* Queue any monitor command line to run at the next gemu_monitor_poll()
 * without waiting for it - safe from the emulator thread (window menus). */
bool         gemu_monitor_enqueue_command(GemuMonitor *mon, const char *line);

/* Registered media devices, for front ends (e.g. a Blocks menu). */
int          gemu_monitor_media_count(const GemuMonitor *mon);
const GemuMediaDevice *gemu_monitor_media_get(const GemuMonitor *mon, int index);
void         gemu_monitor_shutdown_or_pause(GemuMonitor *mon, bool no_shutdown);

/* ── Machine-provided commands ─────────────────────────────────────────────
 *
 * A handler receives the argument text after the command name with leading
 * and trailing whitespace removed ("" when there are none).  It runs on the
 * emulator thread from inside gemu_monitor_poll(), must print through
 * gemu_monitor_printf(), and returns the control command to hand back to the
 * run loop - normally GEMU_MON_NONE.
 *
 * The name must exist in the monitor's command table; binding an unknown
 * name fails (and logs to stderr) so that 'help' can never fall out of date.
 * A bound handler takes precedence over a built-in one of the same name.
 */
typedef GemuMonCmd (*GemuMonHandler)(GemuMonitor *mon, const char *args,
                                     void *ud);

bool         gemu_monitor_bind(GemuMonitor *mon, const char *name,
                               GemuMonHandler fn, void *ud);

/* Print to whichever transport issued the current command (stdout, telnet
 * client, or the QMP human-monitor-command reply). */
void         gemu_monitor_printf(const GemuMonitor *mon, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Memory view backing x, xp, peek, poke and memsave.  read must not have
 * side effects (no clearing of status registers, no FIFO pops); return false
 * for addresses that can't be inspected safely and they print as "--". */
typedef struct GemuMonMemory {
    bool   (*read)(void *ud, uint64_t addr, uint8_t *val);
    bool   (*write)(void *ud, uint64_t addr, uint8_t val);   /* optional */
    void    *ud;
    unsigned addr_bits;   /* width of the address space: 16, 20, 32, 64... */
    bool     big_endian;  /* byte order for x's h/w/g sizes */
} GemuMonMemory;

void         gemu_monitor_set_memory(GemuMonitor *mon, const GemuMonMemory *mem);

bool         gemu_monitor_register_media(GemuMonitor *mon,
                                         const GemuMediaDevice *dev);
/* The monitor's own copy of a registered device (its file[] is what
 * info block shows), or NULL. */
GemuMediaDevice *gemu_monitor_find_media(GemuMonitor *mon, const char *name);
void         gemu_monitor_set_input_reset_cb(GemuMonitor *mon,
                                             void (*cb)(void *ud),
                                             void *ud);
void         gemu_monitor_set_cpu_state_cb(GemuMonitor *mon,
                                           void (*cb)(void *ud,
                                                      char *buf,
                                                      size_t buf_len),
                                           void *ud);

/* Register a loaded ROM image for display by 'info roms'.
 * Call once per ROM after a successful load.  addr and size are in the
 * machine's CPU address space; file is the source path. */
void         gemu_monitor_register_rom(GemuMonitor *mon,
                                       uint32_t addr, uint32_t size,
                                       const char *file);
/* Clear all registered ROM entries (call before re-loading ROMs on reset). */
void         gemu_monitor_clear_roms(GemuMonitor *mon);

/* ── Breakpoints / watchpoints ─────────────────────────────────────────────
 *
 * Breakpoints fire on exec; watchpoints fire on memory read, write, or both.
 * The check_* functions are called by the machine in its run loop / memory
 * callbacks and return true (+ set paused) when a matching breakpoint fires.
 */
typedef enum {
    GEMU_BP_EXEC  = 1,
    GEMU_BP_READ  = 2,
    GEMU_BP_WRITE = 4,
} GemuBpType;

bool gemu_monitor_check_exec (GemuMonitor *mon, uint32_t addr);
bool gemu_monitor_check_read (GemuMonitor *mon, uint32_t addr);
bool gemu_monitor_check_write(GemuMonitor *mon, uint32_t addr);
bool gemu_monitor_has_exec_breakpoints(const GemuMonitor *mon);
/* Stop emulation as if by "stop" (e.g. a machine's own raster breakpoint). */
void gemu_monitor_pause(GemuMonitor *mon);
/* Address lines the machine decodes: breakpoints and symbols then match
 * through mirrors (an Atari 2600 passes 0x1FFF, so "break F000" also stops
 * at $1000).  0 = compare every bit. */
void gemu_monitor_set_addr_mask(GemuMonitor *mon, uint32_t mask);

/* ── Symbols (monitor_symbols.c) ───────────────────────────────────────────
 * Labels from an assembler (DASM -s, VICE/ca65 label files, NAME = $addr).
 * Every command that takes an address accepts them, with +/- offsets. */
int         gemu_monitor_load_symbols(GemuMonitor *mon, const char *path);   /* count, -1 = error */
void        gemu_monitor_clear_symbols(GemuMonitor *mon);
bool        gemu_monitor_reload_symbols_if_changed(GemuMonitor *mon);
const char *gemu_monitor_symbol_name(const GemuMonitor *mon, uint32_t addr);
bool        gemu_monitor_symbol_value(const GemuMonitor *mon, const char *name, uint32_t *value);
bool        gemu_monitor_parse_addr(const GemuMonitor *mon, const char *text, uint64_t *out);
bool gemu_monitor_has_mem_breakpoints (const GemuMonitor *mon);

/* Start the reader thread (no-op for stdio when stdin is not a TTY). */
void         gemu_monitor_start(GemuMonitor *mon);
/* Signal and join the reader thread. */
void         gemu_monitor_stop(GemuMonitor *mon);

/* Run queued commands until one needs the run loop's attention (reset,
 * quit, stop, cont, step) and return it, or return GEMU_MON_NONE once the
 * queue is empty.  Also updates the paused flag for STOP/CONT/STEP. */
GemuMonCmd   gemu_monitor_poll(GemuMonitor *mon);
uint32_t     gemu_monitor_step_count(const GemuMonitor *mon);

bool         gemu_monitor_is_paused(const GemuMonitor *mon);

/* Register a screendump callback.  The callback receives the output path and
 * is responsible for capturing the current framebuffer and writing the file.
 * Use gemu_screendump() from <gemu/screendump.h> for the actual I/O. */
void gemu_monitor_set_screendump_cb(GemuMonitor *mon,
                                    bool (*cb)(void *ud, const char *path),
                                    void *ud);
