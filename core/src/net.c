/* QEMU-style networking: -netdev / -nic / -net / -device MODEL,netdev=ID.
 * See gemu/net.h for the model.  Backends run on one I/O thread; every
 * backend call (slirp included) and every adapter queue is under net_lock. */

#include "gemu/net.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#  define NET_HOST_IO 1
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <pthread.h>
#  include <time.h>
#  include <unistd.h>
#  ifdef __linux__
#    include <linux/if.h>
#    include <linux/if_tun.h>
#    include <sys/ioctl.h>
#  endif
#  ifdef HAVE_SLIRP
#    include <slirp/libslirp.h>
#  endif
#else
#  undef HAVE_SLIRP
#endif

#define MAX_BACKENDS 16
#define MAX_NICS     16
#define MAX_FWD      16
#define MAX_SEARCH   8
#define RXQ_N        256
#define RXQ_FRAME    2048
#define MIN_FRAME    60

typedef enum { BE_USER, BE_TAP } BeType;

typedef struct {
    bool     udp;
    uint32_t haddr;               /* network order, 0 = any */
    int      hport;
    uint32_t gaddr;               /* network order, 0 = the DHCP start */
    int      gport;
} HostFwd;

typedef struct Backend {
    char    id[64];
    BeType  type;
    int     hub;                  /* legacy -net hub, -1 for a -netdev */
    bool    link_down;
    GemuNic *peer;                /* the adapter of a -netdev / -nic */
    /* user */
    uint32_t net, mask, host, dhcp, dns;   /* network order */
    bool     restricted, ipv4, ipv6;
    char     hostname[64], domainname[128], tftp[256], bootfile[128];
    char    *dnssearch[MAX_SEARCH + 1];
    HostFwd  fwd[MAX_FWD];
    int      n_fwd;
#ifdef HAVE_SLIRP
    Slirp   *slirp;
#endif
    /* tap */
    char     ifname[32];
    int      tapfd;
} Backend;

struct GemuNic {
    char     name[64];
    char     model[32];           /* "" until resolved: the default model */
    char     netdev[64];          /* -device/-net nic netdev= */
    uint8_t  mac[6];
    bool     has_mac;
    bool     from_device;         /* -device (names it with id=) */
    Backend *peer;
    int      hub;                 /* legacy hub, -1 none */
    bool     link_down;
    void   (*link_cb)(void *ud, bool up);
    void    *link_ud;
    uint8_t (*q)[RXQ_FRAME];
    uint16_t qlen[RXQ_N];
    unsigned qhead, qcount;
};

static Backend  backends[MAX_BACKENDS];
static int      n_backends;
static GemuNic  nic_store[MAX_NICS];
static GemuNic *nic_ptrs[MAX_NICS];
static int      n_nics;
static bool     none_requested, any_requested, help_requested;
static int      n_user_auto;      /* "user.N" names for unnamed backends */

/* ── Option strings ──────────────────────────────────────────────────────── */

typedef struct { char *key; char *val; } Opt;
typedef struct { Opt o[48]; int n; char buf[2048]; } Opts;

/* "a,b=c,d=e,,f": the first item may lack '=' (a type or model, returned as
 * key with val NULL); ",," is a literal comma, as in QEMU. */
static bool opts_split(const char *spec, Opts *out, const char *what) {
    memset(out, 0, sizeof(*out));
    if (strlen(spec) >= sizeof(out->buf)) {
        fprintf(stderr, "gemu: %s: option string too long\n", what);
        return false;
    }
    char *w = out->buf;
    const char *p = spec;
    while (*p) {
        if (out->n >= (int)(sizeof(out->o) / sizeof(out->o[0]))) {
            fprintf(stderr, "gemu: %s: too many options\n", what);
            return false;
        }
        Opt *o = &out->o[out->n++];
        o->key = w;
        while (*p) {
            if (*p == ',') {
                if (p[1] == ',') { *w++ = ','; p += 2; continue; }
                p++;
                break;
            }
            if (*p == '=' && !o->val) { *w++ = '\0'; o->val = w; p++; continue; }
            *w++ = *p++;
        }
        *w++ = '\0';
    }
    return true;
}

static const char *opt_take(Opts *o, const char *key) {
    for (int i = 0; i < o->n; i++) {
        if (o->o[i].key && o->o[i].val && strcmp(o->o[i].key, key) == 0) {
            const char *v = o->o[i].val;
            o->o[i].key = NULL;           /* consumed */
            return v;
        }
    }
    return NULL;
}

static bool opt_bool(const char *v, bool *out, const char *what, const char *key) {
    if (!strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "y") ||
        !strcmp(v, "true") || !strcmp(v, "1")) { *out = true; return true; }
    if (!strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "n") ||
        !strcmp(v, "false") || !strcmp(v, "0")) { *out = false; return true; }
    fprintf(stderr, "gemu: %s: %s=%s: expected on or off\n", what, key, v);
    return false;
}

static bool opts_leftover(const Opts *o, int from, const char *what) {
    for (int i = from; i < o->n; i++) {
        if (!o->o[i].key) continue;
        fprintf(stderr, "gemu: %s: unknown option '%s'\n", what, o->o[i].key);
        return false;
    }
    return true;
}

static bool parse_mac(const char *s, uint8_t mac[6]) {
    unsigned v[6];
    char tail;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &v[0], &v[1], &v[2], &v[3], &v[4],
               &v[5], &tail) != 6 &&
        sscanf(s, "%x-%x-%x-%x-%x-%x%c", &v[0], &v[1], &v[2], &v[3], &v[4],
               &v[5], &tail) != 6)
        return false;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) return false;
        mac[i] = (uint8_t)v[i];
    }
    return !(mac[0] & 1);          /* not a multicast address */
}

