/*
 * Netplay > Connect... / Lobby... for GTK builds (see gemu/netplay_dialog.h):
 * the same two pages netplay_dialog_sdl.c draws, as GTK widgets in a
 * GtkStack - the connection form while offline, the lobby (games, users,
 * Create / Join / Leave / Start, the chat) once connected.
 */
#ifdef GEMU_GTK
#include "gemu/netplay_dialog.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_GAMES 512
#define MAX_USERS 512
#define MAX_LOG   300

/* Shown columns, then hidden numbers the numeric ones sort by. */
enum { G_ID, G_NAME, G_EMULATOR, G_PLAYERS, G_STATUS, G_OWNER, G_NPLAYERS, G_COLS };
enum { U_NAME, U_PING, U_NPING, U_COLS };
enum { S_NAME, S_ADDRESS, S_USERS, S_GAMES, S_PING, S_LOCATION, S_NUSERS, S_NGAMES, S_NPING, S_COLS };
#define NO_PING 999999          /* sorts after every real ping */
#define MAX_SERVERS 400

typedef struct {
    NetplayDialog base;
    Netplay      *np;
    uint32_t      seen_version;
    NetplayInfo   info;
    GtkWidget    *window, *stack, *status;
    GtkWidget    *name, *server, *conn, *connect, *clog;           /* connect page */
    GtkWidget    *servers, *refresh;
    GtkListStore *server_store;
    int           n_servers_shown;
    bool          busy_shown, fetched;
    gint64        last_ping;
    GtkWidget    *games, *users, *users_label, *chat, *say;         /* lobby page */
    GtkWidget    *b_create, *b_join, *b_leave, *b_start;
    GtkListStore *game_store, *user_store;
    char          log_shown[MAX_LOG][NETPLAY_LINE_LEN];
    int           n_log_shown;
} GtkNetplayDialog;

/* Kaillera names come in whatever encoding the server's people use (Latin-1,
 * Shift-JIS, GBK...) and GTK wants UTF-8: UTF-8 shows as it is, anything
 * else with '?' for its non-ASCII bytes.  One of four buffers per call, so
 * a gtk_list_store_set() can take several. */
static const char *utf8(const char *s) {
    static char buf[4][256];
    static int next;
    if (g_utf8_validate(s, -1, NULL)) return s;
    char *b = buf[next++ & 3];
    size_t i = 0;
    for (; s[i] && i + 1 < sizeof(buf[0]); i++) b[i] = (unsigned char)s[i] < 0x80 ? s[i] : '?';
    b[i] = '\0';
    return b;
}

static bool lobby(const GtkNetplayDialog *d) {
    return d->info.state != NETPLAY_OFFLINE && d->info.state != NETPLAY_CONNECTING;
}

static uint32_t selected_game(GtkNetplayDialog *d) {
    GtkTreeModel *model;
    GtkTreeIter it;
    guint id = 0;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->games)), &model, &it))
        gtk_tree_model_get(model, &it, G_ID, &id, -1);
    return id;
}

static void set_log(GtkWidget *view, char (*lines)[NETPLAY_LINE_LEN], int n) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GString *s = g_string_new(NULL);
    for (int i = 0; i < n; i++) {
        g_string_append(s, lines[i]);
        if (i + 1 < n) g_string_append_c(s, '\n');
    }
    gtk_text_buffer_set_text(buf, s->str, -1);
    g_string_free(s, TRUE);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    GtkTextMark *mark = gtk_text_buffer_create_mark(buf, NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view), mark, 0, FALSE, 0, 1);
    gtk_text_buffer_delete_mark(buf, mark);
}

