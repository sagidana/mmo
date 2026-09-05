# gridmmo

A keyboard-centric (vim-style) grid-world MMO. Client in C + raylib,
server in Python asyncio, JSON over WebSocket. See `protocol.md` for the
wire protocol.

Built in layers. **Current: Layer 2** — the grid world: walk a map drawn
in an ASCII file, vim-style (`12j`, `3l`), with other players live.

## Server

```
cd gridmmo
pip install -r server/requirements.txt
python3 -m server.main --host 0.0.0.0 --port 4000 --db gridmmo.db --map server/map.txt
```

State lives in one SQLite file (`--db`). First login with a new name
registers it. The map is an ASCII file (`.` floor, `#` wall, `S` spawn) —
edit it in vim, restart the server. Training NPCs spawn near `S` by default
(dummies to hit, a wanderer); `--no-npcs` disables them.

### Challenge mode

`--challenge challenges/example.txt` runs the server as a challenge:
every player gets a private instance with prescribed conditions (map,
starting vitals, npcs, objective) and must overcome it — result and time
show on screen, `:retry` resets. Challenge files are single vim-editable
text files; see `challenges/example.txt` for the format.

## Client

Needs raylib. Either install it system-wide (`pkg-config raylib` must work)
or point the build at a raylib release directory:

```
cd client
make client                                  # system raylib
make client RAYLIB_DIR=~/raylib-5.5_linux_amd64   # or a release dir
./client
```

First boot asks for a name + password once and saves them to `gridmmo.cfg`
next to the binary (plaintext — hobby-grade). After that the client opens
on an empty home screen: `:connect` joins the last server (first ever
connect uses the built-in default), `:connect host:port` joins another,
`:setup` changes the saved identity. First login on a server registers it.

### Windows build (for friends)

Cross-compiled from linux; produces a single static `gridmmo.exe` with no
dependencies — just send the file:

```
make windows WINCC=<llvm-mingw>/bin/x86_64-w64-mingw32-gcc \
             RAYLIB_WIN_DIR=<raylib-5.5_win64_mingw-w64>
```

Toolchain: https://github.com/mstorsjo/llvm-mingw (ucrt ubuntu release) and
raylib's `win64_mingw-w64` release zip. SmartScreen will warn on first run
(unsigned exe) — "More info" -> "Run anyway".

`make test_cli` builds a headless protocol exerciser (no raylib needed):

```
./test_cli localhost 4000 myname mypassword
```

## Layout

```
protocol.md    the wire protocol, layer by layer
server/        python asyncio server (websockets + sqlite)
client/        C raylib client
  net.c        non-blocking tcp
  ws.c         rfc 6455 websocket client (no deps)
  protocol.c   json envelope helpers (vendored cJSON)
  main.c       screens: connect -> login -> lobby
```

## Keys (world)

```
h j k l        move (count prefix works: 12j), costs stamina
H J K L        dash (4 tiles per press: shift + move)
d{motion}      melee attack (count = damage pool: 5dl)
x              raise guard (blocks the faced direction; esc or x releases)
ctrl-hjkl      turn in place (aim / steer the guard)
m{motion}      cast the active spell (count = power: 8ml)
               tile spells (fire missile) open an aim cursor instead:
               hjkl+counts move it, enter drops the mark, esc cancels
s              open the spellbook (j/k, enter selects)
d/m + ctrl-dir  arm the action into . without firing (100m ctrl-l)
.              fire the register (server-side: last/armed melee, cast,
               or missile — missiles re-aim relative to where you stand)
esc            clear pending count / operator
zz / zZ        center screen vertically / both axes
zh zl zk zj    view the area left / right / above / below
+ / -          zoom
: or ~         open the console (event log + command prompt)
```

## Commands

```
:q                    quit
:disconnect           back to the home screen
:connect [host:port]  connect + log in (no arg = last/default address)
:setup                change the saved name/password
:mute                 toggle sound

Commands run from the console prompt (`:` or `~` opens it): enter runs,
esc closes, tab completes/cycles command names, arrow up/down walks
history (persisted in gridmmo.hist; seeded with the last server so
up-arrow always recalls it), ctrl-u/d scrolls the event log.

```

## Layers

1. **done** — envelope, hello/auth handshake, tokens, settings, presence
   events, ping/reap.
2. **done** — grid world: ASCII map, `intent` movement (`{op, motion,
   count}`), collision (walls + players), 20Hz tick-batched `state` deltas,
   optimistic echo, position persistence, 3D block rendering.
3. **done** — combat: stamina economy, melee (`5dl` draft pool semantics),
   health/damage/death/respawn, training NPCs, hp bars + hit flashes.
4. next — dashes (`w`/`b`), `dd` (attack last opponent), then magic/defence.