static bool parse_ip4(const char *s, uint32_t *out) {
#ifdef NET_HOST_IO
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) return false;
    *out = a.s_addr;
    return true;
#else
    unsigned b[4];
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3], &tail) != 4) return false;
    for (int i = 0; i < 4; i++) if (b[i] > 255) return false;
    uint8_t *p = (uint8_t *)out;
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)b[i];
    return true;
#endif
}

static uint32_t be32(uint32_t host_order) {
    uint8_t b[4] = { (uint8_t)(host_order >> 24), (uint8_t)(host_order >> 16),
                     (uint8_t)(host_order >> 8), (uint8_t)host_order };
    uint32_t v;
    memcpy(&v, b, 4);
    return v;
}
static uint32_t from_be32(uint32_t net_order) {
    uint8_t b[4];
    memcpy(b, &net_order, 4);
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static void ip4_str(uint32_t a, char *buf, size_t cap) {
    uint32_t h = from_be32(a);
    snprintf(buf, cap, "%u.%u.%u.%u", h >> 24, (h >> 16) & 0xFF, (h >> 8) & 0xFF,
             h & 0xFF);
}

/* "[tcp|udp]:[hostaddr]:hostport-[guestaddr]:guestport" */
static bool parse_hostfwd(const char *s, HostFwd *f) {
    char buf[160];
    if (strlen(s) >= sizeof(buf)) return false;
    strcpy(buf, s);
    memset(f, 0, sizeof(*f));
    char *p = buf;
    char *c = strchr(p, ':');
    if (!c) return false;
    *c = '\0';
    if (!strcmp(p, "udp")) f->udp = true;
    else if (strcmp(p, "tcp") && *p) return false;
    p = c + 1;
    char *dash = strchr(p, '-');
    if (!dash) return false;
    *dash = '\0';
    char *guest = dash + 1;
    c = strrchr(p, ':');
    if (!c) return false;
    *c = '\0';
    if (*p && !parse_ip4(p, &f->haddr)) return false;
    char *end;
    long hp = strtol(c + 1, &end, 10);
    if (*end || hp < 0 || hp > 65535 || end == c + 1) return false;
    f->hport = (int)hp;
    c = strrchr(guest, ':');
    if (!c) return false;
    *c = '\0';
    if (*guest && !parse_ip4(guest, &f->gaddr)) return false;
    long gp = strtol(c + 1, &end, 10);
    if (*end || gp <= 0 || gp > 65535) return false;
    f->gport = (int)gp;
    return true;
}

/* ── Backends ────────────────────────────────────────────────────────────── */

static Backend *find_backend(const char *id) {
    for (int i = 0; i < n_backends; i++)
        if (!strcmp(backends[i].id, id)) return &backends[i];
    return NULL;
}

static GemuNic *find_nic(const char *name) {
    for (int i = 0; i < n_nics; i++)
        if (!strcmp(nic_store[i].name, name)) return &nic_store[i];
    return NULL;
}

/* Creates a backend of `type` from the remaining options in `o` (index 0,
 * the type, already taken).  `id` NULL names it "type.N". */
static Backend *backend_new(const char *type, Opts *o, const char *id, int hub,
                            const char *what) {
    BeType t;
    if (!strcmp(type, "user")) {
#ifndef HAVE_SLIRP
        fprintf(stderr, "gemu: %s: user-mode networking is not in this build "
                "(configure did not find libslirp)\n", what);
        return NULL;
#endif
        t = BE_USER;
    } else if (!strcmp(type, "tap")) {
#if !defined(__linux__)
        fprintf(stderr, "gemu: %s: tap is only supported on Linux\n", what);
        return NULL;
#endif
        t = BE_TAP;
    } else {
        fprintf(stderr, "gemu: %s: unsupported network backend '%s' "
                "(supported: user, tap)\n", what, type);
        return NULL;
    }
    if (n_backends >= MAX_BACKENDS) {
        fprintf(stderr, "gemu: %s: too many network backends\n", what);
        return NULL;
    }
    if (id && find_backend(id)) {
        fprintf(stderr, "gemu: %s: duplicate network backend id '%s'\n", what, id);
        return NULL;
    }
    Backend *b = &backends[n_backends];
    memset(b, 0, sizeof(*b));
    b->type = t;
    b->hub = hub;
    b->tapfd = -1;
    if (id) {
        snprintf(b->id, sizeof(b->id), "%s", id);
    } else {
        int k = 0;
        for (int i = 0; i < n_backends; i++) k += backends[i].type == t;
        snprintf(b->id, sizeof(b->id), "%s.%d", type, k);
        if (find_backend(b->id))
            snprintf(b->id, sizeof(b->id), "%s.%d", type, n_backends + n_user_auto++);
    }

    const char *v;
    if (t == BE_USER) {
        /* QEMU's defaults: 10.0.2.0/24, gateway .2, DNS .3, DHCP from .15. */
        b->net = be32(0x0A000200u);
        b->mask = be32(0xFFFFFF00u);
        b->ipv4 = b->ipv6 = true;
        bool has_host = false, has_dhcp = false, has_dns = false;
        if ((v = opt_take(o, "net"))) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s", v);
            char *slash = strchr(buf, '/');
            if (slash) *slash++ = '\0';
            if (!parse_ip4(buf, &b->net)) goto bad_net;
            if (slash) {
                char *end;
                long bits = strtol(slash, &end, 10);
                if (!*end && end != slash) {
                    if (bits < 1 || bits > 30) goto bad_net;
                    b->mask = be32(bits == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> bits));
                } else if (!parse_ip4(slash, &b->mask)) {
                    goto bad_net;
                }
            }
            b->net &= b->mask;
        }
        if ((v = opt_take(o, "host"))) {
            if (!parse_ip4(v, &b->host)) { fprintf(stderr, "gemu: %s: bad host=%s\n", what, v); return NULL; }
            has_host = true;
        }
        if ((v = opt_take(o, "dhcpstart"))) {
            if (!parse_ip4(v, &b->dhcp)) { fprintf(stderr, "gemu: %s: bad dhcpstart=%s\n", what, v); return NULL; }
            has_dhcp = true;
        }
        if ((v = opt_take(o, "dns"))) {
            if (!parse_ip4(v, &b->dns)) { fprintf(stderr, "gemu: %s: bad dns=%s\n", what, v); return NULL; }
            has_dns = true;
        }
        uint32_t base = from_be32(b->net);
        if (!has_host) b->host = be32(base | 2);
        if (!has_dhcp) b->dhcp = be32(base | 15);
        if (!has_dns)  b->dns  = be32(base | 3);
        if ((v = opt_take(o, "restrict")) && !opt_bool(v, &b->restricted, what, "restrict")) return NULL;
        if ((v = opt_take(o, "ipv4")) && !opt_bool(v, &b->ipv4, what, "ipv4")) return NULL;
        if ((v = opt_take(o, "ipv6")) && !opt_bool(v, &b->ipv6, what, "ipv6")) return NULL;
        if ((v = opt_take(o, "hostname")))   snprintf(b->hostname, sizeof(b->hostname), "%s", v);
        if ((v = opt_take(o, "domainname"))) snprintf(b->domainname, sizeof(b->domainname), "%s", v);
        if ((v = opt_take(o, "tftp")))       snprintf(b->tftp, sizeof(b->tftp), "%s", v);
        if ((v = opt_take(o, "bootfile")))   snprintf(b->bootfile, sizeof(b->bootfile), "%s", v);
        int ns = 0;
        while ((v = opt_take(o, "dnssearch"))) {
            if (ns >= MAX_SEARCH) { fprintf(stderr, "gemu: %s: too many dnssearch=\n", what); return NULL; }
            b->dnssearch[ns++] = strdup(v);
        }
        while ((v = opt_take(o, "hostfwd"))) {
            if (b->n_fwd >= MAX_FWD) { fprintf(stderr, "gemu: %s: too many hostfwd=\n", what); return NULL; }
            if (!parse_hostfwd(v, &b->fwd[b->n_fwd])) {
                fprintf(stderr, "gemu: %s: bad hostfwd=%s (expected "
                        "[tcp|udp]:[hostaddr]:hostport-[guestaddr]:guestport)\n", what, v);
                return NULL;
            }
            b->n_fwd++;
        }
    } else {
        if ((v = opt_take(o, "ifname"))) snprintf(b->ifname, sizeof(b->ifname), "%s", v);
        if ((v = opt_take(o, "fd"))) {
            char *end;
            long fd = strtol(v, &end, 10);
            if (*end || fd < 0) { fprintf(stderr, "gemu: %s: bad fd=%s\n", what, v); return NULL; }
            b->tapfd = (int)fd;
        }
        const char *keys[] = { "script", "downscript" };
        for (int k = 0; k < 2; k++) {
            if ((v = opt_take(o, keys[k])) && strcmp(v, "no") != 0) {
                fprintf(stderr, "gemu: %s: %s=%s: gemu does not run interface scripts; "
                        "set the interface up yourself and use %s=no\n",
                        what, keys[k], v, keys[k]);
                return NULL;
            }
        }
    }
    if (!opts_leftover(o, 0, what)) return NULL;
    n_backends++;
    return b;

