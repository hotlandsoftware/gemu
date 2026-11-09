/*
 * Netplay over Kaillera (protocol 0.83), as EmuLinker-K and the original
 * Kaillera servers speak it.
 *
 * UDP.  "HELLO0.83" to the server's port (27888) is answered with
 * "HELLOD00D<port>", the port the session uses from then on.  Every packet
 * after that is a bundle: a message count, then messages newest first,
 * each a 16-bit sequence number, a 16-bit length (counting the type byte)
 * and a type, all little-endian.  Each packet repeats the sender's last few
 * messages so a lost packet loses nothing; a receiver handles only numbers
 * past the last it saw, oldest first.  Strings end in NUL.
 *
 * In a game, each player sends its input `conn` frames at a time (game
 * data, 0x12) and gets back everyone's, interleaved by frame; input the
 * receiver has seen before can come as an index into a 256-entry cache of
 * past packets instead (0x13).  The cache drops its oldest entry when full,
 * so indices shift - as EmuLinker-K's does.
 *
 * Message layouts are those of the protocol notes by SupraFast and the
 * Okai project (as kept in direlera-rs), checked against EmuLinker-K.
 */
#include "netplay_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#  include <sys/select.h>
#  include <errno.h>
#endif

#define DEFAULT_PORT   27888
#define HIST           5        /* messages repeated in every packet */
#define MSG_MAX        300      /* our messages: chat is the longest */
#define CHAT_MAX       127
#define CACHE          256
#define KEEPALIVE_MS   15000
#define RESEND_MS      200      /* waiting for game data this long: ask again */
#define ANSWER_MS      500      /* a request unanswered this long: send it again */

enum {
    M_USER_QUIT = 0x01, M_USER_JOINED, M_LOGIN, M_SERVER_STATUS, M_SERVER_ACK, M_CLIENT_ACK,
    M_CHAT, M_GAME_CHAT, M_KEEPALIVE, M_CREATE_GAME, M_QUIT_GAME, M_JOIN_GAME, M_PLAYER_INFO,
    M_GAME_STATUS, M_KICK, M_CLOSE_GAME, M_START_GAME, M_GAME_DATA, M_GAME_CACHE, M_DROP_GAME,
    M_READY, M_REJECTED, M_INFO,
};

typedef struct {
    struct sockaddr_storage addr;     /* the session's port, after HELLO */
    socklen_t       addrlen;
    char            host[128];
    int             port;
    uint16_t        out_seq;
    struct { uint16_t seq; uint16_t len; uint8_t msg[MSG_MAX]; } hist[HIST];
    int             n_hist;
    int             in_last;          /* last message number handled, -1 = none */
    uint8_t        *cache[CACHE];
    uint16_t        cache_len[CACHE];
    int             n_cache;
    bool            have_id;
    uint16_t        my_id;
    bool            want_create;
    uint32_t        want_join;
    uint32_t        last_send, last_data, last_rx;
    bool            awaiting;         /* sent something the server answers; nothing back yet */
    bool            cache_miss_logged;
} Kai;

static Kai *K(Netplay *np) { return np->proto; }

/* ── Packets ─────────────────────────────────────────────────────────────── */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void send_raw(Netplay *np, const uint8_t *buf, size_t len) {
    Kai *k = K(np);
    sendto(np->sock, (const char *)buf, (int)len, 0, (struct sockaddr *)&k->addr, k->addrlen);
    k->last_send = np_now_ms();
}

/* Resend what we sent last.  The server sees nothing new in it, and
 * answers that by repeating its own last messages - how a client asks for
 * what it missed. */
static void resend(Netplay *np) {
    Kai *k = K(np);
    uint8_t pkt[1 + HIST * (5 + MSG_MAX)];
    size_t o = 1;
    int n = 0;
    for (int i = 0; i < k->n_hist; i++, n++) {
        put16(pkt + o, k->hist[i].seq);
        put16(pkt + o + 2, k->hist[i].len);
        memcpy(pkt + o + 4, k->hist[i].msg, k->hist[i].len);
        o += 4 + k->hist[i].len;
    }
    if (!n) return;
    pkt[0] = (uint8_t)n;
    send_raw(np, pkt, o);
}