static void reload(GtkNetplayDialog *d) {
    static NetplayGame games[MAX_GAMES];
    static NetplayUser users[MAX_USERS];
    static char lines[MAX_LOG][NETPLAY_LINE_LEN];
    netplay_info(d->np, &d->info);
    d->seen_version = netplay_version(d->np);

    gtk_stack_set_visible_child_name(GTK_STACK(d->stack), lobby(d) ? "lobby" : "connect");
    gtk_widget_set_sensitive(d->connect, d->info.state == NETPLAY_OFFLINE);
    gtk_button_set_label(GTK_BUTTON(d->connect),
                         d->info.state == NETPLAY_CONNECTING ? "Connecting..." : "_Connect");

    /* the public servers (only rebuilt when they change, keeping the selection) */
    static NetplayServer sv[MAX_SERVERS];
    int ns = netplay_servers(d->np, sv, MAX_SERVERS);
    bool busy = netplay_servers_busy(d->np);
    gtk_widget_set_sensitive(d->refresh, !busy);
    gtk_button_set_label(GTK_BUTTON(d->refresh), busy ? "Fetching..." : "_Refresh list");
    if (ns != d->n_servers_shown || busy != d->busy_shown) {
        d->n_servers_shown = ns;
        d->busy_shown = busy;
        gtk_list_store_clear(d->server_store);
        for (int i = 0; i < ns; i++) {
            char games[16];
            snprintf(games, sizeof(games), "%d", sv[i].games);
            GtkTreeIter it;
            gtk_list_store_append(d->server_store, &it);
            gtk_list_store_set(d->server_store, &it, S_NAME, utf8(sv[i].name), S_ADDRESS, utf8(sv[i].address),
                               S_USERS, sv[i].users, S_GAMES, games, S_LOCATION, utf8(sv[i].location),
                               S_NUSERS, atoi(sv[i].users), S_NGAMES, sv[i].games, -1);
        }
    }
    /* pings, in place, so the list doesn't jump */
    GtkTreeModel *sm = GTK_TREE_MODEL(d->server_store);
    GtkTreeIter sit;
    for (gboolean ok = gtk_tree_model_get_iter_first(sm, &sit); ok; ok = gtk_tree_model_iter_next(sm, &sit)) {
        gchar *addr;
        gtk_tree_model_get(sm, &sit, S_ADDRESS, &addr, -1);
        for (int i = 0; i < ns; i++)
            if (!strcmp(sv[i].address, addr)) {
                char ping[16];
                snprintf(ping, sizeof(ping), sv[i].ping >= 0 ? "%d ms" : "-", sv[i].ping);
                gtk_list_store_set(d->server_store, &sit, S_PING, ping,
                                   S_NPING, sv[i].ping >= 0 ? sv[i].ping : NO_PING, -1);
                break;
            }
        g_free(addr);
    }

    /* games, keeping the selection */
    uint32_t keep = selected_game(d);
    int ng = netplay_games(d->np, games, MAX_GAMES);
    gtk_list_store_clear(d->game_store);
    static const char *const st[] = { "waiting", "playing", "syncing" };
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->games));
    for (int i = 0; i < ng; i++) {
        GtkTreeIter it;
        gtk_list_store_append(d->game_store, &it);
        gtk_list_store_set(d->game_store, &it, G_ID, games[i].id, G_NAME, utf8(games[i].name),
                           G_EMULATOR, utf8(games[i].emulator),
                           G_PLAYERS, games[i].players, G_STATUS, games[i].status < 3 ? st[games[i].status] : "?",
                           G_OWNER, utf8(games[i].owner), G_NPLAYERS, atoi(games[i].players), -1);
        if (games[i].id == keep) gtk_tree_selection_select_iter(sel, &it);
    }

    /* users: the server's, or the players in our game */
    bool room = d->info.state >= NETPLAY_ROOM;
    int nu = room ? netplay_room(d->np, users, NETPLAY_MAX_PLAYERS) : netplay_users(d->np, users, MAX_USERS);
    gtk_list_store_clear(d->user_store);
    for (int i = 0; i < nu; i++) {
        char name[64], ping[16];
        if (room) snprintf(name, sizeof(name), "P%d %s", i + 1, utf8(users[i].name));
        else snprintf(name, sizeof(name), "%s", utf8(users[i].name));
        snprintf(ping, sizeof(ping), "%u ms", users[i].ping);
        GtkTreeIter it;
        gtk_list_store_append(d->user_store, &it);
        gtk_list_store_set(d->user_store, &it, U_NAME, name, U_PING, ping, U_NPING, (gint)users[i].ping, -1);
    }
    gtk_label_set_text(GTK_LABEL(d->users_label), room ? "In this game" : "Users");

    /* the log: only when it changed, so scrolling back isn't undone */
    int n = netplay_log(d->np, lines, MAX_LOG);
    if (n != d->n_log_shown || (n && memcmp(lines[n - 1], d->log_shown[n - 1], NETPLAY_LINE_LEN))) {
        memcpy(d->log_shown, lines, (size_t)n * NETPLAY_LINE_LEN);
        d->n_log_shown = n;
        set_log(d->chat, lines, n);
        set_log(d->clog, lines, n);
    }

    NetplayState s = d->info.state;
    gtk_widget_set_sensitive(d->b_create, s == NETPLAY_LOBBY);
    gtk_widget_set_sensitive(d->b_join, s == NETPLAY_LOBBY && selected_game(d));
    gtk_widget_set_sensitive(d->b_leave, s >= NETPLAY_ROOM);
    gtk_widget_set_sensitive(d->b_start, s == NETPLAY_ROOM && d->info.owner);

    char status[200];
    const NetplayInfo *in = &d->info;
    switch (s) {
    case NETPLAY_OFFLINE:    snprintf(status, sizeof(status), "Not connected.  This game: %s", netplay_game_name(d->np)); break;
    case NETPLAY_CONNECTING: snprintf(status, sizeof(status), "Connecting to %s...", in->server); break;
    case NETPLAY_LOBBY:      snprintf(status, sizeof(status), "%s on %s - pick a game and Join, or Create Game", in->user, in->server); break;
    case NETPLAY_ROOM:       snprintf(status, sizeof(status), "In game %u%s - %s", in->game_id, in->owner ? " (yours)" : "",
                                      in->owner ? "press Start when everyone's in" : "waiting for the owner to start"); break;
    case NETPLAY_STARTING:   snprintf(status, sizeof(status), "Starting - waiting for every player to be ready..."); break;
    case NETPLAY_PLAYING:    snprintf(status, sizeof(status), "Playing: player %d of %d, delay %d frames, frame %llu",
                                      in->player, in->players, in->delay, (unsigned long long)in->frame); break;
    }
    gtk_label_set_text(GTK_LABEL(d->status), status);
}

