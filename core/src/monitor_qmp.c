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
    cJSON_AddNumberToObject(gemu, "major", 1);
    cJSON_AddNumberToObject(gemu, "minor", 0);
    cJSON_AddNumberToObject(gemu, "micro", 0);
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

static const struct {
    const char *name;
    QmpFn       fn;
} qmp_cmds[] = {
    { "cont",                  qmp_cont },
    { "human-monitor-command", qmp_human_monitor_command },
    { "qmp_capabilities",      qmp_capabilities },
    { "query-commands",        qmp_query_commands },
    { "query-status",          qmp_query_status },
    { "query-version",         qmp_query_version },
    { "quit",                  qmp_quit },
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
