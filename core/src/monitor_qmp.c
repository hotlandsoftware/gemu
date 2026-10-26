/*
 * GEMU monitor - GMP, the QMP-compatible JSON protocol.
 *
 * Speaks the QEMU Machine Protocol over a TCP socket (-monitor gmp:HOST:PORT)
 * or stdio (-monitor gmp-stdio): a greeting, capabilities negotiation, then
 * one JSON object per command, each answered with {"return": ...} or
 * {"error": {"class": ..., "desc": ...}}, echoing the request's "id".
 *
 * JSON is parsed and generated with cJSON.  Monitor commands run on the
 * emulator thread like console commands; human-monitor-command returns
 * whatever the command printed.
 */
#include "monitor_priv.h"
#include "cJSON.h"
#include "gemu/vnc.h"
#include "gemu/gemu.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* How long to wait for the emulator thread to run a command. */
#define QMP_WAIT_MS   10000
/* Largest request accepted; anything bigger is rejected and discarded. */
#define QMP_MAX_INPUT (1024 * 1024)

typedef struct {
    GemuMonitor *mon;
    FILE        *out;          /* stdio transport; NULL = mon->client_fd */
    bool         negotiated;   /* qmp_capabilities has been accepted */

    /* Framing state: bytes of the top-level JSON value being received. */
    char        *buf;
    size_t       len, cap;
    int          depth;
    bool         in_string, escape;
} QmpConn;

typedef struct {
    const char *klass;
    char        desc[256];
} QmpError;

/* ── Output ──────────────────────────────────────────────────────────────── */

static void qmp_send_json(QmpConn *c, cJSON *msg) {
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!text) return;
    size_t len = strlen(text);
    if (c->out) {
        fwrite(text, 1, len, c->out);
        fputs("\r\n", c->out);
        fflush(c->out);
    } else {
        mon_sendall(c->mon->client_fd, text, len);
        mon_sendall(c->mon->client_fd, "\r\n", 2);
    }
    cJSON_free(text);
}

/* Attach a copy of the request's id, if it had one. */
static void qmp_add_id(cJSON *msg, const cJSON *id) {
    if (id) cJSON_AddItemToObject(msg, "id", cJSON_Duplicate(id, true));
}

static void qmp_send_return(QmpConn *c, cJSON *result, const cJSON *id) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddItemToObject(msg, "return", result ? result : cJSON_CreateObject());
    qmp_add_id(msg, id);
    qmp_send_json(c, msg);
}

static void qmp_send_error(QmpConn *c, const char *klass, const char *desc,
                           const cJSON *id) {
    cJSON *err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "class", klass ? klass : "GenericError");
    cJSON_AddStringToObject(err, "desc", desc);
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddItemToObject(msg, "error", err);
    qmp_add_id(msg, id);
    qmp_send_json(c, msg);
}

static cJSON *qmp_version(void) {
    cJSON *v = cJSON_CreateObject();
    cJSON *qemu = cJSON_AddObjectToObject(v, "qemu");
    cJSON_AddNumberToObject(qemu, "major", 1);
    cJSON_AddNumberToObject(qemu, "minor", 6);
    cJSON_AddNumberToObject(qemu, "micro", 0);
    cJSON *gemu = cJSON_AddObjectToObject(v, "gemu");
    cJSON_AddNumberToObject(gemu, "major", GEMU_VERSION_MAJOR);
    cJSON_AddNumberToObject(gemu, "minor", GEMU_VERSION_MINOR);
    cJSON_AddNumberToObject(gemu, "micro", GEMU_VERSION_PATCH);
    cJSON_AddStringToObject(gemu, "stage", GEMU_VERSION_STAGE);
    cJSON_AddStringToObject(v, "package", "");
    return v;
}

static void qmp_send_greeting(QmpConn *c) {
    cJSON *qmp = cJSON_CreateObject();
    cJSON_AddItemToObject(qmp, "version", qmp_version());
    cJSON_AddArrayToObject(qmp, "capabilities");
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddItemToObject(msg, "QMP", qmp);
    qmp_send_json(c, msg);
}

/* ── Commands ────────────────────────────────────────────────────────────── */

/* A command returns its result (NULL means {}), or sets err->klass. */
typedef cJSON *(*QmpFn)(QmpConn *c, const cJSON *args, QmpError *err);

