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
edit it in vim, restart the server.

## Client

Needs raylib. Either install it system-wide (`pkg-config raylib` must work)
or point the build at a raylib release directory:

```
cd client
make client                                  # system raylib
make client RAYLIB_DIR=~/raylib-5.5_linux_amd64   # or a release dir
./client
```

Type the server address (`host:port`), enter, then name + password.
First login registers the name. The lobby shows who is online.

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
h j k l        move (count prefix works: 12j)
esc            clear pending count
zz / zZ        center screen vertically / both axes
zh zl zk zj    view the area left / right / above / below
+ / -          zoom
:              command mode
```

## Commands

```
:q                    quit
:disconnect           back to the connect screen
:connect [host:port]  connect (no arg = current address)

```

## Layers

1. **done** — envelope, hello/auth handshake, tokens, settings, presence
   events, ping/reap.
2. **done** — grid world: ASCII map, `intent` movement (`{op, motion,
   count}`), collision (walls + players), 20Hz tick-batched `state` deltas,
   optimistic echo, position persistence, 3D block rendering.
3. next — the operator grammar: melee (`3dl`), stamina, dashes, health.
