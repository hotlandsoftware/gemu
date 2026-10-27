#pragma once
/* Internals shared by monitor.c (transports, queue) and monitor_cmds.c
 * (command table, dispatch, built-in commands).  Not a public header. */
#include "gemu/monitor.h"
#include <pthread.h>
#include <stdio.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
typedef SOCKET sock_t;
#else
typedef int sock_t;
#endif

#define MON_QUEUE_SIZE   32
#define MON_LINE_MAX     256
#define MEDIA_DEVICE_MAX 16
#define ROM_ENTRY_MAX    16
#define BP_MAX           32
#define MON_BIND_MAX     64
#define BLOCK_NODE_MAX   16

typedef struct {
    uint32_t addr;
    uint32_t size;
    char     file[512];
} MonRomEntry;

typedef struct {
    int        id;
    GemuBpType type;
    uint32_t   addr;
} MonBpEntry;

typedef struct {
    char     name[48];
    uint32_t value;
} MonSymbol;

/* A block node from QMP blockdev-add: an image file that can be inserted
 * into a drive's open tray with blockdev-insert-medium. */
typedef struct {
    bool used;
    char name[64];
    char file[512];
    bool read_only;
    int  attached;     /* media index + 1 of the drive holding it; 0 = none */
} MonBlockNode;

/* QEMU's tray model, kept per media device.  Devices only know "loaded" or
 * "empty", so opening the tray unloads the medium from the device but keeps
 * it in the tray; closing the tray loads whatever is in it. */
typedef struct {
    bool open;
    char file[512];    /* medium sitting in the open tray; "" = none */
    int  node;         /* node index + 1 backing the drive's medium; 0 = none */
} MonTray;

typedef enum {
    MON_ENTRY_CMD,   /* control command from a non-console source (GTK menu) */
    MON_ENTRY_TEXT,  /* a command line to parse and run */
    MON_ENTRY_FN,    /* a function to call on the emulator thread (QMP) */
} MonEntryType;

typedef GemuMonCmd (*MonEntryFn)(GemuMonitor *mon, void *ud);

typedef struct {
    MonEntryType type;
    GemuMonCmd   cmd;
    MonEntryFn   fn;
    void        *fn_ud;
    bool         capture;  /* collect output for a QMP human-monitor-command */
    uint64_t     seq;
    char         text[MON_LINE_MAX];
} MonEntry;

typedef enum {
    MON_BACKEND_STDIO,
    MON_BACKEND_NONE,
    MON_BACKEND_TELNET,
    MON_BACKEND_GMP,
    MON_BACKEND_GMP_STDIO,
} MonBackend;

typedef struct {
    const char    *name;   /* canonical name from the command table */
    GemuMonHandler fn;
    void          *ud;
} MonBinding;

struct GemuMonitor {
    MonEntry        queue[MON_QUEUE_SIZE];
    int             head, tail;
    pthread_mutex_t lock;
    pthread_cond_t  done_cond;        /* signalled when an entry finishes */
    uint64_t        next_seq;         /* last sequence number handed out */
    uint64_t        done_seq;         /* last sequence number dispatched */
    pthread_t       thread;
    bool            running;
    bool            thread_started;
    volatile bool   thread_done;      /* reader thread has returned */
    volatile bool   replying;         /* reader is answering a request; stop()
                                         lets it finish so e.g. QMP 'quit'
                                         still gets its reply */
    bool            thread_abandoned; /* Windows: reader stuck in console
                                         read; leak mon instead of joining */
    bool            paused;
    uint32_t        last_step_count;

    /* Output capture for QMP human-monitor-command.  Only touched by the
     * emulator thread while dispatching a capture entry; the QMP thread
     * reads it after done_seq passes its entry, under lock. */
    bool            capturing;
    char           *capture_buf;
    size_t          capture_len, capture_size;