bad_net:
    fprintf(stderr, "gemu: %s: bad net= (expected ADDR[/MASK])\n", what);
    return NULL;
}

static GemuNic *nic_new(const char *what) {
    if (n_nics >= MAX_NICS) {
        fprintf(stderr, "gemu: %s: too many network adapters\n", what);
        return NULL;
    }
    GemuNic *n = &nic_store[n_nics++];
    memset(n, 0, sizeof(*n));
    n->hub = -1;
    return n;
}

/* model= / mac= shared by -nic and -net nic ("macaddr" there). */
static bool nic_common(GemuNic *n, Opts *o, const char *mac_key, const char *what) {
    const char *v;
    if ((v = opt_take(o, "model"))) {
        if (!strcmp(v, "help") || !strcmp(v, "?")) help_requested = true;
        else snprintf(n->model, sizeof(n->model), "%s", v);
    }
    if ((v = opt_take(o, mac_key))) {
        if (!parse_mac(v, n->mac)) {
            fprintf(stderr, "gemu: %s: bad %s=%s (expected a unicast "
                    "xx:xx:xx:xx:xx:xx)\n", what, mac_key, v);
            return false;
        }
        n->has_mac = true;
    }
    return true;
}

bool gemu_net_parse_netdev(const char *spec) {
    Opts o;
    if (!opts_split(spec, &o, "-netdev")) return false;
    if (!o.n || o.o[0].val) {
        fprintf(stderr, "gemu: -netdev: expected TYPE,id=ID[,...] (TYPE: user, tap)\n");
        return false;
    }
    const char *type = o.o[0].key;
    o.o[0].key = NULL;
    const char *id = opt_take(&o, "id");
    if (!id || !*id) {
        fprintf(stderr, "gemu: -netdev %s: id= is required\n", type);
        return false;
    }
    any_requested = true;
    return backend_new(type, &o, id, -1, "-netdev") != NULL;
}