/* Send one message (type + body), with the last few before it. */
static void send_msg(Netplay *np, uint8_t type, const uint8_t *body, size_t len) {
    Kai *k = K(np);
    if (len + 1 > MSG_MAX) len = MSG_MAX - 1;
    memmove(&k->hist[1], &k->hist[0], (HIST - 1) * sizeof(k->hist[0]));
    k->hist[0].seq = k->out_seq++;
    k->hist[0].len = (uint16_t)(len + 1);
    k->hist[0].msg[0] = type;
    memcpy(k->hist[0].msg + 1, body, len);
    if (k->n_hist < HIST) k->n_hist++;
    if (type == M_LOGIN || type == M_CLIENT_ACK || type == M_CREATE_GAME || type == M_JOIN_GAME ||
        type == M_START_GAME || type == M_READY)
        k->awaiting = true;
    resend(np);
}

/* body builders */
typedef struct { uint8_t b[MSG_MAX]; size_t n; } Body;
static void b_u8(Body *b, uint8_t v)   { if (b->n < MSG_MAX) b->b[b->n++] = v; }
static void b_u16(Body *b, uint16_t v) { b_u8(b, (uint8_t)v); b_u8(b, (uint8_t)(v >> 8)); }
static void b_u32(Body *b, uint32_t v) { b_u16(b, (uint16_t)v); b_u16(b, (uint16_t)(v >> 16)); }
static void b_str(Body *b, const char *s, size_t max) {
    for (size_t i = 0; s[i] && i < max; i++) b_u8(b, (uint8_t)s[i]);
    b_u8(b, 0);
}

/* readers: a bad read leaves rd.bad set and returns zeros */
typedef struct { const uint8_t *p; size_t n, i; bool bad; } Rd;
static uint8_t r_u8(Rd *r) { if (r->i + 1 > r->n) { r->bad = true; return 0; } return r->p[r->i++]; }
static uint16_t r_u16(Rd *r) { uint16_t lo = r_u8(r); return (uint16_t)(lo | r_u8(r) << 8); }
static uint32_t r_u32(Rd *r) { uint32_t lo = r_u16(r); return lo | (uint32_t)r_u16(r) << 16; }
static void r_str(Rd *r, char *out, size_t len) {
    size_t o = 0;
    for (;;) {
        if (r->i >= r->n) { r->bad = true; break; }
        char c = (char)r->p[r->i++];
        if (!c) break;
        if (o + 1 < len) out[o++] = c;
    }
    if (len) out[o] = '\0';
}

/* ── Requests ────────────────────────────────────────────────────────────── */

static void kai_chat(Netplay *np, const char *text, bool game) {
    Body b = {0};
    b_u8(&b, 0);
    b_str(&b, text, CHAT_MAX);
    send_msg(np, game ? M_GAME_CHAT : M_CHAT, b.b, b.n);
}

static void kai_create_game(Netplay *np, const char *name) {
    Body b = {0};
    b_u8(&b, 0);
    b_str(&b, name, NETPLAY_GAME_LEN - 1);
    b_u8(&b, 0);
    b_u32(&b, 0xFFFFFFFFu);
    K(np)->want_create = true;
    send_msg(np, M_CREATE_GAME, b.b, b.n);
}

static void kai_join_game(Netplay *np, uint32_t id) {
    Body b = {0};
    b_u8(&b, 0);
    b_u32(&b, id);
    b_u8(&b, 0);
    b_u32(&b, 0);
    b_u16(&b, 0xFFFF);
    b_u8(&b, (uint8_t)np->info.conn);
    K(np)->want_join = id;
    send_msg(np, M_JOIN_GAME, b.b, b.n);
}