static void set_error(QmpError *err, const char *klass, const char *fmt, const char *arg) {
    err->klass = klass;
    snprintf(err->desc, sizeof(err->desc), fmt, arg);
}

/* Run a monitor command line on the emulator thread.  Returns its output
 * (malloc'd, may be NULL), or sets err. */
static char *run_line(QmpConn *c, const char *line, QmpError *err) {
    char *out = NULL;
    switch (mon_run_line(c->mon, line, true, QMP_WAIT_MS, &out)) {
    case MON_RUN_OK:
        break;
    case MON_RUN_QUEUE_FULL:
        set_error(err, "GenericError", "%s", "monitor command queue is full");
        break;
    case MON_RUN_TIMEOUT:
        set_error(err, "GenericError", "%s",
                  "timed out waiting for the machine to run the command");
        break;
    }
    return out;
}

static cJSON *qmp_capabilities(QmpConn *c, const cJSON *args, QmpError *err) {
    (void)args; (void)err;
    c->negotiated = true;
    return NULL;
}

static cJSON *qmp_query_status(QmpConn *c, const cJSON *args, QmpError *err) {
    (void)args; (void)err;
    bool paused = gemu_monitor_is_paused(c->mon);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "running", !paused);
    cJSON_AddBoolToObject(r, "singlestep", false);
    cJSON_AddStringToObject(r, "status", paused ? "paused" : "running");
    return r;
}

static cJSON *qmp_query_version(QmpConn *c, const cJSON *args, QmpError *err) {
    (void)c; (void)args; (void)err;
    return qmp_version();
}

static cJSON *qmp_query_commands(QmpConn *c, const cJSON *args, QmpError *err);

/* Commands that map onto a monitor command with no arguments. */
static cJSON *qmp_simple(QmpConn *c, const char *line, QmpError *err) {
    free(run_line(c, line, err));
    return NULL;
}

static cJSON *qmp_system_reset(QmpConn *c, const cJSON *a, QmpError *e) { (void)a; return qmp_simple(c, "system_reset", e); }
static cJSON *qmp_stop(QmpConn *c, const cJSON *a, QmpError *e)         { (void)a; return qmp_simple(c, "stop", e); }
static cJSON *qmp_cont(QmpConn *c, const cJSON *a, QmpError *e)         { (void)a; return qmp_simple(c, "cont", e); }
static cJSON *qmp_quit(QmpConn *c, const cJSON *a, QmpError *e)         { (void)a; return qmp_simple(c, "quit", e); }

static cJSON *qmp_human_monitor_command(QmpConn *c, const cJSON *args, QmpError *err) {
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(args, "command-line");
    if (!cmd) {
        set_error(err, "GenericError", "Parameter '%s' is missing", "command-line");
        return NULL;
    }
    if (!cJSON_IsString(cmd)) {
        set_error(err, "GenericError",
                  "Invalid parameter type for '%s', expected: string", "command-line");
        return NULL;
    }
    const char *line = cmd->valuestring;
    while (isspace((unsigned char)*line)) line++;
    if (!*line)
        return cJSON_CreateString("");
    if (strlen(line) >= MON_LINE_MAX) {
        set_error(err, "GenericError", "%s", "command line is too long");
        return NULL;
    }
    char *out = run_line(c, line, err);
    if (err->klass) {
        free(out);
        return NULL;
    }
    cJSON *r = cJSON_CreateString(out ? out : "");
    free(out);
    return r;
}

/* set_password / expire_password: {"protocol": "vnc", ...}, like QEMU. */
static bool vnc_protocol(const cJSON *args, QmpError *err) {
    const cJSON *proto = cJSON_GetObjectItemCaseSensitive(args, "protocol");
    if (!cJSON_IsString(proto)) {
        set_error(err, "GenericError", "Parameter '%s' is missing", "protocol");
        return false;
    }
    if (strcmp(proto->valuestring, "vnc") != 0) {
        set_error(err, "GenericError", "Unsupported protocol '%s'", proto->valuestring);
        return false;
    }
    return true;
}