bool gemu_net_parse_nic(const char *spec) {
    Opts o;
    if (!opts_split(spec, &o, "-nic")) return false;
    const char *type = "user";
    if (o.n && !o.o[0].val) {
        type = o.o[0].key;
        o.o[0].key = NULL;
    }
    if (!strcmp(type, "none")) {
        none_requested = true;
        return opts_leftover(&o, 0, "-nic none");
    }
    any_requested = true;
    GemuNic *n = nic_new("-nic");
    if (!n || !nic_common(n, &o, "mac", "-nic")) return false;
    const char *id = opt_take(&o, "id");
    Backend *b = backend_new(type, &o, id, -1, "-nic");
    if (!b) return false;
    b->peer = n;
    n->peer = b;
    return true;
}

bool gemu_net_parse_net(const char *spec) {
    Opts o;
    if (!opts_split(spec, &o, "-net")) return false;
    if (!o.n || o.o[0].val) {
        fprintf(stderr, "gemu: -net: expected nic|user|tap|none[,...]\n");
        return false;
    }
    const char *type = o.o[0].key;
    o.o[0].key = NULL;
    if (!strcmp(type, "none")) {
        none_requested = true;
        return opts_leftover(&o, 0, "-net none");
    }
    any_requested = true;
    int hub = 0;
    const char *v;
    if ((v = opt_take(&o, "vlan"))) {
        char *end;
        long h = strtol(v, &end, 10);
        if (*end || h < 0 || h > 255) { fprintf(stderr, "gemu: -net: bad vlan=%s\n", v); return false; }
        hub = (int)h;
    }
    const char *name = opt_take(&o, "name");
    if (!strcmp(type, "nic")) {
        GemuNic *n = nic_new("-net nic");
        if (!n || !nic_common(n, &o, "macaddr", "-net nic")) return false;
        if ((v = opt_take(&o, "netdev"))) snprintf(n->netdev, sizeof(n->netdev), "%s", v);
        else n->hub = hub;
        if (name) snprintf(n->name, sizeof(n->name), "%s", name);
        return opts_leftover(&o, 0, "-net nic");
    }
    return backend_new(type, &o, name, hub, "-net") != NULL;
}

bool gemu_net_device_model(const char *spec, char *model, size_t cap) {
    size_t n = strcspn(spec, ",");
    if (n == 0 || n >= cap) return false;
    memcpy(model, spec, n);
    model[n] = '\0';
    return true;
}

bool gemu_net_parse_device(const char *spec) {
    Opts o;
    if (!opts_split(spec, &o, "-device")) return false;
    any_requested = true;
    GemuNic *n = nic_new("-device");
    if (!n) return false;
    snprintf(n->model, sizeof(n->model), "%s", o.o[0].key);
    o.o[0].key = NULL;
    n->from_device = true;
    char what[64];
    snprintf(what, sizeof(what), "-device %s", n->model);
    const char *v;
    if ((v = opt_take(&o, "mac"))) {
        if (!parse_mac(v, n->mac)) {
            fprintf(stderr, "gemu: %s: bad mac=%s\n", what, v);
            return false;
        }
        n->has_mac = true;
    }
    if ((v = opt_take(&o, "netdev"))) snprintf(n->netdev, sizeof(n->netdev), "%s", v);
    if ((v = opt_take(&o, "id")))     snprintf(n->name, sizeof(n->name), "%s", v);
    return opts_leftover(&o, 0, what);
}

bool gemu_net_requested(void) { return any_requested; }

/* ── Frame paths (net_lock held) ─────────────────────────────────────────── */

#ifdef NET_HOST_IO
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       net_thread;
static bool            net_thread_up;
static volatile bool   net_stop;
static int             wake_pipe[2] = { -1, -1 };
#  define LOCK()   pthread_mutex_lock(&net_lock)
#  define UNLOCK() pthread_mutex_unlock(&net_lock)
static void net_wake(void) {
    if (wake_pipe[1] >= 0) {
        char c = 0;
        ssize_t r = write(wake_pipe[1], &c, 1);
        (void)r;
    }
}
#else
#  define LOCK()   ((void)0)
#  define UNLOCK() ((void)0)
static void net_wake(void) {}
#endif

static void nic_enqueue(GemuNic *n, const uint8_t *f, size_t len) {
    if (n->link_down || len > RXQ_FRAME || !n->q) return;
    if (n->qcount >= RXQ_N) return;                  /* overrun: dropped */
    unsigned slot = (n->qhead + n->qcount) % RXQ_N;
    memcpy(n->q[slot], f, len);
    if (len < MIN_FRAME) {
        memset(n->q[slot] + len, 0, MIN_FRAME - len);
        len = MIN_FRAME;
    }
    n->qlen[slot] = (uint16_t)len;
    n->qcount++;
}

static void backend_send(Backend *b, const uint8_t *f, size_t len) {
    if (b->link_down) return;
#ifdef HAVE_SLIRP
    if (b->type == BE_USER && b->slirp) {
        slirp_input(b->slirp, f, (int)len);
        net_wake();
        return;
    }
#endif
#ifdef NET_HOST_IO
    if (b->type == BE_TAP && b->tapfd >= 0) {
        ssize_t r = write(b->tapfd, f, len);
        (void)r;
    }
#endif
}