/* ── Actions ─────────────────────────────────────────────────────────────── */

static void on_connect(GtkWidget *w, gpointer ud) {
    (void)w;
    GtkNetplayDialog *d = ud;
    char err[160];
    const char *server = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d->server))));
    int conn = gtk_combo_box_get_active(GTK_COMBO_BOX(d->conn)) + NETPLAY_CONN_MIN;
    if (!netplay_connect(d->np, NETPLAY_MODE_KAILLERA, server, gtk_entry_get_text(GTK_ENTRY(d->name)),
                         conn, err, sizeof(err)))
        netplay_note(d->np, "%s", err);
    reload(d);
}

static void on_refresh(GtkWidget *w, gpointer ud) {
    (void)w;
    GtkNetplayDialog *d = ud;
    netplay_refresh_servers(d->np);
    reload(d);
}

/* Picking a public server puts its address in the Server box. */
static void on_server_selected(GtkTreeSelection *sel, gpointer ud) {
    GtkNetplayDialog *d = ud;
    GtkTreeModel *model;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) return;
    gchar *addr;
    gtk_tree_model_get(model, &it, S_ADDRESS, &addr, -1);
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d->server))), utf8(addr));
    g_free(addr);
}

static void on_connect(GtkWidget *w, gpointer ud);
static void on_server_activated(GtkTreeView *v, GtkTreePath *p, GtkTreeViewColumn *c, gpointer ud) {
    (void)v; (void)p; (void)c;
    on_connect(NULL, ud);
}

