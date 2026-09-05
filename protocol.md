# Protocol — Layer 1

Transport: WebSocket, one JSON object per text frame.

## Envelope

```
client -> server   {"type": "auth", "seq": 3, "data": {...}}
server reply       {"type": "welcome", "seq": 3, "data": {...}}     <- echoes seq
server event       {"type": "player_joined", "data": {...}}         <- no seq, unsolicited
error reply        {"type": "error", "seq": 3, "data": {"code": "bad_credentials", "message": "..."}}
```

Rules:

- Every client message carries `seq`, a client-chosen integer increasing per message.
- The server answers a client message with exactly one reply echoing its `seq`
  (either the success type or `error`).
- Server events carry no `seq`.
- Unknown fields are ignored; new features add new `type` values.
- A new feature never changes the meaning of an existing message.

## Connection lifecycle

```
state: NEW ---hello---> GREETED ---auth---> AUTHED
```

Messages arriving in the wrong state get `error {"code": "bad_state"}`.
Anything unparsable gets `error {"code": "bad_json"}` (seq 0) and the
connection is closed.

### hello

```
c->s  {"type": "hello", "seq": 1, "data": {"proto": 1, "client": "gridmmo/0.1"}}
s->c  {"type": "hello_ok", "seq": 1, "data": {"proto": 1, "motd": "..."}}
```

`proto` mismatch: `error {"code": "version_mismatch"}` and the server closes.

### auth

By password (registers the name on first use):

```
c->s  {"type": "auth", "seq": 2, "data": {"name": "sagi", "password": "..."}}
s->c  {"type": "welcome", "seq": 2, "data": {"player_id": 1,
                                             "token": "9f2c...",
                                             "settings": {...},
                                             "online": ["sagi", "dana"]}}
```

By token (resume, no password):

```
c->s  {"type": "auth", "seq": 2, "data": {"name": "sagi", "token": "9f2c..."}}
```

- Wrong password / unknown token: `error {"code": "bad_credentials"}`.
- The token is rotated on every successful password login.
- Logging in while already connected kicks the old connection
  (it receives event `kicked` and is closed); the new connection wins.
- `online` is the snapshot; `player_joined` / `player_left` are the deltas.

### ping

Allowed in GREETED and AUTHED. `t` is opaque to the server and echoed back;
the client sends its own clock to measure RTT.

```
c->s  {"type": "ping", "seq": 7, "data": {"t": 1725450000123}}
s->c  {"type": "pong", "seq": 7, "data": {"t": 1725450000123}}
```

The server drops connections silent for 30 seconds. Clients should ping
every 10 seconds.

### settings

Server-side persisted JSON blob per player, schema-free, max 16 KiB.
Returned in `welcome`.

```
c->s  {"type": "set_settings", "seq": 4, "data": {"settings": {...}}}
s->c  {"type": "settings_ok", "seq": 4, "data": {}}

c->s  {"type": "get_settings", "seq": 5, "data": {}}
s->c  {"type": "settings", "seq": 5, "data": {"settings": {...}}}
```

## Events (server -> client, AUTHED only)

```
{"type": "player_joined", "data": {"name": "dana"}}
{"type": "player_left",   "data": {"name": "dana"}}
{"type": "kicked",        "data": {"reason": "logged_in_elsewhere"}}
```

## Error codes

```
bad_json           unparsable frame or missing envelope fields (closes)
bad_state          message not allowed in current lifecycle state
unknown_type       type not recognized
version_mismatch   proto incompatible (closes)
bad_credentials    wrong password or unknown/stale token
bad_request        malformed data for a known type
too_large          settings blob over the size cap
```

# Protocol — Layer 2: the grid world

## map (event, after welcome)

Chunk-shaped; today the whole map is one chunk at (0,0).

```
s->c  {"type": "map", "data": {"x": 0, "y": 0, "w": 40, "h": 25, "tiles": [0,0,1,...]}}
```

`tiles` is row-major. `0` floor, `1` wall. The server loads it from an
ASCII file: `.` floor, `#` wall, `S` spawn (unknown chars become walls;
short lines are padded with walls).

## state (event)

A full snapshot right after `map` (includes yourself), then tick-batched
deltas (20 Hz), sent only when something changed. Deltas never include
your own moves — those arrive in `intent_ok` replies, so the client can
render its own movement optimistically without double-applying.

```
s->c  {"type": "state", "data": {"full": true, "players": [{"name": "sagi", "x": 4, "y": 7}]}}
s->c  {"type": "state", "data": {"players": [{"name": "dana", "x": 5, "y": 7}], "gone": ["mike"]}}
```