static cJSON *qmp_set_password(QmpConn *c, const cJSON *args, QmpError *err) {
    (void)c;
    if (!vnc_protocol(args, err)) return NULL;
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(args, "password");
    if (!cJSON_IsString(pw)) {
        set_error(err, "GenericError", "Parameter '%s' is missing", "password");
        return NULL;
    }
    char msg[160];
    if (!gemu_vnc_set_password(gemu_vnc_get(), pw->valuestring, msg, sizeof(msg)))
        set_error(err, "GenericError", "%s", msg);
    return NULL;
}

static cJSON *qmp_expire_password(QmpConn *c, const cJSON *args, QmpError *err) {
    (void)c;
    if (!vnc_protocol(args, err)) return NULL;
    const cJSON *time = cJSON_GetObjectItemCaseSensitive(args, "time");
    if (!cJSON_IsString(time)) {
        set_error(err, "GenericError", "Parameter '%s' is missing", "time");
        return NULL;
    }
    if (strcmp(time->valuestring, "now") == 0)
        gemu_vnc_expire_password(gemu_vnc_get());
    else if (strcmp(time->valuestring, "never") != 0)
        set_error(err, "GenericError", "%s", "only 'now' and 'never' are supported");
    return NULL;
}

static cJSON *qmp_query_vnc(QmpConn *c, const cJSON *args, QmpError *err) {
    (void)c; (void)args; (void)err;
    GemuVncServer *vnc = gemu_vnc_get();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", vnc != NULL);
    if (vnc) {
        char info[2048];
        gemu_vnc_info(vnc, info, sizeof(info));
        cJSON_AddStringToObject(r, "info", info);
    }
    return r;
}

/* ── Block devices ───────────────────────────────────────────────────────── */

/* A string argument: true and copied if present, else sets err when
 * required.  Absent optional arguments leave out untouched. */
static bool str_arg(const cJSON *args, const char *name, bool required,
                    char *out, size_t out_len, QmpError *err) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, name);
    if (!v) {
        if (required) set_error(err, "GenericError", "Parameter '%s' is missing", name);
        return false;
    }
    if (!cJSON_IsString(v)) {
        set_error(err, "GenericError", "Invalid parameter type for '%s', expected: string", name);
        return false;
    }
    if (strlen(v->valuestring) >= out_len) {
        set_error(err, "GenericError", "Parameter '%s' is too long", name);
        return false;
    }
    snprintf(out, out_len, "%s", v->valuestring);
    return true;
}

static bool bool_arg(const cJSON *args, const char *name, bool *out, QmpError *err) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, name);
    if (!v) return true;
    if (!cJSON_IsBool(v)) {
        set_error(err, "GenericError", "Invalid parameter type for '%s', expected: boolean", name);
        return false;
    }
    *out = cJSON_IsTrue(v);
    return true;
}

/* The drive: "id" (qdev id) or "device" (block backend name).  GEMU's
 * media devices have one name that serves as both. */
static bool device_arg(const cJSON *args, MonBlockReq *r, QmpError *err) {
    bool have_dev = str_arg(args, "device", false, r->device, sizeof(r->device), err);
    if (err->klass) return false;
    char id[64];
    bool have_id = str_arg(args, "id", false, id, sizeof(id), err);
    if (err->klass) return false;
    if (have_dev && have_id) {
        set_error(err, "GenericError", "%s", "Need exactly one of 'device' and 'id'");
        return false;
    }
    if (!have_dev && !have_id) {
        set_error(err, "GenericError", "Parameter '%s' is missing", "id");
        return false;
    }
    if (have_id) snprintf(r->device, sizeof(r->device), "%s", id);
    return true;
}

/* Run a filled-in request on the emulator thread.  The request is on the
 * heap: if the wait times out the emulator thread may still write to it,
 * so it is leaked rather than freed. */
static cJSON *run_block(QmpConn *c, MonBlockReq *r, QmpError *err) {
    if (err->klass) {
        free(r);
        return NULL;
    }
    switch (mon_run_fn(c->mon, mon_block_run, r, QMP_WAIT_MS)) {
    case MON_RUN_OK:
        if (r->klass) {
            err->klass = r->klass;
            snprintf(err->desc, sizeof(err->desc), "%s", r->desc);
        }
        free(r);
        break;
    case MON_RUN_QUEUE_FULL:
        set_error(err, "GenericError", "%s", "monitor command queue is full");
        free(r);
        break;
    case MON_RUN_TIMEOUT:
        set_error(err, "GenericError", "%s",
                  "timed out waiting for the machine to run the command");
        break;
    }
    return NULL;
}

