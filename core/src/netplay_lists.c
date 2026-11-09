/* The public server lists - see netplay_refresh_servers() in gemu/netplay.h.
 *
 * Kaillera's master list (www.kaillera.com) and Kaillera Reborn's
 * (kaillerareborn.2manygames.fr, which EmuLinker servers report to) are
 * plain HTTP pages, two lines a server:
 *
 *     Server name
 *     host:port;users/max;games;version;location
 *
 * fetched here with a bare HTTP/1.0 GET, so no HTTP or TLS library is
 * needed. */
#include "netplay_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#  include <sys/select.h>
#endif

#define MAX_SERVERS 400
#define TIMEOUT_MS  8000
#define MAX_PAGE    (256 * 1024)

static const struct { const char *host, *path; } lists[] = {
    { "www.kaillera.com",              "/raw_server_list2.php" },
    { "kaillerareborn.2manygames.fr",  "/server_list.php" },
};

/* GET http://host/path; the body (malloc'd, NUL-terminated) or NULL. */
static char *http_get(const char *host, const char *path) {
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    if (getaddrinfo(host, "80", &hints, &ai) != 0 || !ai) return NULL;
    np_sock_t s = NP_BAD_SOCK;
    for (struct addrinfo *a = ai; a && s == NP_BAD_SOCK; a = a->ai_next) {
        s = socket(a->ai_family, SOCK_STREAM, 0);
        if (s != NP_BAD_SOCK && connect(s, a->ai_addr, (int)a->ai_addrlen) != 0) {
            np_closesocket(s);
            s = NP_BAD_SOCK;
        }
    }
    freeaddrinfo(ai);
    if (s == NP_BAD_SOCK) return NULL;
    char req[512];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: GEMU\r\n"
                                       "Connection: close\r\n\r\n", path, host);
    send(s, req, n, 0);
    char *buf = malloc(MAX_PAGE + 1);
    size_t len = 0;
    uint32_t start = np_now_ms();
    while (buf && len < MAX_PAGE && np_now_ms() - start < TIMEOUT_MS) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        struct timeval tv = { 0, 200000 };
        if (select((int)s + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        int got = (int)recv(s, buf + len, (int)(MAX_PAGE - len), 0);
        if (got <= 0) break;
        len += (size_t)got;
    }
    np_closesocket(s);
    if (!buf) return NULL;
    buf[len] = '\0';
    char *body = strstr(buf, "\r\n\r\n");
    if (strncmp(buf, "HTTP/1.", 7) != 0 || !body || atoi(buf + 9) != 200) { free(buf); return NULL; }
    memmove(buf, body + 4, len - (size_t)(body + 4 - buf) + 1);
    return buf;
}

static void field(char *out, size_t len, char **p) {
    char *semi = strchr(*p, ';');
    size_t n = semi ? (size_t)(semi - *p) : strlen(*p);
    snprintf(out, len, "%.*s", (int)(n < len ? n : len - 1), *p);
    *p += n + (semi ? 1 : 0);
}

/* Name line, then the details line; adds to out (skipping addresses already
 * there) and returns the new count. */
static int parse(char *page, NetplayServer *out, int n, int max) {
    char *name = NULL, *next;
    for (char *line = page; line && *line; line = next) {
        next = line + strcspn(line, "\r\n");
        if (*next) { *next++ = '\0'; next += strspn(next, "\r\n"); } else next = NULL;
        if (!*line) continue;
        if (!strchr(line, ';')) { name = line; continue; }
        if (!name || n >= max) continue;
        NetplayServer s = {0};
        char games[16];
        snprintf(s.name, sizeof(s.name), "%s", name);
        char *p = line;
        field(s.address, sizeof(s.address), &p);
        field(s.users, sizeof(s.users), &p);
        field(games, sizeof(games), &p);
        field(s.version, sizeof(s.version), &p);
        snprintf(s.location, sizeof(s.location), "%s", p);   /* the rest: locations can have ';' */
        s.games = atoi(games);
        s.ping = -1;
        name = NULL;
        if (!strchr(s.address, ':')) continue;
        bool dup = false;
        for (int i = 0; i < n && !dup; i++) dup = !strcmp(out[i].address, s.address);
        if (!dup) out[n++] = s;
    }
    return n;
}

static void *fetch(void *arg) {
    Netplay *np = arg;
    NetplayServer *found = calloc(MAX_SERVERS, sizeof(*found));
    int n = 0, lists_ok = 0;
    for (size_t i = 0; found && i < sizeof(lists) / sizeof(lists[0]); i++) {
        char *page = http_get(lists[i].host, lists[i].path);
        if (!page) continue;
        lists_ok++;
        n = parse(page, found, n, MAX_SERVERS);
        free(page);
    }
    pthread_mutex_lock(&np->lock);
    if (found && lists_ok) {
        for (int i = 0; i < n; i++)          /* keep pings we already had */
            for (int j = 0; j < np->n_servers; j++)
                if (!strcmp(found[i].address, np->servers[j].address)) found[i].ping = np->servers[j].ping;
        free(np->servers);
        np->servers = found;
        np->n_servers = n;
        found = NULL;
        np_logf(np, "* Server list: %d server%s", n, n == 1 ? "" : "s");
    } else {
        np_logf(np, "* Couldn't fetch the server lists (www.kaillera.com, kaillerareborn.2manygames.fr)");
    }
    np->list_busy = false;
    np_changed(np);
    pthread_mutex_unlock(&np->lock);
    free(found);
    return NULL;   /* the window pings them (netplay_ping_servers) */
}