static void on_create(GtkWidget *w, gpointer ud)     { (void)w; netplay_create_game(((GtkNetplayDialog *)ud)->np); }
static void on_leave(GtkWidget *w, gpointer ud)      { (void)w; netplay_leave_game(((GtkNetplayDialog *)ud)->np); }
static void on_start(GtkWidget *w, gpointer ud)      { (void)w; netplay_start_game(((GtkNetplayDialog *)ud)->np); }
static void on_disconnect(GtkWidget *w, gpointer ud) { (void)w; netplay_disconnect(((GtkNetplayDialog *)ud)->np); }

static void on_join(GtkWidget *w, gpointer ud) {
    (void)w;
    GtkNetplayDialog *d = ud;
    uint32_t id = selected_game(d);
    if (id) netplay_join_game(d->np, id);
}

static void on_game_activated(GtkTreeView *v, GtkTreePath *p, GtkTreeViewColumn *c, gpointer ud) {
    (void)v; (void)p; (void)c;
    on_join(NULL, ud);
}

static void on_game_selected(GtkTreeSelection *sel, gpointer ud) {
    (void)sel;
    GtkNetplayDialog *d = ud;
    gtk_widget_set_sensitive(d->b_join, d->info.state == NETPLAY_LOBBY && selected_game(d));
}

static void on_say(GtkWidget *w, gpointer ud) {
    (void)w;
    GtkNetplayDialog *d = ud;
    netplay_chat(d->np, gtk_entry_get_text(GTK_ENTRY(d->say)));
    gtk_entry_set_text(GTK_ENTRY(d->say), "");
}

static gboolean on_delete_event(GtkWidget *w, GdkEvent *ev, gpointer ud) {
    (void)ev; (void)ud;
    gtk_widget_hide(w);
    return TRUE;
}

/* ── NetplayDialog ───────────────────────────────────────────────────────── */

static void gtk_show(NetplayDialog *base) {
    GtkNetplayDialog *d = (GtkNetplayDialog *)base;
    if (!d->fetched) {                  /* the public lists, the first time */
        d->fetched = true;
        netplay_refresh_servers(d->np);
    }
    reload(d);
    gtk_widget_show_all(d->window);
    gtk_window_present(GTK_WINDOW(d->window));
    gtk_widget_grab_focus(lobby(d) ? d->say : gtk_bin_get_child(GTK_BIN(d->server)));
}

static void gtk_hide(NetplayDialog *base) {
    gtk_widget_hide(((GtkNetplayDialog *)base)->window);
}

static bool gtk_visible(const NetplayDialog *base) {
    return gtk_widget_get_visible(((const GtkNetplayDialog *)base)->window);
}

static void gtk_refresh(NetplayDialog *base) {
    GtkNetplayDialog *d = (GtkNetplayDialog *)base;
    if (!gtk_visible(base)) return;
    gint64 now = g_get_monotonic_time();
    if (!lobby(d) && d->n_servers_shown && now - d->last_ping > 5000000) {   /* every 5 s */
        d->last_ping = now;
        netplay_ping_servers(d->np);
    }
    if (netplay_version(d->np) != d->seen_version) reload(d);
}

static void gtk_destroy(NetplayDialog *base) {
    GtkNetplayDialog *d = (GtkNetplayDialog *)base;
    if (!d) return;
    gtk_widget_destroy(d->window);
    g_object_unref(d->game_store);
    g_object_unref(d->user_store);
    g_object_unref(d->server_store);
    free(d);
}

/* A resizable column showing text column `show`, sorted by `sort_by`. */
static void add_column(GtkWidget *view, const char *title, int show, int sort_by, bool expand) {
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        title, gtk_cell_renderer_text_new(), "text", show, NULL);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_expand(col, expand);
    gtk_tree_view_column_set_sort_column_id(col, sort_by);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
}

static GtkWidget *scrolled(GtkWidget *child) {
    GtkWidget *s = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(s), child);
    return s;
}