/* A frame from the outside world. */
static void backend_deliver(Backend *b, const uint8_t *f, size_t len) {
    if (b->link_down) return;
    if (b->peer) { nic_enqueue(b->peer, f, len); return; }
    if (b->hub < 0) return;
    for (int i = 0; i < n_nics; i++)
        if (nic_store[i].hub == b->hub) nic_enqueue(&nic_store[i], f, len);
}

const char *gemu_nic_model(const GemuNic *n) { return n->model; }
const char *gemu_nic_name(const GemuNic *n)  { return n->name; }
void gemu_nic_mac(const GemuNic *n, uint8_t mac[6]) { memcpy(mac, n->mac, 6); }
bool gemu_nic_link_up(const GemuNic *n) { return !n->link_down; }

void gemu_nic_set_link_cb(GemuNic *n, void (*cb)(void *, bool), void *ud) {
    n->link_cb = cb;
    n->link_ud = ud;
}

bool gemu_nic_send(GemuNic *n, const uint8_t *f, size_t len) {
    LOCK();
    if (!n->link_down) {
        if (n->peer) {
            backend_send(n->peer, f, len);
        } else if (n->hub >= 0) {
            for (int i = 0; i < n_backends; i++)
                if (backends[i].hub == n->hub) backend_send(&backends[i], f, len);
            for (int i = 0; i < n_nics; i++)
                if (&nic_store[i] != n && nic_store[i].hub == n->hub)
                    nic_enqueue(&nic_store[i], f, len);
        }
    }
    UNLOCK();
    return true;
}

size_t gemu_nic_recv(GemuNic *n, uint8_t *frame, size_t cap) {
    if (!n->qcount) return 0;                        /* racy peek, fine */
    size_t len = 0;
    LOCK();
    while (n->qcount) {
        unsigned slot = n->qhead;
        len = n->qlen[slot];
        n->qhead = (n->qhead + 1) % RXQ_N;
        n->qcount--;
        if (len <= cap) { memcpy(frame, n->q[slot], len); break; }
        len = 0;                                     /* too big for the device */
    }
    UNLOCK();
    return len;
}

/* ── Backend thread ──────────────────────────────────────────────────────── */

#ifdef NET_HOST_IO
static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

typedef struct { struct pollfd *fds; int n, cap; } PollSet;

static int pollset_add(PollSet *ps, int fd, short events) {
    if (ps->n == ps->cap) {
        int cap = ps->cap ? ps->cap * 2 : 32;
        struct pollfd *nf = realloc(ps->fds, (size_t)cap * sizeof(*nf));
        if (!nf) return -1;
        ps->fds = nf;
        ps->cap = cap;
    }
    ps->fds[ps->n] = (struct pollfd){ .fd = fd, .events = events };
    return ps->n++;
}

#ifdef HAVE_SLIRP
typedef struct SlirpTimer {
    Backend *b;
    SlirpTimerId id;
    void *cb_opaque;
    int64_t expire_ms;                               /* -1 = not armed */
    struct SlirpTimer *next;
} SlirpTimer;
static SlirpTimer *timers;

static slirp_ssize_t sl_send(const void *buf, size_t len, void *opaque) {
    backend_deliver(opaque, buf, len);
    return (slirp_ssize_t)len;
}
static void sl_guest_error(const char *msg, void *opaque) {
    (void)opaque;
    fprintf(stderr, "gemu: user network: guest error: %s\n", msg);
}
static int64_t sl_clock(void *opaque) { (void)opaque; return now_ns(); }
static void *sl_timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque) {
    SlirpTimer *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->b = opaque;
    t->id = id;
    t->cb_opaque = cb_opaque;
    t->expire_ms = -1;
    t->next = timers;
    timers = t;
    return t;
}
static void sl_timer_free(void *timer, void *opaque) {
    (void)opaque;
    for (SlirpTimer **pp = &timers; *pp; pp = &(*pp)->next) {
        if (*pp == timer) { *pp = ((SlirpTimer *)timer)->next; free(timer); return; }
    }
}
static void sl_timer_mod(void *timer, int64_t expire_ms, void *opaque) {
    (void)opaque;
    ((SlirpTimer *)timer)->expire_ms = expire_ms;
    net_wake();
}
static void sl_notify(void *opaque) { (void)opaque; net_wake(); }
#if SLIRP_CONFIG_VERSION_MAX >= 6
static void sl_reg_sock(slirp_os_socket s, void *opaque) { (void)s; (void)opaque; }
#endif

static int sl_add_poll(slirp_os_socket fd, int events, void *opaque) {
    short ev = 0;
    if (events & SLIRP_POLL_IN)  ev |= POLLIN;
    if (events & SLIRP_POLL_OUT) ev |= POLLOUT;
    if (events & SLIRP_POLL_PRI) ev |= POLLPRI;
    return pollset_add(opaque, (int)fd, ev);
}
static int sl_get_revents(int idx, void *opaque) {
    PollSet *ps = opaque;
    if (idx < 0 || idx >= ps->n) return 0;
    short r = ps->fds[idx].revents;
    int ev = 0;
    if (r & POLLIN)  ev |= SLIRP_POLL_IN;
    if (r & POLLOUT) ev |= SLIRP_POLL_OUT;
    if (r & POLLPRI) ev |= SLIRP_POLL_PRI;
    if (r & POLLERR) ev |= SLIRP_POLL_ERR;
    if (r & POLLHUP) ev |= SLIRP_POLL_HUP;
    return ev;
}