static MonBlockReq *new_block_req(MonBlockOp op, QmpError *err) {
    MonBlockReq *r = calloc(1, sizeof(*r));
    if (!r) set_error(err, "GenericError", "%s", "out of memory");
    else    r->op = op;
    return r;
}

/* blockdev-add: {"driver": "file", "node-name": N, "filename": F} or a
 * format driver over a file child, {"driver": "raw", "node-name": N,
 * "file": {"driver": "file", "filename": F}}.  GEMU's devices detect image
 * formats themselves, so the format driver only has to be one we know. */
static cJSON *qmp_blockdev_add(QmpConn *c, const cJSON *args, QmpError *err) {
    static const char *const drivers[] = {
        "file", "host_cdrom", "host_device", "raw", "qcow2", "vpc", "vmdk", "vdi", "dmg",
    };
    MonBlockReq *r = new_block_req(MON_BLOCK_ADD, err);
    if (!r) return NULL;
    char driver[32];
    if (!str_arg(args, "driver", true, driver, sizeof(driver), err))
        return run_block(c, r, err);
    bool known = false;
    for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++)
        known |= strcmp(driver, drivers[i]) == 0;
    if (!known) {
        set_error(err, "GenericError", "Unsupported driver '%s'", driver);
        return run_block(c, r, err);
    }
    if (!str_arg(args, "node-name", true, r->node, sizeof(r->node), err) ||
        !bool_arg(args, "read-only", &r->read_only, err))
        return run_block(c, r, err);

    const cJSON *file = cJSON_GetObjectItemCaseSensitive(args, "file");
    if (strcmp(driver, "file") == 0 || strncmp(driver, "host_", 5) == 0)
        str_arg(args, "filename", true, r->file, sizeof(r->file), err);
    else if (cJSON_IsObject(file))
        str_arg(file, "filename", true, r->file, sizeof(r->file), err);
    else if (cJSON_IsString(file))
        set_error(err, "GenericError", "%s",
                  "'file' must be an inline {\"driver\": \"file\", \"filename\": ...} object");
    else
        set_error(err, "GenericError", "Parameter '%s' is missing", "file");
    return run_block(c, r, err);
}

static cJSON *qmp_blockdev_del(QmpConn *c, const cJSON *args, QmpError *err) {
    MonBlockReq *r = new_block_req(MON_BLOCK_DEL, err);
    if (!r) return NULL;
    str_arg(args, "node-name", true, r->node, sizeof(r->node), err);
    return run_block(c, r, err);
}

static cJSON *qmp_blockdev_open_tray(QmpConn *c, const cJSON *args, QmpError *err) {
    MonBlockReq *r = new_block_req(MON_BLOCK_OPEN_TRAY, err);
    if (!r) return NULL;
    bool force = false;   /* trays are never locked, so force changes nothing */
    if (device_arg(args, r, err)) bool_arg(args, "force", &force, err);
    return run_block(c, r, err);
}

static cJSON *qmp_blockdev_close_tray(QmpConn *c, const cJSON *args, QmpError *err) {
    MonBlockReq *r = new_block_req(MON_BLOCK_CLOSE_TRAY, err);
    if (!r) return NULL;
    device_arg(args, r, err);
    return run_block(c, r, err);
}

static cJSON *qmp_blockdev_remove_medium(QmpConn *c, const cJSON *args, QmpError *err) {
    MonBlockReq *r = new_block_req(MON_BLOCK_REMOVE_MEDIUM, err);
    if (!r) return NULL;
    device_arg(args, r, err);
    return run_block(c, r, err);
}

static cJSON *qmp_blockdev_insert_medium(QmpConn *c, const cJSON *args, QmpError *err) {
    MonBlockReq *r = new_block_req(MON_BLOCK_INSERT_MEDIUM, err);
    if (!r) return NULL;
    if (device_arg(args, r, err))
        str_arg(args, "node-name", true, r->node, sizeof(r->node), err);
    return run_block(c, r, err);
}

/* blockdev-change-medium: format and read-only-mode are accepted for
 * compatibility; devices detect formats and open media as they need. */