static GtkWidget *log_view(void) {
    GtkWidget *v = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(v), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(v), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(v), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(v), TRUE);
    return v;
}

static GtkWidget *button(const char *label, GCallback cb, gpointer ud) {
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    g_signal_connect(b, "clicked", cb, ud);
    return b;
}

static GtkWidget *label_left(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

static GtkWidget *connect_page(GtkNetplayDialog *d) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);

    GtkWidget *mode = gtk_combo_box_text_new();
    for (int m = 0; m < NETPLAY_N_MODES; m++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), netplay_mode_name((NetplayMode)m));
    gtk_combo_box_set_active(GTK_COMBO_BOX(mode), 0);
    gtk_grid_attach(GTK_GRID(grid), label_left("Mode:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), mode, 1, 0, 1, 1);

    char user[NETPLAY_NAME_LEN];
    int conn;
    netplay_last_settings(user, sizeof(user), &conn);
    d->name = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(d->name), NETPLAY_NAME_LEN - 1);
    gtk_entry_set_text(GTK_ENTRY(d->name), user);
    gtk_grid_attach(GTK_GRID(grid), label_left("Name:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), d->name, 1, 1, 1, 1);

    d->server = gtk_combo_box_text_new_with_entry();
    char recent[8][128];
    int n = netplay_recent_servers(recent, 8);
    for (int i = 0; i < n; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->server), recent[i]);
    if (n) gtk_combo_box_set_active(GTK_COMBO_BOX(d->server), 0);
    GtkWidget *server_entry = gtk_bin_get_child(GTK_BIN(d->server));
    gtk_entry_set_placeholder_text(GTK_ENTRY(server_entry), "host[:port]");
    gtk_entry_set_width_chars(GTK_ENTRY(server_entry), 40);
    g_signal_connect(server_entry, "activate", G_CALLBACK(on_connect), d);
    gtk_grid_attach(GTK_GRID(grid), label_left("Server:"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), d->server, 1, 2, 1, 1);

    d->conn = gtk_combo_box_text_new();
    for (int c = NETPLAY_CONN_MIN; c <= NETPLAY_CONN_MAX; c++) {
        char label[64];
        snprintf(label, sizeof(label), "%s (%d frame%s a packet)", netplay_conn_name(c), c, c == 1 ? "" : "s");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->conn), label);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->conn), conn - NETPLAY_CONN_MIN);
    gtk_grid_attach(GTK_GRID(grid), label_left("Connection:"), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), d->conn, 1, 3, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    d->connect = button("_Connect", G_CALLBACK(on_connect), d);
    gtk_box_pack_start(GTK_BOX(row), d->connect, FALSE, FALSE, 0);
    GtkWidget *close = gtk_button_new_with_mnemonic("C_lose");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_hide), d->window);
    gtk_box_pack_end(GTK_BOX(row), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    /* the public servers */
    d->server_store = gtk_list_store_new(S_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                         G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                         G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);
    d->servers = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->server_store));
    add_column(d->servers, "Server", S_NAME, S_NAME, true);
    add_column(d->servers, "Address", S_ADDRESS, S_ADDRESS, false);
    add_column(d->servers, "Users", S_USERS, S_NUSERS, false);
    add_column(d->servers, "Games", S_GAMES, S_NGAMES, false);
    add_column(d->servers, "Ping", S_PING, S_NPING, false);
    add_column(d->servers, "Location", S_LOCATION, S_LOCATION, false);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->server_store), S_NUSERS,
                                         GTK_SORT_DESCENDING);   /* most users first */
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->servers)), "changed",
                     G_CALLBACK(on_server_selected), d);
    g_signal_connect(d->servers, "row-activated", G_CALLBACK(on_server_activated), d);
    GtkWidget *sbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(sbox), label_left("Public servers (double-click to connect):"), FALSE, FALSE, 0);
    d->refresh = button("_Refresh list", G_CALLBACK(on_refresh), d);
    gtk_box_pack_end(GTK_BOX(sbox), d->refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scrolled(d->servers), TRUE, TRUE, 0);

    d->clog = log_view();
    GtkWidget *clog = scrolled(d->clog);
    gtk_widget_set_size_request(clog, -1, 90);
    gtk_box_pack_start(GTK_BOX(box), clog, FALSE, TRUE, 0);
    return box;
}