`gone` lists despawned players. `player_joined`/`player_left` remain pure
presence; positions always travel in `state`.

## intent (AUTHED only)

Operators and motions compose — new operators multiply nothing.

```
c->s  {"type": "intent", "seq": 9, "data": {"op": "move", "motion": "j", "count": 3}}
s->c  {"type": "intent_ok", "seq": 9, "data": {"granted": 2, "x": 4, "y": 9}}
```

- `op`: `move`, `melee`, `defend` (motion `on`/`off`; while defending, move
  and melee grant 0; stamina drains at `defend_drain`/s and regen pauses; a
  shield soaks the whole remaining melee pool — blocked points cost 1
  stamina each, the uncovered rest hits hp and breaks the guard; guard
  auto-breaks at 0 stamina; `defend` events `{"name", "on"}` broadcast to
  everyone, and full snapshots mark defenders with `"def": 1`).
  Reserved: grab, talk, action.
- `motion`: move accepts `h j k l` and dash `H J K L` (capitals = a
  `dash_mult`-tile step in that direction); melee accepts `h j k l` only.
- `count` is *requested*, 1..4096; `granted` is what the world allowed.
  Movement is one atomic hop: `granted = min(count, free tiles until
  wall/player/map edge)`. `granted: 0` (against a wall) is a normal reply,
  not an error.
- Players block each other; each tile holds one entity.
- Positions persist across sessions (saved on disconnect); first-ever
  login spawns at `S`, an occupied spawn resolves to the nearest free tile.
- No rate limit on `move` in this layer; stamina becomes the natural
  limiter in a later layer.

# Protocol — Layer 3: combat

## rules (in welcome)

Server tuning shipped to the client, which simulates regen locally between
authoritative checkpoints (every `intent_ok`, `vitals` and `state` hp entry
snaps the simulation straight). The wire is silent while bars refill.

```
"rules": {"hp_max": 100, "stamina_max": 100, "hp_regen": 1.0,
          "stamina_regen": 8.0, "move_cost": 1, "melee_cost": 2, "dash_mult": 4}
```

## costs

- `move`: 1 stamina per granted tile; granted also caps at what is affordable.
- `melee`: `granted = min(count, floor(stamina / melee_cost))` pool points,
  committed on the swing — whiffs cost full stamina.

`intent_ok` now always carries vitals:

```
s->c  {"type": "intent_ok", "seq": 9, "data": {"granted": 5, "x": 4, "y": 7,
                                               "hp": 100, "stamina": 88.4}}
```

## melee resolution (draft pool semantics + commitment)

A melee is committed at intent time (stamina charged, `intent_ok` grants the
pool) but resolves after a **wind-up** of `min(pool * melee_windup,
windup_max)` seconds, followed by a **recovery** of `min(pool *
melee_recover, recover_max)` during which all intents grant 0. A `windup`
event broadcasts the telegraph; the `melee` event fires at the strike
(now sent to the attacker too — clients animate at strike time):

```
s->c  {"type": "windup", "data": {"by": "dana", "motion": "l", "count": 12,
                                  "x": 4, "y": 7, "strike_in": 0.36}}
```

The pool travels the motion line until a wall stops it: each gap beyond
adjacent costs `distance - 1` pool; the first entity absorbs pool capped at
its health; leftover pierces only through kills. Positions are read at
strike time — wound-up attacks are dodgeable.

**Directional guard**: the server tracks each entity's facing (last move or
melee direction). A shield only blocks attacks arriving from the faced
direction; flanked defenders take full hp damage and keep their guard up.
Chasers telegraph bites (`windup`, 0.45s) with the direction locked —
stepping away dodges. Rules gained `melee_windup`, `windup_max`,
`melee_recover`, `recover_max`.

## events

```
{"type": "melee",  "data": {"by": "dana", "motion": "l", "count": 5, "x": 4, "y": 7}}
{"type": "died",   "data": {"name": "sagi", "by": "dana"}}
{"type": "vitals", "data": {"hp": 93.0, "stamina": 70.0}}            <- to the victim
{"type": "vitals", "data": {"hp": 100, "stamina": 100, "x": 10, "y": 8}}  <- respawn (position included)
```

`melee` is broadcast to everyone but the attacker (who animates its own
swing optimistically). `state` delta entries gain optional `"hp"`/`"hp_max"`
when a player was hurt. Death refills both bars and respawns at `S`
(players) or at the entity's home (npcs).

## npcs

