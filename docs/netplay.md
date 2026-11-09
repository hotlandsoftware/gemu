# Netplay

The NES and Famicom (`-M nes`, `nespal`, `famicom`, `dendy`) can be played
with other people over the network, through a **Kaillera** server: the
public lobbies run EmuLinker-K or the original Kaillera server. Nothing
extra needs installing; GEMU talks to the server itself.

Everyone in a game plays with GEMU: Kaillera only passes controller input
around, and other emulators emulate the NES differently, so their games
would drift apart. Servers won't start a game whose players use different
emulators (GEMU's name includes its version, so two different GEMU
versions can't be mixed either).

## Playing

1. **Netplay > Connect...** Type your name, pick a server and a connection
   type, and press Connect. The list below has the servers you've used, then
   the public ones from the Kaillera and Kaillera Reborn (EmuLinker) server
   lists, with how many users and games each has and your ping to it
   (measured every few seconds while the list is open): click one to use it,
   double-click to connect. Click a column's header to sort by it (again to
   reverse), drag a header's edge to resize the column; servers start with
   the busiest first, as games in the lobby do. **Refresh list** fetches the lists again. You can
   also type any server as `host` or `host:port` (the port is 27888 unless
   the server says otherwise). GEMU remembers your name, connection type and
   the servers you've logged in to.
2. The window becomes the **lobby**: the server's games, its users, and the
   chat. **Create Game** offers the game you have loaded; the others pick it
   and press **Join** (or double-click it). A game is listed under the ROM's
   name and a short code made from the ROM's contents and the region and
   controller settings, so only players whose games would stay in step can
   join. To join someone's game, load the same ROM first. The users' pings
   are the ones the server measured when they logged in: Kaillera servers
   don't measure them again.
3. When everyone is in, the game's owner presses **Start**. Every player's
   console powers on at the same moment, and player 1 and 2's controllers are
   the two pads (if your NES has just the one controller - the default - a
   second goes into port 2 for the game). You always play on your own controller 1 (Input menu); the
   game puts it in your player's port.
4. **Leave** ends the game for you; **Disconnect** leaves the server. Your
   own game comes back as it was before the netplay game, battery save
   included: nothing from a netplay game is written to your save.

**Netplay > Lobby...** reopens the window during a game, for the chat.

### Connection type

Each type sends input every so many frames: LAN 1, Excellent 2, Good 3,
Average 4, Low 5, Bad 6. Fewer frames per packet means less delay but more
packets. Everyone in a game must choose the same type. The server works out
the input delay from your ping when the game starts; the lobby shows it
("delay 5 frames").

### During a game

Every player's console runs the same frames with the same input, so
anything that would change only one of them is locked out:

- **Reset** is passed to everyone, and every console resets on the same frame.
- **Pause** (or a debugger stop) holds the game for everyone.
- `loadvm`, changing the cartridge, and Game Genie codes are disabled.
- The FDS, VS. System games and Lua scripts can't be played online yet.

Every five seconds the players compare their consoles' memory. If they ever
differ, the chat says `*** Desync` - usually a different ROM or setting.

If the game was loaded with Blocks > Change... rather than `-cartridge`,
restart GEMU with `-cartridge FILE` to play it online: a netplay game starts
from the console as it was created, and swapping the cartridge leaves it
different from everyone else's.

## Monitor

`netplay` does everything the window does:

```
(gemu) netplay servers     (the public lists; "netplay servers refresh" fetches them again)
(gemu) netplay connect myserver.example:27888 alice 3
(gemu) netplay games
(gemu) netplay join 12
(gemu) netplay chat hi!
(gemu) netplay start
(gemu) netplay            (state, game, player, delay, frame)
(gemu) netplay log 30
(gemu) netplay leave
(gemu) netplay disconnect
```