static GtkWidget *lobby_page(GtkNetplayDialog *d) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *lists = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    d->game_store = gtk_list_store_new(G_COLS, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                       G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    d->games = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->game_store));
    add_column(d->games, "Game", G_NAME, G_NAME, true);
    add_column(d->games, "Emulator", G_EMULATOR, G_EMULATOR, false);
    add_column(d->games, "Players", G_PLAYERS, G_NPLAYERS, false);
    add_column(d->games, "Status", G_STATUS, G_STATUS, false);
    add_column(d->games, "Owner", G_OWNER, G_OWNER, false);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->game_store), G_NPLAYERS,
                                         GTK_SORT_DESCENDING);   /* most players first */
    g_signal_connect(d->games, "row-activated", G_CALLBACK(on_game_activated), d);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->games)), "changed",
                     G_CALLBACK(on_game_selected), d);
    gtk_paned_pack1(GTK_PANED(lists), scrolled(d->games), TRUE, FALSE);

    GtkWidget *ubox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    d->users_label = label_left("Users");
    gtk_box_pack_start(GTK_BOX(ubox), d->users_label, FALSE, FALSE, 0);
    d->user_store = gtk_list_store_new(U_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    d->users = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->user_store));
    add_column(d->users, "Name", U_NAME, U_NAME, true);
    add_column(d->users, "Ping", U_PING, U_NPING, false);
    gtk_box_pack_start(GTK_BOX(ubox), scrolled(d->users), TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(lists), ubox, FALSE, FALSE);
    gtk_paned_set_position(GTK_PANED(lists), 600);
    gtk_box_pack_start(GTK_BOX(box), lists, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    d->b_create = button("C_reate Game", G_CALLBACK(on_create), d);
    d->b_join   = button("_Join", G_CALLBACK(on_join), d);
    d->b_leave  = button("_Leave", G_CALLBACK(on_leave), d);
    d->b_start  = button("_Start", G_CALLBACK(on_start), d);
    GtkWidget *bs[] = { d->b_create, d->b_join, d->b_leave, d->b_start };
    for (int i = 0; i < 4; i++) gtk_box_pack_start(GTK_BOX(row), bs[i], FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), button("_Disconnect", G_CALLBACK(on_disconnect), d), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    d->chat = log_view();
    gtk_box_pack_start(GTK_BOX(box), scrolled(d->chat), TRUE, TRUE, 0);
    GtkWidget *say = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    d->say = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(d->say), 120);
    g_signal_connect(d->say, "activate", G_CALLBACK(on_say), d);
    gtk_box_pack_start(GTK_BOX(say), d->say, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(say), button("Se_nd", G_CALLBACK(on_say), d), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), say, FALSE, FALSE, 0);
    return box;
}

NetplayDialog *netplay_dialog_gtk_create(Netplay *np) {
    if (!np) return NULL;
    GtkNetplayDialog *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->base = (NetplayDialog){ gtk_show, gtk_hide, gtk_visible, gtk_refresh, gtk_destroy };
    d->np = np;
    d->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(d->window), "GEMU - Netplay");
    gtk_window_set_default_size(GTK_WINDOW(d->window), 860, 560);
    g_signal_connect(d->window, "delete-event", G_CALLBACK(on_delete_event), d);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(d->window), box);
    d->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(d->stack), connect_page(d), "connect");
    gtk_stack_add_named(GTK_STACK(d->stack), lobby_page(d), "lobby");
    gtk_box_pack_start(GTK_BOX(box), d->stack, TRUE, TRUE, 0);
    d->status = label_left("");
    gtk_box_pack_start(GTK_BOX(box), d->status, FALSE, FALSE, 0);
    return &d->base;
}
#endif /* GEMU_GTK */