    GemuMediaDevice media[MEDIA_DEVICE_MAX];
    MonTray         tray[MEDIA_DEVICE_MAX];
    int             n_media;
    MonBlockNode    nodes[BLOCK_NODE_MAX];
    void          (*input_reset_cb)(void *ud);
    void           *input_reset_ud;
    MonRomEntry     rom_entries[ROM_ENTRY_MAX];
    int             n_rom_entries;
    MonBpEntry      bp_entries[BP_MAX];
    int             n_bps;
    int             next_bp_id;
    bool            has_exec_bp;
    bool            bp_resume_skip;   /* step over the breakpoint just hit */
    uint32_t        bp_resume_addr;
    bool            has_mem_bp;
    void          (*cpu_state_cb)(void *ud, char *buf, size_t buf_len);
    void           *cpu_state_ud;
    bool          (*screendump_cb)(void *ud, const char *path);
    void           *screendump_ud;
    GemuMonMemory   mem;
    bool            has_mem;
    MonBinding      bindings[MON_BIND_MAX];
    int             n_bindings;

    MonSymbol      *syms;            /* symbols loaded from an assembler's output */
    int             n_syms, cap_syms;
    char            sym_path[512];
    int64_t         sym_mtime;
    uint32_t        addr_mask;       /* address lines decoded (mirrors); 0 = all */

    MonBackend      backend;
    char            telnet_host[64];
    int             telnet_port;
    sock_t          listen_fd;
    sock_t          client_fd;
};

/* monitor.c */
typedef enum {
    MON_RUN_OK,
    MON_RUN_QUEUE_FULL,
    MON_RUN_TIMEOUT,
} MonRunResult;

/* Queue a command line and wait up to timeout_ms for the emulator thread to
 * run it.  With capture, *out receives everything the command printed
 * (malloc'd; NULL if it printed nothing). */
MonRunResult mon_run_line(GemuMonitor *mon, const char *line, bool capture,
                          unsigned timeout_ms, char **out);
/* Queue fn(mon, ud) and wait up to timeout_ms for the emulator thread to run
 * it.  On MON_RUN_TIMEOUT it may still run later, so ud must stay valid. */
MonRunResult mon_run_fn(GemuMonitor *mon, MonEntryFn fn, void *ud,
                        unsigned timeout_ms);
/* Listening-socket helpers shared by the telnet and GMP transports.
 * mon_accept_client waits up to a second and sets mon->client_fd. */
bool mon_accept_client(GemuMonitor *mon);
int  mon_recv_client(GemuMonitor *mon, void *buf, size_t len);
void mon_close_client(GemuMonitor *mon);
bool mon_sendall(sock_t fd, const void *buf, size_t n);
void mon_printf(const GemuMonitor *mon, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
const char *mon_machine_name(void);   /* NULL when unknown */

/* monitor_qmp.c - GMP, the QMP-compatible JSON protocol */
void mon_qmp_stdio_loop(GemuMonitor *mon, FILE *out);
void mon_qmp_socket_loop(GemuMonitor *mon);

/* monitor_cmds.c */
/* Parse and run one command line on the emulator thread. */
GemuMonCmd mon_dispatch(GemuMonitor *mon, const char *line);
/* True when the line is a quit command (the reader thread stops reading). */
bool       mon_line_is_quit(const char *line);
/* Called by the reader thread for lines it must handle itself because they
 * read further input from the transport (e.g. "change vnc password"). */
bool       mon_line_is_vnc_password(const char *line);

/* monitor_symbols.c */
GemuMonCmd mon_cmd_symbols(GemuMonitor *mon, const char *args);

/* monitor_block.c - the QMP blockdev-* operations.  Fill a MonBlockReq on
 * any thread, then run mon_block_run on the emulator thread (mon_run_fn). */
typedef enum {
    MON_BLOCK_ADD,             /* node, file, read_only */
    MON_BLOCK_DEL,             /* node */
    MON_BLOCK_OPEN_TRAY,       /* device */
    MON_BLOCK_CLOSE_TRAY,      /* device */
    MON_BLOCK_REMOVE_MEDIUM,   /* device */
    MON_BLOCK_INSERT_MEDIUM,   /* device, node */
    MON_BLOCK_CHANGE_MEDIUM,   /* device, file */
} MonBlockOp;

typedef struct {
    MonBlockOp  op;
    char        device[64];
    char        node[64];
    char        file[512];
    bool        read_only;
    /* Result: klass is NULL on success, else a QMP error class and desc. */
    const char *klass;
    char        desc[256];
} MonBlockReq;

GemuMonCmd mon_block_run(GemuMonitor *mon, void *req);
/* The text monitor's change/eject went through: keep the tray in step
 * (QEMU's eject leaves the tray open; change closes it). */
void       mon_block_media_changed(GemuMonitor *mon, GemuMediaDevice *dev, bool ejected);