static const SlirpCb sl_cb = {
    .send_packet = sl_send,
    .guest_error = sl_guest_error,
    .clock_get_ns = sl_clock,
    .timer_free = sl_timer_free,
    .timer_mod = sl_timer_mod,
    .notify = sl_notify,
    .timer_new_opaque = sl_timer_new_opaque,
#if SLIRP_CONFIG_VERSION_MAX >= 6
    .register_poll_socket = sl_reg_sock,
    .unregister_poll_socket = sl_reg_sock,
#endif
};

static bool user_start(Backend *b) {
    SlirpConfig c;
    memset(&c, 0, sizeof(c));
#if SLIRP_CONFIG_VERSION_MAX >= 6
    c.version = 6;
#else
    c.version = 4;
#endif
    c.restricted = b->restricted;
    c.in_enabled = b->ipv4;
    c.vnetwork.s_addr = b->net;
    c.vnetmask.s_addr = b->mask;
    c.vhost.s_addr = b->host;
    c.vdhcp_start.s_addr = b->dhcp;
    c.vnameserver.s_addr = b->dns;
    c.in6_enabled = b->ipv6;
    inet_pton(AF_INET6, "fec0::", &c.vprefix_addr6);
    c.vprefix_len = 64;
    inet_pton(AF_INET6, "fec0::2", &c.vhost6);
    inet_pton(AF_INET6, "fec0::3", &c.vnameserver6);
    c.vhostname = b->hostname[0] ? b->hostname : NULL;
    c.vdomainname = b->domainname[0] ? b->domainname : NULL;
    c.tftp_path = b->tftp[0] ? b->tftp : NULL;
    c.bootfile = b->bootfile[0] ? b->bootfile : NULL;
    c.vdnssearch = b->dnssearch[0] ? (const char **)b->dnssearch : NULL;
    b->slirp = slirp_new(&c, &sl_cb, b);
    if (!b->slirp) {
        fprintf(stderr, "gemu: netdev %s: could not start user-mode networking\n", b->id);
        return false;
    }
    for (int i = 0; i < b->n_fwd; i++) {
        const HostFwd *f = &b->fwd[i];
        struct in_addr ha = { .s_addr = f->haddr };
        struct in_addr ga = { .s_addr = f->gaddr ? f->gaddr : b->dhcp };
        if (slirp_add_hostfwd(b->slirp, f->udp, ha, f->hport, ga, f->gport) < 0) {
            fprintf(stderr, "gemu: netdev %s: could not forward host %s port %d "
                    "(in use?)\n", b->id, f->udp ? "udp" : "tcp", f->hport);
            return false;
        }
    }
    return true;
}
#endif /* HAVE_SLIRP */

static bool tap_start(Backend *b) {
#ifdef __linux__
    if (b->tapfd < 0) {
        int fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "gemu: netdev %s: /dev/net/tun: %s\n", b->id, strerror(errno));
            return false;
        }
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
        if (b->ifname[0]) snprintf(ifr.ifr_name, IFNAMSIZ, "%s", b->ifname);
        if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
            fprintf(stderr, "gemu: netdev %s: cannot open tap%s%s: %s%s\n", b->id,
                    b->ifname[0] ? " " : "", b->ifname, strerror(errno),
                    errno == EPERM ? " (create it first: ip tuntap add NAME mode tap user $USER)" : "");
            close(fd);
            return false;
        }
        snprintf(b->ifname, sizeof(b->ifname), "%s", ifr.ifr_name);
        b->tapfd = fd;
    }
    fcntl(b->tapfd, F_SETFL, fcntl(b->tapfd, F_GETFL) | O_NONBLOCK);
    return true;
#else
    (void)b;
    return false;
#endif
}

static void *net_thread_main(void *arg) {
    (void)arg;
    PollSet ps = { 0 };
    uint8_t buf[RXQ_FRAME + 64];
    while (!net_stop) {
        LOCK();
        ps.n = 0;
        pollset_add(&ps, wake_pipe[0], POLLIN);
        int tap_idx[MAX_BACKENDS];
        uint32_t timeout = 1000;
        for (int i = 0; i < n_backends; i++) {
            Backend *b = &backends[i];
            tap_idx[i] = b->type == BE_TAP && b->tapfd >= 0
                       ? pollset_add(&ps, b->tapfd, POLLIN) : -1;
#ifdef HAVE_SLIRP
            if (b->slirp) slirp_pollfds_fill_socket(b->slirp, &timeout, sl_add_poll, &ps);
#endif
        }
#ifdef HAVE_SLIRP
        int64_t now = now_ns() / 1000000;
        for (SlirpTimer *t = timers; t; t = t->next) {
            if (t->expire_ms < 0) continue;
            int64_t d = t->expire_ms - now;
            if (d < 0) d = 0;
            if ((uint64_t)d < timeout) timeout = (uint32_t)d;
        }
#endif
        UNLOCK();

        int r = poll(ps.fds, (nfds_t)ps.n, (int)timeout);

        LOCK();
        if (ps.fds[0].revents & POLLIN) {
            char drain[64];
            while (read(wake_pipe[0], drain, sizeof(drain)) > 0) {}
        }
        for (int i = 0; i < n_backends; i++) {
            Backend *b = &backends[i];
            if (tap_idx[i] >= 0 && (ps.fds[tap_idx[i]].revents & POLLIN)) {
                for (int k = 0; k < 64; k++) {
                    ssize_t len = read(b->tapfd, buf, sizeof(buf));
                    if (len <= 0) break;
                    backend_deliver(b, buf, (size_t)len);
                }
            }
#ifdef HAVE_SLIRP
            if (b->slirp) slirp_pollfds_poll(b->slirp, r < 0, sl_get_revents, &ps);
#endif
        }
#ifdef HAVE_SLIRP
        now = now_ns() / 1000000;
        for (SlirpTimer *t = timers, *next; t; t = next) {
            next = t->next;                          /* a handler may free t */
            if (t->expire_ms >= 0 && t->expire_ms <= now) {
                t->expire_ms = -1;
                slirp_handle_timer(t->b->slirp, t->id, t->cb_opaque);
            }
        }
#endif
        UNLOCK();
        (void)r;
    }
    free(ps.fds);
    return NULL;
}

