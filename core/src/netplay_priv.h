#pragma once
/* Netplay internals shared by netplay.c (the session, lists, log and the
 * frame queue) and the modes' protocol code (netplay_kaillera.c). */
#include "gemu/netplay.h"
#include <pthread.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET np_sock_t;
#  define NP_BAD_SOCK INVALID_SOCKET
#  define np_closesocket closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <unistd.h>
typedef int np_sock_t;
#  define NP_BAD_SOCK (-1)
#  define np_closesocket close
#endif

#define NP_LOG_LINES   300
#define NP_QUEUE       512    /* combined frames received and not yet played */
#define NP_HASHES      16

/* A mode's protocol, called with the session locked. */
typedef struct NetplayBackend {
    bool (*start)(Netplay *np, const char *server, char *err, size_t err_len);  /* begin connecting */
    void (*stop)(Netplay *np);              /* say goodbye; the thread ends */
    void (*chat)(Netplay *np, const char *text, bool game);
    void (*create_game)(Netplay *np, const char *name);
    void (*join_game)(Netplay *np, uint32_t id);
    void (*leave_game)(Netplay *np);        /* drop (if playing) and quit the game */
    void (*start_game)(Netplay *np);
    void (*ready)(Netplay *np);
    void (*send_input)(Netplay *np, const uint8_t *frames, int n);   /* n frames of ours */
} NetplayBackend;

extern const NetplayBackend netplay_kaillera;

struct Netplay {
    pthread_mutex_t lock;
    char            emulator[64];
    char            game[NETPLAY_GAME_LEN];
    const NetplayBackend *be;
    NetplayInfo     info;
    uint32_t        version;

    NetplayUser    *users;  int n_users, cap_users;
    NetplayGame    *games;  int n_games, cap_games;
    NetplayUser     room[NETPLAY_MAX_PLAYERS];
    int             n_room;

    char            log[NP_LOG_LINES][NETPLAY_LINE_LEN];
    int             log_head, log_n;

    /* the game */
    bool            start_pending;        /* for netplay_take_start */
    int             prefill;              /* frames of no input still to hand out */
    uint8_t         queue[NP_QUEUE][NETPLAY_MAX_PLAYERS * NETPLAY_INPUT_BYTES];
    int             q_r, q_w;             /* free-running */
    uint8_t         pending[NETPLAY_CONN_MAX * NETPLAY_INPUT_BYTES];
    int             n_pending;            /* our frames not sent yet (sent conn at a time) */
    bool            local_taken;          /* this frame's input is in pending[] */
    struct { uint64_t frame; uint32_t hash; } mine[NP_HASHES], theirs[NP_HASHES];
    int             n_mine, n_theirs;
    uint64_t        desync_frame;         /* reported (0 = none) */
    uint32_t        dropped;              /* players who have dropped (bit n-1) */

    /* the public server lists (netplay_lists.c) */
    NetplayServer  *servers;
    int             n_servers;
    pthread_t       list_thread, ping_thread;
    bool            list_running, list_busy, ping_running, ping_busy;
    volatile bool   quit_lists;           /* destroying: stop pinging */

    /* the network thread and its socket (owned by the mode) */
    pthread_t       thread;
    bool            thread_running;
    volatile bool   quit;
    np_sock_t       sock;
    void           *proto;                /* the mode's own state */
};

/* With the lock held: */
void np_changed(Netplay *np);
void np_logf(Netplay *np, const char *fmt, ...);
void np_set_state(Netplay *np, NetplayState st);
void np_clear_lists(Netplay *np);
void np_clear_game(Netplay *np);                    /* leaving or ending a game */
NetplayUser *np_user_add(Netplay *np, const NetplayUser *u);   /* or update by id */
void np_user_remove(Netplay *np, uint16_t id);
NetplayGame *np_game_find(Netplay *np, uint32_t id);
NetplayGame *np_game_add(Netplay *np, const NetplayGame *g);
void np_game_remove(Netplay *np, uint32_t id);
void np_room_add(Netplay *np, const NetplayUser *u);
void np_room_remove(Netplay *np, uint16_t id);
/* The game started: our player number, how many, our delay value. */
void np_game_started(Netplay *np, int player, int players, int delay);
void np_game_playing(Netplay *np);                  /* everyone is ready */
void np_push_frames(Netplay *np, const uint8_t *data, size_t len);   /* combined input */
/* A game chat line: true if it was ours (a sync hash), not for the log. */
bool np_game_chat_in(Netplay *np, const char *user, const char *text);

uint32_t np_now_ms(void);
/* Remember a server we logged in to, with the name and connection type. */
void np_save_settings(const char *server, const char *user, int conn);