static void kai_leave_game(Netplay *np) {
    Body b = {0};
    if (np->info.state == NETPLAY_STARTING || np->info.state == NETPLAY_PLAYING) {
        b_u8(&b, 0);
        b_u8(&b, 0);
        send_msg(np, M_DROP_GAME, b.b, b.n);
        b.n = 0;
    }
    b_u8(&b, 0);
    b_u16(&b, 0xFFFF);
    send_msg(np, M_QUIT_GAME, b.b, b.n);
    np_clear_game(np);
    np_set_state(np, NETPLAY_LOBBY);
    np_logf(np, "* Left the game");
}

static void kai_start_game(Netplay *np) {
    Body b = {0};
    b_u8(&b, 0);
    b_u16(&b, 0xFFFF);
    b_u8(&b, 0xFF);
    b_u8(&b, 0xFF);
    send_msg(np, M_START_GAME, b.b, b.n);
}

static void kai_ready(Netplay *np) {
    uint8_t z = 0;
    send_msg(np, M_READY, &z, 1);
}

static void kai_send_input(Netplay *np, const uint8_t *frames, int n) {
    Body b = {0};
    b_u8(&b, 0);
    b_u16(&b, (uint16_t)(n * NETPLAY_INPUT_BYTES));
    for (int i = 0; i < n * NETPLAY_INPUT_BYTES; i++) b_u8(&b, frames[i]);
    send_msg(np, M_GAME_DATA, b.b, b.n);
}

/* ── Notifications ───────────────────────────────────────────────────────── */

static void cache_add(Kai *k, const uint8_t *data, size_t len) {
    for (int i = 0; i < k->n_cache; i++)
        if (k->cache_len[i] == len && !memcmp(k->cache[i], data, len)) return;
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) return;
    memcpy(copy, data, len);
    if (k->n_cache == CACHE) {          /* drop the oldest: every index moves down one */
        free(k->cache[0]);
        memmove(&k->cache[0], &k->cache[1], (CACHE - 1) * sizeof(k->cache[0]));
        memmove(&k->cache_len[0], &k->cache_len[1], (CACHE - 1) * sizeof(k->cache_len[0]));
        k->n_cache--;
    }
    k->cache[k->n_cache] = copy;
    k->cache_len[k->n_cache++] = (uint16_t)len;
}

static void cache_clear(Kai *k) {
    for (int i = 0; i < k->n_cache; i++) free(k->cache[i]);
    k->n_cache = 0;
    k->cache_miss_logged = false;
}

static bool is_me(Netplay *np, const char *name, uint16_t id) {
    Kai *k = K(np);
    return k->have_id ? id == k->my_id : strcmp(name, np->info.user) == 0;
}

static void logged_in(Netplay *np) {
    if (np->info.state != NETPLAY_CONNECTING) return;
    np_set_state(np, NETPLAY_LOBBY);
    np_logf(np, "* Connected to %s", np->info.server);
    np_save_settings(np->info.server, np->info.user, np->info.conn);
}