static bool backends_start(void) {
    if (!n_backends) return true;
    for (int i = 0; i < n_backends; i++) {
        Backend *b = &backends[i];
#ifdef HAVE_SLIRP
        if (b->type == BE_USER && !user_start(b)) return false;
#endif
        if (b->type == BE_TAP && !tap_start(b)) return false;
    }
    if (pipe(wake_pipe) < 0) {
        fprintf(stderr, "gemu: network: pipe: %s\n", strerror(errno));
        return false;
    }
    for (int k = 0; k < 2; k++) {
        fcntl(wake_pipe[k], F_SETFL, fcntl(wake_pipe[k], F_GETFL) | O_NONBLOCK);
        fcntl(wake_pipe[k], F_SETFD, FD_CLOEXEC);
    }
    if (pthread_create(&net_thread, NULL, net_thread_main, NULL) != 0) {
        fprintf(stderr, "gemu: network: cannot start the I/O thread\n");
        return false;
    }
    net_thread_up = true;
    return true;
}
#else
static bool backends_start(void) {
    if (n_backends) {
        fprintf(stderr, "gemu: network backends are not supported in this build\n");
        return false;
    }
    return true;
}
#endif /* NET_HOST_IO */

void gemu_net_shutdown(void) {
#ifdef NET_HOST_IO
    if (net_thread_up) {
        net_stop = true;
        net_wake();
        pthread_join(net_thread, NULL);
        net_thread_up = false;
    }
    for (int i = 0; i < n_backends; i++) {
        Backend *b = &backends[i];
#ifdef HAVE_SLIRP
        if (b->slirp) { slirp_cleanup(b->slirp); b->slirp = NULL; }
#endif
        if (b->tapfd >= 0) { close(b->tapfd); b->tapfd = -1; }
    }
#endif
}

/* ── Resolution ──────────────────────────────────────────────────────────── */

static bool model_known(const GemuDevDesc *models, int n, const char *m) {
    for (int i = 0; i < n; i++)
        if (!strcmp(models[i].name, m)) return true;
    return false;
}

