#pragma once
/*
 * Netplay: playing a machine with other people over the network.
 *
 * A Netplay session is a client of a netplay service (a "mode"; Kaillera
 * for now).  The UI (Netplay > Connect... / Lobby..., and the "netplay"
 * monitor command) drives it: connect to a server, chat, create or join a
 * game, start it.  The machine drives the game side, once per frame:
 *
 *   netplay_take_start()  a game is starting: power the machine on in the
 *                         same state as everyone else, then netplay_ready()
 *   netplay_frame()       give this player's input for the next frame and
 *                         get every player's input for it - or 0 when it
 *                         hasn't arrived yet (run nothing, try again), or
 *                         -1 when the game has ended
 *
 * Every machine in the game runs the same frames with the same inputs
 * (lockstep), so the emulation must be deterministic: the machine locks out
 * anything that would change it on one side only (savestates, cheats,
 * pausing...).  netplay_sync_hash() lets the session compare the players'
 * machine state now and then, and report a desync if they ever differ.
 *
 * The network runs on its own thread; every call here is thread-safe and
 * quick.  Lists and logs are copied out under the lock, and
 * netplay_version() changes whenever anything a window shows changes.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Netplay Netplay;

typedef enum {
    NETPLAY_MODE_KAILLERA,
    NETPLAY_N_MODES
} NetplayMode;

typedef enum {
    NETPLAY_OFFLINE,
    NETPLAY_CONNECTING,
    NETPLAY_LOBBY,       /* connected, not in a game */
    NETPLAY_ROOM,        /* in a game that hasn't started */
    NETPLAY_STARTING,    /* the game is starting: the machine powers on */
    NETPLAY_PLAYING,
} NetplayState;

/* Kaillera's connection types: how many frames go in each packet (1-6).
 * Everyone in a game must use the same one. */
#define NETPLAY_CONN_MIN      1
#define NETPLAY_CONN_MAX      6
#define NETPLAY_CONN_DEFAULT  3          /* Good */
const char *netplay_conn_name(int conn);  /* "LAN", "Excellent", ... */
const char *netplay_mode_name(NetplayMode mode);

#define NETPLAY_MAX_PLAYERS   8
#define NETPLAY_INPUT_BYTES   2          /* per player per frame */
#define NETPLAY_INPUT_RESET   0x01u      /* byte 1: reset the machine on this frame */

#define NETPLAY_NAME_LEN      32
#define NETPLAY_GAME_LEN      128
#define NETPLAY_LINE_LEN      200

typedef struct {
    char     name[NETPLAY_NAME_LEN];
    uint16_t id;
    uint32_t ping;          /* ms */
    uint8_t  conn;          /* connection type */
    uint8_t  status;        /* 0 playing, 1 idle (lobby list) */
} NetplayUser;

typedef struct {
    uint32_t id;
    char     name[NETPLAY_GAME_LEN];
    char     emulator[64];
    char     owner[NETPLAY_NAME_LEN];
    char     players[16];   /* "1/2" */
    uint8_t  status;        /* 0 waiting, 1 playing, 2 synchronizing */
} NetplayGame;

typedef struct {
    NetplayState state;
    NetplayMode  mode;
    char         server[128];
    char         user[NETPLAY_NAME_LEN];
    int          conn;
    uint32_t     game_id;       /* the game we're in (ROOM and later) */
    bool         owner;         /* ...and created it */
    int          player;        /* our player number (1-based), once started */
    int          players;
    int          delay;         /* frames of input delay, once playing */
    uint64_t     frame;         /* frames played */
} NetplayInfo;

/* emulator: the name every player's emulator must match ("GEMU 0.0.1A").
 * game: what Create Game offers, normally the loaded ROM. */
Netplay *netplay_create(const char *emulator, const char *game);
void     netplay_destroy(Netplay *np);
void     netplay_set_game(Netplay *np, const char *game);
const char *netplay_game_name(Netplay *np);

/* ── Session (the UI and the monitor) ────────────────────────────────────── */

/* server: "host[:port]".  Starts connecting; how it goes shows in the log
 * and the state.  False (with err) for bad arguments or while connected. */
bool netplay_connect(Netplay *np, NetplayMode mode, const char *server, const char *user,
                     int conn, char *err, size_t err_len);
void netplay_disconnect(Netplay *np);
void netplay_chat(Netplay *np, const char *text);   /* the game's chat in a game, else the server's */
void netplay_create_game(Netplay *np);
void netplay_join_game(Netplay *np, uint32_t game_id);
void netplay_leave_game(Netplay *np);
void netplay_start_game(Netplay *np);               /* the game's owner */

uint32_t netplay_version(Netplay *np);
void     netplay_info(Netplay *np, NetplayInfo *out);
int      netplay_users(Netplay *np, NetplayUser *out, int max);   /* everyone on the server */
int      netplay_games(Netplay *np, NetplayGame *out, int max);
int      netplay_room(Netplay *np, NetplayUser *out, int max);    /* the players in our game */
/* The chat and status log, oldest first; returns how many lines. */
int      netplay_log(Netplay *np, char (*out)[NETPLAY_LINE_LEN], int max);
/* Add a line to the log (the machine's own notes: "Netplay: console reset"). */
void     netplay_note(Netplay *np, const char *fmt, ...);

/* Public servers, from the Kaillera and Kaillera Reborn (EmuLinker) master
 * lists.  netplay_refresh_servers() fetches them in the background (plain
 * HTTP); netplay_servers() copies what's arrived.  The list is empty until
 * the first fetch finishes - netplay_servers_busy() says it's on its way. */
typedef struct {
    char name[64];
    char address[64];     /* host:port */
    char users[16];       /* "3/100" */
    int  games;
    char version[16];
    char location[48];
    int  ping;            /* ms, -1 = no answer yet */
} NetplayServer;
void netplay_refresh_servers(Netplay *np);
/* Time a ping to every listed server (Kaillera's PING/PONG), in the
 * background; the results land in the list.  Ignored while one runs. */
void netplay_ping_servers(Netplay *np);
bool netplay_servers_busy(Netplay *np);
int  netplay_servers(Netplay *np, NetplayServer *out, int max);

/* Recently used servers and the last name/connection type, kept in the
 * config directory (netplay.ini). */
int      netplay_recent_servers(char (*out)[128], int max);
void     netplay_last_settings(char *user, size_t user_len, int *conn);

/* ── Game (the machine, once per frame) ──────────────────────────────────── */

/* True once when a game starts: our player number and the player count. */
bool netplay_take_start(Netplay *np, int *player, int *players);
void netplay_ready(Netplay *np);          /* the machine is in its starting state */
bool netplay_in_game(Netplay *np);        /* STARTING or PLAYING */

/* local: this player's NETPLAY_INPUT_BYTES for the next frame (taken once
 * per frame, however often this is called while waiting).  On 1, all[]
 * holds *players entries of NETPLAY_INPUT_BYTES for that frame. */
int  netplay_frame(Netplay *np, const uint8_t *local, uint8_t *all, int *players);

/* The "netplay" monitor command: info, connect, games, create, join, start,
 * chat, log... (see its usage line). */
typedef struct GemuMonitor GemuMonitor;
void netplay_monitor(Netplay *np, GemuMonitor *mon, const char *args);

/* A hash of the machine's state after `frame` frames, to compare with the
 * other players' (every few seconds is plenty). */
void netplay_sync_hash(Netplay *np, uint64_t frame, uint32_t hash);