static void handle(Netplay *np, uint8_t type, Rd *r) {
    Kai *k = K(np);
    char name[NETPLAY_GAME_LEN], text[512];
    switch (type) {
    case M_SERVER_ACK: {                           /* ping: answer straight away */
        Body b = {0};
        b_u8(&b, 0);
        for (uint32_t i = 0; i < 4; i++) b_u32(&b, i);
        send_msg(np, M_CLIENT_ACK, b.b, b.n);
        break;
    }
    case M_SERVER_STATUS: {
        r_str(r, name, sizeof(name));
        uint32_t nu = r_u32(r), ng = r_u32(r);
        for (uint32_t i = 0; i < nu && !r->bad; i++) {
            NetplayUser u = {0};
            r_str(r, u.name, sizeof(u.name));
            u.ping = r_u32(r);
            u.status = r_u8(r);
            u.id = r_u16(r);
            u.conn = r_u8(r);
            if (!r->bad) np_user_add(np, &u);
        }
        for (uint32_t i = 0; i < ng && !r->bad; i++) {
            NetplayGame g = {0};
            r_str(r, g.name, sizeof(g.name));
            g.id = r_u32(r);
            r_str(r, g.emulator, sizeof(g.emulator));
            r_str(r, g.owner, sizeof(g.owner));
            r_str(r, g.players, sizeof(g.players));
            g.status = r_u8(r);
            if (!r->bad) np_game_add(np, &g);
        }
        logged_in(np);
        break;
    }
    case M_USER_JOINED: {
        NetplayUser u = {0};
        r_str(r, u.name, sizeof(u.name));
        u.id = r_u16(r);
        u.ping = r_u32(r);
        u.conn = r_u8(r);
        u.status = 1;
        if (r->bad) break;
        np_user_add(np, &u);
        if (!k->have_id && strcmp(u.name, np->info.user) == 0) {
            k->have_id = true;
            k->my_id = u.id;
            logged_in(np);
        } else {
            np_logf(np, "* %s joined", u.name);
        }
        break;
    }
    case M_USER_QUIT: {
        r_str(r, name, sizeof(name));
        uint16_t id = r_u16(r);
        r_str(r, text, sizeof(text));
        np_user_remove(np, id);
        np_logf(np, text[0] ? "* %s left (%s)" : "* %s left", name, text);
        break;
    }
    case M_CHAT:
        r_str(r, name, sizeof(name));
        r_str(r, text, sizeof(text));
        if (!r->bad) np_logf(np, "<%s> %s", name, text);
        break;
    case M_GAME_CHAT:
        r_str(r, name, sizeof(name));
        r_str(r, text, sizeof(text));
        if (!r->bad && !np_game_chat_in(np, name, text)) np_logf(np, "[game] <%s> %s", name, text);
        break;
    case M_INFO:
        r_str(r, name, sizeof(name));
        r_str(r, text, sizeof(text));
        if (!r->bad) np_logf(np, "* %s", text);
        break;
    case M_CREATE_GAME: {
        NetplayGame g = {0};
        r_str(r, g.owner, sizeof(g.owner));
        r_str(r, g.name, sizeof(g.name));
        r_str(r, g.emulator, sizeof(g.emulator));
        g.id = r_u32(r);
        snprintf(g.players, sizeof(g.players), "1/?");
        if (r->bad) break;
        np_game_add(np, &g);
        if (k->want_create && strcmp(g.owner, np->info.user) == 0) {
            k->want_create = false;
            np->info.game_id = g.id;
            np->info.owner = true;
            NetplayUser me = { .id = k->my_id, .conn = (uint8_t)np->info.conn };
            snprintf(me.name, sizeof(me.name), "%s", np->info.user);
            np->n_room = 0;
            np_room_add(np, &me);
            np_set_state(np, NETPLAY_ROOM);
            np_logf(np, "* Created %s - waiting for players; Start when everyone's in", g.name);
        }
        break;
    }
    case M_GAME_STATUS: {
        r_u8(r);
        uint32_t id = r_u32(r);
        uint8_t status = r_u8(r), players = r_u8(r), max = r_u8(r);
        NetplayGame *g = np_game_find(np, id);
        if (g && !r->bad) {
            g->status = status;
            snprintf(g->players, sizeof(g->players), "%u/%u", players, max);
            np_changed(np);
        }
        break;
    }
    case M_PLAYER_INFO: {                          /* who's already in the game we joined */
        r_u8(r);
        uint32_t n = r_u32(r);
        for (uint32_t i = 0; i < n && !r->bad; i++) {
            NetplayUser u = {0};
            r_str(r, u.name, sizeof(u.name));
            u.ping = r_u32(r);
            u.id = r_u16(r);
            u.conn = r_u8(r);
            if (!r->bad) np_room_add(np, &u);
        }
        break;
    }
    case M_JOIN_GAME: {
        r_u8(r);
        r_u32(r);
        NetplayUser u = {0};
        r_str(r, u.name, sizeof(u.name));
        u.ping = r_u32(r);
        u.id = r_u16(r);
        u.conn = r_u8(r);
        if (r->bad) break;
        if (is_me(np, u.name, u.id)) {
            if (!k->want_join) break;
            np->info.game_id = k->want_join;
            k->want_join = 0;
            np_room_add(np, &u);
            np_set_state(np, NETPLAY_ROOM);
            NetplayGame *g = np_game_find(np, np->info.game_id);
            np_logf(np, "* Joined %s - the owner starts the game", g ? g->name : "the game");
        } else if (np->info.state >= NETPLAY_ROOM) {
            np_room_add(np, &u);
            np_logf(np, "* %s joined the game", u.name);
        }
        break;
    }
    case M_QUIT_GAME: {
        r_str(r, name, sizeof(name));
        uint16_t id = r_u16(r);
        if (r->bad || np->info.state < NETPLAY_ROOM) break;
        if (is_me(np, name, id)) break;            /* we already left */
        np_room_remove(np, id);
        np_logf(np, "* %s left the game", name);
        break;
    }
    case M_CLOSE_GAME: {
        r_u8(r);
        uint32_t id = r_u32(r);
        if (r->bad) break;
        np_game_remove(np, id);
        if (np->info.state >= NETPLAY_ROOM && id == np->info.game_id) {
            np_clear_game(np);
            np_set_state(np, NETPLAY_LOBBY);
            np_logf(np, "* The game was closed");
        }
        break;
    }
    case M_START_GAME: {
        r_u8(r);
        uint16_t delay = r_u16(r);
        uint8_t player = r_u8(r), total = r_u8(r);
        if (r->bad || np->info.state != NETPLAY_ROOM) break;
        cache_clear(k);
        np_game_started(np, player, total, delay);
        np_logf(np, "* Starting...");
        break;
    }
    case M_READY:
        if (np->info.state != NETPLAY_STARTING) break;
        cache_clear(k);
        k->last_data = np_now_ms();
        np_game_playing(np);
        break;
    case M_GAME_DATA: {
        r_u8(r);
        uint16_t len = r_u16(r);
        if (r->bad || r->i + len > r->n || np->info.state != NETPLAY_PLAYING) break;
        cache_add(k, r->p + r->i, len);
        np_push_frames(np, r->p + r->i, len);
        k->last_data = np_now_ms();
        break;
    }
    case M_GAME_CACHE: {
        r_u8(r);
        uint8_t pos = r_u8(r);
        if (r->bad || np->info.state != NETPLAY_PLAYING) break;
        if (pos < k->n_cache) {
            np_push_frames(np, k->cache[pos], k->cache_len[pos]);
            k->last_data = np_now_ms();
        } else if (!k->cache_miss_logged) {
            k->cache_miss_logged = true;
            np_logf(np, "*** The server sent input this client doesn't have (cache %u of %d)",
                    pos, k->n_cache);
        }
        break;
    }
    case M_DROP_GAME: {
        r_str(r, name, sizeof(name));
        uint8_t player = r_u8(r);
        if (r->bad || np->info.state < NETPLAY_STARTING || player < 1 || player > 32) break;
        if (np->dropped & (1u << (player - 1))) break;   /* told twice */
        np->dropped |= 1u << (player - 1);
        if (player == np->info.player) {
            np_set_state(np, NETPLAY_ROOM);
            np_logf(np, "* You dropped out of the game");
        } else {
            np_logf(np, "* %s (player %u) dropped - their controller is now idle", name, player);
        }
        break;
    }
    case M_REJECTED:
        r_str(r, name, sizeof(name));
        r_u16(r);
        r_str(r, text, sizeof(text));
        np_logf(np, "* The server refused: %s", text);
        np->quit = true;
        np_clear_lists(np);
        np_set_state(np, NETPLAY_OFFLINE);
        break;
    default:
        break;
    }
}