static cJSON *qmp_blockdev_change_medium(QmpConn *c, const cJSON *args, QmpError *err) {
    MonBlockReq *r = new_block_req(MON_BLOCK_CHANGE_MEDIUM, err);
    if (!r) return NULL;
    char format[32], mode[32];
    bool force = false;
    if (device_arg(args, r, err) &&
        str_arg(args, "filename", true, r->file, sizeof(r->file), err) &&
        bool_arg(args, "force", &force, err)) {
        str_arg(args, "format", false, format, sizeof(format), err);
        if (!err->klass && str_arg(args, "read-only-mode", false, mode, sizeof(mode), err) &&
            strcmp(mode, "retain") && strcmp(mode, "read-only") && strcmp(mode, "read-write"))
            set_error(err, "GenericError",
                      "Parameter 'read-only-mode' does not accept value '%s'", mode);
    }
    return run_block(c, r, err);
}

static const struct {
    const char *name;
    QmpFn       fn;
} qmp_cmds[] = {
    { "blockdev-add",            qmp_blockdev_add },
    { "blockdev-change-medium",  qmp_blockdev_change_medium },
    { "blockdev-close-tray",     qmp_blockdev_close_tray },
    { "blockdev-del",            qmp_blockdev_del },
    { "blockdev-insert-medium",  qmp_blockdev_insert_medium },
    { "blockdev-open-tray",      qmp_blockdev_open_tray },
    { "blockdev-remove-medium",  qmp_blockdev_remove_medium },
    { "cont",                  qmp_cont },
    { "expire_password",       qmp_expire_password },
    { "human-monitor-command", qmp_human_monitor_command },
    { "qmp_capabilities",      qmp_capabilities },
    { "query-commands",        qmp_query_commands },
    { "query-status",          qmp_query_status },
    { "query-version",         qmp_query_version },
    { "query-vnc",             qmp_query_vnc },
    { "quit",                  qmp_quit },
    { "set_password",          qmp_set_password },
    { "stop",                  qmp_stop },
    { "system_reset",          qmp_system_reset },
};

#define N_QMP_CMDS (sizeof(qmp_cmds) / sizeof(qmp_cmds[0]))

static cJSON *qmp_query_commands(QmpConn *c, const cJSON *args, QmpError *err) {
    (void)c; (void)args; (void)err;
    cJSON *list = cJSON_CreateArray();
    for (size_t i = 0; i < N_QMP_CMDS; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", qmp_cmds[i].name);
        cJSON_AddItemToArray(list, e);
    }
    return list;
}

/* ── Requests ────────────────────────────────────────────────────────────── */

static void qmp_handle_request(QmpConn *c, const cJSON *req) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");

    if (!cJSON_IsObject(req)) {
        qmp_send_error(c, "GenericError", "QMP input must be a JSON object", NULL);
        return;
    }
    const cJSON *member;
    cJSON_ArrayForEach(member, req) {
        if (strcmp(member->string, "execute") != 0 &&
            strcmp(member->string, "arguments") != 0 &&
            strcmp(member->string, "id") != 0) {
            char desc[160];
            snprintf(desc, sizeof(desc), "QMP input member '%s' is unexpected",
                     member->string);
            qmp_send_error(c, "GenericError", desc, id);
            return;
        }
    }

    const cJSON *execute = cJSON_GetObjectItemCaseSensitive(req, "execute");
    if (!execute) {
        qmp_send_error(c, "GenericError", "QMP input lacks member 'execute'", id);
        return;
    }
    if (!cJSON_IsString(execute)) {
        qmp_send_error(c, "GenericError", "QMP input member 'execute' must be a string", id);
        return;
    }
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "arguments");
    if (args && !cJSON_IsObject(args)) {
        qmp_send_error(c, "GenericError", "QMP input member 'arguments' must be an object", id);
        return;
    }

    const char *name = execute->valuestring;
    bool is_caps = strcmp(name, "qmp_capabilities") == 0;
    if (!c->negotiated && !is_caps) {
        qmp_send_error(c, "CommandNotFound",
                       "Expecting capabilities negotiation with 'qmp_capabilities'", id);
        return;
    }
    if (c->negotiated && is_caps) {
        qmp_send_error(c, "CommandNotFound",
                       "Capabilities negotiation is already complete, command ignored", id);
        return;
    }

    for (size_t i = 0; i < N_QMP_CMDS; i++) {
        if (strcmp(name, qmp_cmds[i].name) != 0) continue;
        QmpError err = { NULL, "" };
        cJSON *result = qmp_cmds[i].fn(c, args, &err);
        if (err.klass) {
            cJSON_Delete(result);
            qmp_send_error(c, err.klass, err.desc, id);
        } else {
            qmp_send_return(c, result, id);
        }
        return;
    }

    char desc[160];
    snprintf(desc, sizeof(desc), "The command %s has not been found", name);
    qmp_send_error(c, "CommandNotFound", desc, id);
}