void netplay_refresh_servers(Netplay *np) {
    if (!np) return;
    pthread_mutex_lock(&np->lock);
    bool busy = np->list_busy;
    pthread_mutex_unlock(&np->lock);
    if (busy) return;
    if (np->list_running) { pthread_join(np->list_thread, NULL); np->list_running = false; }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    pthread_mutex_lock(&np->lock);
    np->list_busy = true;
    np_changed(np);
    pthread_mutex_unlock(&np->lock);
    if (pthread_create(&np->list_thread, NULL, fetch, np) == 0) {
        np->list_running = true;
    } else {
        pthread_mutex_lock(&np->lock);
        np->list_busy = false;
        pthread_mutex_unlock(&np->lock);
    }
}

bool netplay_servers_busy(Netplay *np) {
    if (!np) return false;
    pthread_mutex_lock(&np->lock);
    bool b = np->list_busy;
    pthread_mutex_unlock(&np->lock);
    return b;
}

int netplay_servers(Netplay *np, NetplayServer *out, int max) {
    if (!np) return 0;
    pthread_mutex_lock(&np->lock);
    int n = np->n_servers < max ? np->n_servers : max;
    memcpy(out, np->servers, (size_t)n * sizeof(*out));
    pthread_mutex_unlock(&np->lock);
    return n;
}

/* ── Pings ──────────────────────────────────────────────────────────────────
 * "PING" to a server's connection port is answered with "PONG" (no session
 * needed).  One socket, one PING to each server, answers matched by address
 * for a second and a half. */

#define PING_WAIT_MS 1500

static void *ping_all(void *arg) {
    Netplay *np = arg;
    pthread_mutex_lock(&np->lock);
    int n = np->n_servers;
    char (*addrs)[64] = n ? malloc((size_t)n * 64) : NULL;
    for (int i = 0; addrs && i < n; i++) memcpy(addrs[i], np->servers[i].address, 64);
    pthread_mutex_unlock(&np->lock);

    struct sockaddr_storage *sa = addrs ? calloc((size_t)n, sizeof(*sa)) : NULL;
    uint32_t *sent = sa ? calloc((size_t)n, sizeof(*sent)) : NULL;
    int *ms = sent ? malloc((size_t)n * sizeof(*ms)) : NULL;
    np_sock_t s = ms ? socket(AF_INET, SOCK_DGRAM, 0) : NP_BAD_SOCK;
    if (s != NP_BAD_SOCK) {
        for (int i = 0; i < n; i++) {
            ms[i] = -1;
            char host[64], *colon = strrchr(addrs[i], ':');
            if (!colon) continue;
            snprintf(host, sizeof(host), "%.*s", (int)(colon - addrs[i]), addrs[i]);
            struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *ai = NULL;
            if (getaddrinfo(host, colon + 1, &hints, &ai) != 0 || !ai) continue;
            memcpy(&sa[i], ai->ai_addr, ai->ai_addrlen);
            freeaddrinfo(ai);
            sent[i] = np_now_ms();
            sendto(s, "PING", 5, 0, (struct sockaddr *)&sa[i], sizeof(struct sockaddr_in));
        }
        uint32_t start = np_now_ms();
        while (np_now_ms() - start < PING_WAIT_MS && !np->quit_lists) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(s, &fds);
            struct timeval tv = { 0, 100000 };
            if (select((int)s + 1, &fds, NULL, NULL, &tv) <= 0) continue;
            char buf[64];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int got = (int)recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (got < 4 || memcmp(buf, "PONG", 4) != 0) continue;
            uint32_t now = np_now_ms();
            for (int i = 0; i < n; i++) {
                const struct sockaddr_in *to = (const struct sockaddr_in *)&sa[i];
                if (to->sin_family == AF_INET && to->sin_addr.s_addr == from.sin_addr.s_addr &&
                    to->sin_port == from.sin_port && ms[i] < 0)
                    ms[i] = (int)(now - sent[i]);
            }
        }
        np_closesocket(s);
        pthread_mutex_lock(&np->lock);
        for (int i = 0; i < n; i++)
            for (int j = 0; j < np->n_servers; j++)
                if (!strcmp(np->servers[j].address, addrs[i])) np->servers[j].ping = ms[i];
        np_changed(np);
        pthread_mutex_unlock(&np->lock);
    }
    free(ms); free(sent); free(sa); free(addrs);
    pthread_mutex_lock(&np->lock);
    np->ping_busy = false;
    pthread_mutex_unlock(&np->lock);
    return NULL;
}

void netplay_ping_servers(Netplay *np) {
    if (!np) return;
    pthread_mutex_lock(&np->lock);
    bool skip = np->ping_busy || !np->n_servers;
    if (!skip) np->ping_busy = true;
    pthread_mutex_unlock(&np->lock);
    if (skip) return;
    if (np->ping_running) { pthread_join(np->ping_thread, NULL); np->ping_running = false; }
    if (pthread_create(&np->ping_thread, NULL, ping_all, np) == 0) {
        np->ping_running = true;
    } else {
        pthread_mutex_lock(&np->lock);
        np->ping_busy = false;
        pthread_mutex_unlock(&np->lock);
    }
}