/* A bundle from the server: messages newest first. */
static void receive(Netplay *np, const uint8_t *buf, size_t len) {
    Kai *k = K(np);
    if (len < 1) return;
    int count = buf[0];
    struct { uint16_t seq; const uint8_t *msg; uint16_t len; } m[64];
    int n = 0;
    size_t o = 1;
    for (int i = 0; i < count && n < 64; i++) {
        if (o + 4 > len) break;
        uint16_t seq = (uint16_t)(buf[o] | buf[o + 1] << 8);
        uint16_t ml = (uint16_t)(buf[o + 2] | buf[o + 3] << 8);
        o += 4;
        if (ml < 1 || o + ml > len) break;
        m[n].seq = seq; m[n].msg = buf + o; m[n].len = ml;
        n++;
        o += ml;
    }
    /* Handle new messages oldest first, and only in order: if this packet
     * starts past the next one we expect, messages were lost - ask for them
     * again (by resending ours) rather than skip game input. */
    bool any = false;
    for (int i = n - 1; i >= 0; i--) {
        if (k->in_last >= 0) {
            int16_t ahead = (int16_t)(m[i].seq - (uint16_t)k->in_last);
            if (ahead <= 0) continue;              /* seen it */
            if (ahead > 1) { resend(np); return; }
        }
        k->in_last = m[i].seq;
        if (!any) { any = true; k->awaiting = false; k->last_rx = np_now_ms(); }
        Rd r = { m[i].msg + 1, (size_t)m[i].len - 1, 0, false };
        handle(np, m[i].msg[0], &r);
    }
}