/* ── Framing ─────────────────────────────────────────────────────────────── *
 *
 * A request can span several reads or lines, and one read can hold several
 * requests, so incoming bytes are split into top-level JSON values by
 * tracking nesting depth (outside of strings) before handing each complete
 * value to cJSON.  Text that doesn't start a JSON object or array is
 * reported once the whitespace after it arrives, like QEMU does.
 */

static void frame_reset(QmpConn *c) {
    c->len = 0;
    c->depth = 0;
    c->in_string = c->escape = false;
}

static bool frame_append(QmpConn *c, char ch) {
    if (c->len + 1 >= c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 1024;
        char *buf = realloc(c->buf, cap);
        if (!buf) return false;
        c->buf = buf;
        c->cap = cap;
    }
    c->buf[c->len++] = ch;
    c->buf[c->len] = '\0';
    return true;
}

static void frame_complete(QmpConn *c) {
    c->mon->replying = true;
    cJSON *req = cJSON_ParseWithLength(c->buf, c->len);
    if (req)
        qmp_handle_request(c, req);
    else
        qmp_send_error(c, "GenericError", "JSON parse error", NULL);
    cJSON_Delete(req);
    frame_reset(c);
    c->mon->replying = false;
}

static void qmp_feed(QmpConn *c, const char *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ch = data[i];
        bool space = isspace((unsigned char)ch);

        if (c->len == 0 && space)
            continue;   /* whitespace between values */

        /* A bare word at top level: collect it, then report it. */
        if (c->len > 0 && c->depth == 0 && c->buf[0] != '{' && c->buf[0] != '[') {
            if (!space) {
                if (c->len < 64) frame_append(c, ch);
                continue;
            }
            char desc[128];
            snprintf(desc, sizeof(desc), "JSON parse error, invalid keyword '%s'", c->buf);
            qmp_send_error(c, "GenericError", desc, NULL);
            frame_reset(c);
            continue;
        }

        if (c->len >= QMP_MAX_INPUT || !frame_append(c, ch)) {
            qmp_send_error(c, "GenericError", "JSON input too large", NULL);
            frame_reset(c);
            continue;
        }

        if (c->in_string) {
            if (c->escape)         c->escape = false;
            else if (ch == '\\')   c->escape = true;
            else if (ch == '"')    c->in_string = false;
        } else if (ch == '"') {
            c->in_string = true;
        } else if (ch == '{' || ch == '[') {
            c->depth++;
        } else if (ch == '}' || ch == ']') {
            if (--c->depth <= 0)
                frame_complete(c);   /* also handles a stray closing bracket */
        }
    }
}

/* ── Transports ──────────────────────────────────────────────────────────── */

void mon_qmp_stdio_loop(GemuMonitor *mon, FILE *out) {
    QmpConn c = { .mon = mon, .out = out };
    qmp_send_greeting(&c);
    char chunk[4096];
    while (mon->running && fgets(chunk, sizeof(chunk), stdin))
        qmp_feed(&c, chunk, strlen(chunk));
    free(c.buf);
}

void mon_qmp_socket_loop(GemuMonitor *mon) {
    while (mon->running) {
        if (!mon_accept_client(mon)) continue;

        QmpConn c = { .mon = mon };
        qmp_send_greeting(&c);
        char chunk[4096];
        int n;
        while (mon->running && (n = mon_recv_client(mon, chunk, sizeof(chunk))) > 0)
            qmp_feed(&c, chunk, (size_t)n);
        free(c.buf);
        mon_close_client(mon);
    }
}