bool gemu_net_resolve(const GemuDevDesc *models, int n_models,
                      const char *default_model, GemuNic ***out, int *n_out) {
    *out = nic_ptrs;
    *n_out = 0;
    if (help_requested) {
        if (!n_models) {
            printf("This machine has no network adapters.\n");
        } else {
            gemu_print_table("Network adapter models", models, n_models);
        }
        exit(0);
    }
    if (!n_models) {
        if (any_requested) {
            fprintf(stderr, "gemu: this machine has no network adapters "
                    "(only -nic none / -net none apply)\n");
            return false;
        }
        return true;
    }

    /* QEMU: any -net, -nic or -netdev (and here -device NIC) replaces the
     * default adapter; -nic none / -net none / -nodefaults remove it. */
    if (!any_requested && !none_requested && default_model) {
        GemuNic *n = nic_new("default network adapter");
        if (!n) return false;
#ifdef HAVE_SLIRP
        Opts o = { 0 };
        o.n = 1;                                     /* the "type" slot, taken */
        Backend *b = backend_new("user", &o, NULL, -1, "default network");
        if (!b) return false;
        b->peer = n;
        n->peer = b;
#else
        fprintf(stderr, "gemu: note: no user-mode networking in this build "
                "(libslirp); the network adapter has nothing attached\n");
#endif
    }

    for (int i = 0; i < n_nics; i++) {
        GemuNic *n = &nic_store[i];
        if (!n->model[0])
            snprintf(n->model, sizeof(n->model), "%s",
                     default_model ? default_model : models[0].name);
        if (!model_known(models, n_models, n->model)) {
            fprintf(stderr, "gemu: unsupported network adapter model '%s' "
                    "(use -nic model=help to list)\n", n->model);
            return false;
        }
        if (n->netdev[0]) {
            Backend *b = find_backend(n->netdev);
            if (!b || b->hub >= 0) {
                fprintf(stderr, "gemu: %s: no -netdev with id '%s'\n",
                        n->model, n->netdev);
                return false;
            }
            if (b->peer) {
                fprintf(stderr, "gemu: netdev '%s' is already attached to "
                        "another network adapter\n", n->netdev);
                return false;
            }
            b->peer = n;
            n->peer = b;
        }
        if (!n->name[0]) {
            int k = 0;
            for (int j = 0; j < i; j++) k += !strcmp(nic_store[j].model, n->model);
            snprintf(n->name, sizeof(n->name), "%s.%d", n->model, k);
        }
        for (int j = 0; j < i; j++) {
            if (!strcmp(nic_store[j].name, n->name)) {
                fprintf(stderr, "gemu: duplicate network adapter id '%s'\n", n->name);
                return false;
            }
        }
        if (!n->has_mac) {
            static const uint8_t base[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
            memcpy(n->mac, base, 6);
            n->mac[5] = (uint8_t)(base[5] + i);
        }
        n->q = calloc(RXQ_N, RXQ_FRAME);
        if (!n->q) { fprintf(stderr, "gemu: out of memory\n"); return false; }
        nic_ptrs[i] = n;
    }

    /* QEMU's warnings for half-built connections. */
    for (int i = 0; i < n_backends; i++) {
        Backend *b = &backends[i];
        if (b->hub < 0 && !b->peer)
            fprintf(stderr, "gemu: warning: netdev %s has no network adapter "
                    "(add -device MODEL,netdev=%s)\n", b->id, b->id);
    }
    for (int h = 0; h < 256; h++) {
        bool nic = false, be = false;
        for (int i = 0; i < n_nics; i++) nic |= nic_store[i].hub == h;
        for (int i = 0; i < n_backends; i++) be |= backends[i].hub == h;
        if (nic && !be)
            fprintf(stderr, "gemu: warning: -net hub %d has a network adapter but no "
                    "host backend, so nothing reaches the outside (add -net user)\n", h);
        if (be && !nic)
            fprintf(stderr, "gemu: warning: -net hub %d has a host backend but no "
                    "network adapter (add -net nic)\n", h);
    }

    if (!backends_start()) return false;
    *n_out = n_nics;
    return true;
}

/* ── Monitor ─────────────────────────────────────────────────────────────── */

static void backend_desc(const Backend *b, char *buf, size_t cap) {
    if (b->type == BE_USER) {
        char net[20];
        ip4_str(b->net, net, sizeof(net));
        snprintf(buf, cap, "type=user,net=%s,restrict=%s", net,
                 b->restricted ? "on" : "off");
    } else {
        snprintf(buf, cap, "type=tap,ifname=%s", b->ifname);
    }
}

void gemu_net_info(void (*print)(void *, const char *), void *ud) {
    char line[256], d[160];
    if (!n_nics && !n_backends) { print(ud, "No network devices.\n"); return; }
    LOCK();
    for (int i = 0; i < n_nics; i++) {
        const GemuNic *n = &nic_store[i];
        if (n->hub >= 0) continue;
        snprintf(line, sizeof(line),
                 "%s: index=0,type=nic,model=%s,macaddr=%02x:%02x:%02x:%02x:%02x:%02x%s\n",
                 n->name, n->model, n->mac[0], n->mac[1], n->mac[2], n->mac[3],
                 n->mac[4], n->mac[5], n->link_down ? " (link down)" : "");
        print(ud, line);
        if (n->peer) {
            backend_desc(n->peer, d, sizeof(d));
            snprintf(line, sizeof(line), " \\ %s: index=0,%s%s\n", n->peer->id, d,
                     n->peer->link_down ? " (link down)" : "");
            print(ud, line);
        }
    }
    for (int h = 0; h < 256; h++) {
        bool any = false;
        for (int i = 0; i < n_nics; i++) any |= nic_store[i].hub == h;
        for (int i = 0; i < n_backends; i++) any |= backends[i].hub == h;
        if (!any) continue;
        snprintf(line, sizeof(line), "Hub %d\n", h);
        print(ud, line);
        for (int i = 0; i < n_nics; i++) {
            const GemuNic *n = &nic_store[i];
            if (n->hub != h) continue;
            snprintf(line, sizeof(line),
                     " \\ %s: index=0,type=nic,model=%s,macaddr=%02x:%02x:%02x:%02x:%02x:%02x%s\n",
                     n->name, n->model, n->mac[0], n->mac[1], n->mac[2], n->mac[3],
                     n->mac[4], n->mac[5], n->link_down ? " (link down)" : "");
            print(ud, line);
        }
        for (int i = 0; i < n_backends; i++) {
            const Backend *b = &backends[i];
            if (b->hub != h) continue;
            backend_desc(b, d, sizeof(d));
            snprintf(line, sizeof(line), " \\ %s: index=0,%s%s\n", b->id, d,
                     b->link_down ? " (link down)" : "");
            print(ud, line);
        }
    }
    for (int i = 0; i < n_backends; i++) {
        const Backend *b = &backends[i];
        if (b->hub >= 0 || b->peer) continue;
        backend_desc(b, d, sizeof(d));
        snprintf(line, sizeof(line), "%s: index=0,%s (no network adapter)\n", b->id, d);
        print(ud, line);
    }
    UNLOCK();
}

bool gemu_net_set_link(const char *name, bool up) {
    GemuNic *changed[MAX_NICS];
    int nc = 0;
    LOCK();
    GemuNic *n = find_nic(name);
    Backend *b = n ? NULL : find_backend(name);
    if (n) {
        n->link_down = !up;
        if (n->peer) n->peer->link_down = !up;
        changed[nc++] = n;
    } else if (b) {
        b->link_down = !up;
        if (b->peer) {
            b->peer->link_down = !up;
            changed[nc++] = b->peer;
        }
    }
    UNLOCK();
    if (!n && !b) return false;
    for (int i = 0; i < nc; i++)
        if (changed[i]->link_cb) changed[i]->link_cb(changed[i]->link_ud, up);
    return true;
}