/* ── The session's thread ────────────────────────────────────────────────── */

static bool wait_readable(np_sock_t s, int ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select((int)s + 1, &fds, NULL, NULL, &tv) > 0;
}

static void fail(Netplay *np, const char *msg) {
    pthread_mutex_lock(&np->lock);
    np_logf(np, "* %s", msg);
    np_clear_lists(np);
    np_set_state(np, NETPLAY_OFFLINE);
    pthread_mutex_unlock(&np->lock);
}

static void *session(void *arg) {
    Netplay *np = arg;
    Kai *k = K(np);
    char msg[256];

    /* where: host and port */
    char port[16];
    snprintf(port, sizeof(port), "%d", k->port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM }, *ai = NULL;
    if (getaddrinfo(k->host, port, &hints, &ai) != 0 || !ai) {
        snprintf(msg, sizeof(msg), "Can't find server %s", k->host);
        fail(np, msg);
        return NULL;
    }
    np_sock_t s = socket(ai->ai_family, SOCK_DGRAM, 0);
    if (s == NP_BAD_SOCK) {
        freeaddrinfo(ai);
        fail(np, "Can't open a network socket");
        return NULL;
    }
    memcpy(&k->addr, ai->ai_addr, ai->ai_addrlen);
    k->addrlen = (socklen_t)ai->ai_addrlen;
    freeaddrinfo(ai);
    pthread_mutex_lock(&np->lock);
    np->sock = s;
    pthread_mutex_unlock(&np->lock);

    /* HELLO: the server answers with the port the session uses */
    static uint8_t buf[65536];
    int got = -1;
    for (int tries = 0; tries < 4 && got < 0 && !np->quit; tries++) {
        sendto(s, "HELLO0.83", 10, 0, (struct sockaddr *)&k->addr, k->addrlen);
        if (!wait_readable(s, 1000)) continue;
        int n = (int)recv(s, (char *)buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        buf[n] = 0;
        if (!strncmp((char *)buf, "HELLOD00D", 9)) got = atoi((char *)buf + 9);
        else if (!strncmp((char *)buf, "TOO", 3)) { fail(np, "The server is full"); return NULL; }
        else if (!strncmp((char *)buf, "VER", 3)) { fail(np, "The server wants a different Kaillera version"); return NULL; }
    }
    if (np->quit) return NULL;
    if (got <= 0 || got > 65535) {
        snprintf(msg, sizeof(msg), "No answer from %s:%d - is it a Kaillera server?", k->host, k->port);
        fail(np, msg);
        return NULL;
    }
    if (k->addr.ss_family == AF_INET) ((struct sockaddr_in *)&k->addr)->sin_port = htons((uint16_t)got);
    else ((struct sockaddr_in6 *)&k->addr)->sin6_port = htons((uint16_t)got);

    pthread_mutex_lock(&np->lock);
    Body b = {0};
    b_str(&b, np->info.user, NETPLAY_NAME_LEN - 1);
    b_str(&b, np->emulator, sizeof(np->emulator) - 1);
    b_u8(&b, (uint8_t)np->info.conn);
    send_msg(np, M_LOGIN, b.b, b.n);
    pthread_mutex_unlock(&np->lock);
    uint32_t login_at = np_now_ms();

    while (!np->quit) {
        if (wait_readable(s, 20)) {
            int n = (int)recv(s, (char *)buf, sizeof(buf), 0);
            if (n > 0) {
                pthread_mutex_lock(&np->lock);
                receive(np, buf, (size_t)n);
                pthread_mutex_unlock(&np->lock);
            }
        }
        pthread_mutex_lock(&np->lock);
        uint32_t now = np_now_ms();
        if (np->info.state == NETPLAY_CONNECTING && now - login_at > 8000) {
            np->quit = true;
            pthread_mutex_unlock(&np->lock);
            fail(np, "The server didn't answer the login");
            break;
        }
        /* in a game and starved: ask for what we missed */
        if (np->info.state == NETPLAY_PLAYING && np->q_r == np->q_w && np->prefill == 0 &&
            now - k->last_data > RESEND_MS && now - k->last_send > RESEND_MS)
            resend(np);
        /* a request (or its answer) got lost: send it again */
        else if (k->awaiting && now - k->last_send > ANSWER_MS)
            resend(np);
        if (now - k->last_send > KEEPALIVE_MS) {
            uint8_t z = 0;
            send_msg(np, M_KEEPALIVE, &z, 1);
        }
        pthread_mutex_unlock(&np->lock);
    }
    return NULL;
}

static bool kai_start(Netplay *np, const char *server, char *err, size_t err_len) {
    Kai *k = np->proto;
    if (!k) k = np->proto = calloc(1, sizeof(Kai));
    if (!k) { snprintf(err, err_len, "out of memory"); return false; }
    cache_clear(k);
    memset(k, 0, sizeof(*k));
    k->in_last = -1;
    k->port = DEFAULT_PORT;
    /* host, host:port, [v6]:port */
    const char *colon = strrchr(server, ':');
    if (server[0] == '[') {
        const char *end = strchr(server, ']');
        if (!end) { snprintf(err, err_len, "bad server address '%s'", server); return false; }
        snprintf(k->host, sizeof(k->host), "%.*s", (int)(end - server - 1), server + 1);
        if (end[1] == ':') k->port = atoi(end + 2);
    } else if (colon && strchr(server, ':') == colon) {   /* one colon: host:port */
        snprintf(k->host, sizeof(k->host), "%.*s", (int)(colon - server), server);
        k->port = atoi(colon + 1);
    } else {
        snprintf(k->host, sizeof(k->host), "%s", server);
    }
    if (!k->host[0] || k->port <= 0 || k->port > 65535) {
        snprintf(err, err_len, "bad server address '%s'", server);
        return false;
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { snprintf(err, err_len, "WSAStartup failed"); return false; }
#endif
    if (pthread_create(&np->thread, NULL, session, np) != 0) {
        snprintf(err, err_len, "can't start the network thread");
        return false;
    }
    np->thread_running = true;
    return true;
}

static void kai_stop(Netplay *np) {
    Kai *k = K(np);
    if (k && np->sock != NP_BAD_SOCK && k->addrlen && np->info.state != NETPLAY_CONNECTING) {
        if (np->info.state >= NETPLAY_ROOM) kai_leave_game(np);
        Body b = {0};
        b_u8(&b, 0);
        b_u16(&b, 0xFFFF);
        b_str(&b, "GEMU", 32);
        send_msg(np, M_USER_QUIT, b.b, b.n);
    }
    if (k) cache_clear(k);
}

const NetplayBackend netplay_kaillera = {
    .start       = kai_start,
    .stop        = kai_stop,
    .chat        = kai_chat,
    .create_game = kai_create_game,
    .join_game   = kai_join_game,
    .leave_game  = kai_leave_game,
    .start_game  = kai_start_game,
    .ready       = kai_ready,
    .send_input  = kai_send_input,
};
