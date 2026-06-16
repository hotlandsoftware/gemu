#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * GEMU monitor — QEMU-style interactive console on stdin.
 *
 * A background thread reads lines from stdin and enqueues commands.
 * The emulator's run loop drains the queue each frame via gemu_monitor_poll().
 * Only active when stdin is a TTY; silently inert when piped or redirected.
 */

typedef enum {
    GEMU_MON_NONE = 0,
    GEMU_MON_RESET,
    GEMU_MON_QUIT,
    GEMU_MON_STOP,  /* halt emulation  */
    GEMU_MON_CONT,  /* resume          */
    GEMU_MON_STEP,  /* one instruction */
    GEMU_MON_CUSTOM,/* machine-specific command */
} GemuMonCmd;

typedef struct GemuMonitor GemuMonitor;

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
    void (*status)(void *ud, char *buf, size_t buf_len);
} GemuMediaDevice;

GemuMonitor *gemu_monitor_create(void);
void         gemu_monitor_destroy(GemuMonitor *mon);

/* Set the process-wide default monitor transport used by subsequently-created
 * monitors.  Supported forms: stdio, none, telnet:HOST:PORT,server,nowait. */
void         gemu_monitor_set_default(const char *spec);

/* Enqueue a reset or quit command from non-stdin sources (e.g. a GTK menu). */
void         gemu_monitor_enqueue_reset(GemuMonitor *mon);
void         gemu_monitor_enqueue_quit(GemuMonitor *mon);

bool         gemu_monitor_register_media(GemuMonitor *mon,
                                         const GemuMediaDevice *dev);

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
 * Each call to add_bp returns a unique id used for deletion.
 * The check_* functions are called by the machine in its run loop / memory
 * callbacks and return true (+ set paused) when a matching breakpoint fires.
 */
typedef enum {
    GEMU_BP_EXEC  = 1,
    GEMU_BP_READ  = 2,
    GEMU_BP_WRITE = 4,
} GemuBpType;

int  gemu_monitor_add_bp   (GemuMonitor *mon, GemuBpType type, uint32_t addr);
bool gemu_monitor_del_bp   (GemuMonitor *mon, int id);
void gemu_monitor_clear_bps(GemuMonitor *mon);

bool gemu_monitor_check_exec (GemuMonitor *mon, uint32_t addr);
bool gemu_monitor_check_read (GemuMonitor *mon, uint32_t addr);
bool gemu_monitor_check_write(GemuMonitor *mon, uint32_t addr);

/* Start the stdin reader thread (no-op if not a TTY). */
void         gemu_monitor_start(GemuMonitor *mon);
/* Signal and join the reader thread. */
void         gemu_monitor_stop(GemuMonitor *mon);

/* Dequeue one command (NONE if empty). Also updates the paused flag
 * for STOP/CONT/STEP commands so callers don't have to. */
GemuMonCmd   gemu_monitor_poll(GemuMonitor *mon);
uint32_t     gemu_monitor_step_count(const GemuMonitor *mon);
const char  *gemu_monitor_command_text(const GemuMonitor *mon);
void         gemu_monitor_unknown_command(const GemuMonitor *mon);

bool         gemu_monitor_is_paused(const GemuMonitor *mon);

/* Register a screendump callback.  The callback receives the output path and
 * is responsible for capturing the current framebuffer and writing the file.
 * Use gemu_screendump() from <gemu/screendump.h> for the actual I/O. */
void gemu_monitor_set_screendump_cb(GemuMonitor *mon,
                                    bool (*cb)(void *ud, const char *path),
                                    void *ud);