Server-side entities living in the same session registry: they appear in
`online`, `state`, and are valid melee targets; they send nothing. Started
by default, disabled with `--no-npcs`. Current set: `dummy_1..3` (30 hp,
static, in a row near spawn) and `walker` (60 hp, wanders).

## magic (spells)

The server owns the active spell. The catalog ships in `welcome`
(`"spells": [{name, cost, speed, range, windup, recover}]` plus
`"active_spell"`); selection is its own message; casts never name a spell:

```
c->s  {"type": "spell", "seq": 6, "data": {"spell": "firebolt"}}
s->c  {"type": "spell_ok", "seq": 6, "data": {"spell": "firebolt"}}

c->s  {"type": "intent", "seq": 9, "data": {"op": "magic", "motion": "l", "count": 8}}
```

`magic` follows the melee commitment flow (charge at intent, spell-scaled
wind-up with a `windup` telegraph whose count is the aim-line range, then
recovery). At the strike a projectile launches:

```
s->c  {"type": "bolt", "data": {"by": "sagi", "motion": "l", "x": 4, "y": 7,
                                "speed": 9.0, "range": 15, "count": 8}}
```

The server advances bolts each tick: walls stop them, the first entity on
the path takes `count` damage (directional guard applies), no pierce, and
they fizzle at max range. Clients animate the flight locally from the
single `bolt` event — a dodging target simply isn't there when it arrives.

firebolt: cost 3/pt, speed 9 tiles/s, range 15, windup 0.04s/pt, no recovery.

Tile-targeted spells (`"targeting": "tile"` in the catalog) cast at a mark
instead of a motion — the intent carries `tx`/`ty` (chebyshev distance <=
the spell's `range`, floor tiles only). After the wind-up the strike is
scheduled `delay_base + delay_per * count` seconds out and announced:

```
c->s  {"type": "intent", "seq": 9, "data": {"op": "magic", "count": 10, "tx": 14, "ty": 7}}
s->c  {"type": "missile", "data": {"by": "sagi", "sx": 10, "sy": 7,
                                   "x": 14, "y": 7, "count": 10, "eta": 1.4}}
```

At impact, whoever stands on the mark takes `count` damage — shields do
not block falling fire; the only defence is not being there. The mark is
visible to everyone for the whole flight.

firemissile: cost 4/pt, target range 7, delay 0.6 + 0.08/pt.

## the dot register (server-side)

The server records every committed offensive action (melee, cast, or
missile — the mark stored as an offset from the caster) into a per-session
register. Three wire forms:

```
c->s  {"type": "intent", "seq": 9, "data": {"op": "melee", "motion": "l",
                                            "count": 9, "load": true}}    <- arm only: free, no windup
c->s  {"type": "intent", "seq": 10, "data": {"op": "repeat"}}             <- fire the register
c->s  {"type": "intent", "seq": 11, "data": {"op": "repeat", "count": 2}} <- override; the override sticks
```

Repeats run the stored action through the full normal pipeline (stamina,
busy gates, wind-up, telegraphs) using the spell stored at record time,
not the currently active one; missile marks are re-applied relative to
the caster's current position. Empty register: granted 0. Windup events
carry `"op"` so clients can drive cast bars for repeated actions.

# Protocol — Challenge mode

Started with `--challenge <file>`, the server drops each player into a
private instance of the challenge (own map occupancy, own npcs, own
deltas — the open world is simply one shared instance of the same
machinery). No position/vitals persistence; every entry is the file's
prescribed conditions.

```
s->c  {"type": "challenge", "data": {"status": "start", "objective": "kill all enemies", "time": 0}}
s->c  {"type": "challenge", "data": {"status": "won",   "objective": "...", "time": 42.7}}
s->c  {"type": "challenge", "data": {"status": "lost",  "objective": "...", "time": 3.1}}

c->s  {"type": "retry", "seq": 9, "data": {}}      <- resets the instance (challenge servers only)
s->c  {"type": "retry_ok", "seq": 9, "data": {}}   then fresh vitals + state + challenge start
```

- After won/lost the instance freezes: all intents grant 0 until `retry`.
- Objectives: `kill_all` (challenge npcs never respawn), `survive <sec>`,
  `reach <x,y>`. Death = lost.
- Npc kinds: `dummy` (static), `walker` (wanders), `chaser` (hunts the
  nearest player, bites `damage` when adjacent; shields block bites).
- Challenge `[rules]` merge over server defaults and ship in `welcome`.

## Wire format evolution

JSON while the protocol is young. When state traffic needs it, hot paths
(`state`, `intent`) move to binary frames — a binary WebSocket frame whose
first byte selects the codec — while control messages stay JSON. The
envelope above never changes.
