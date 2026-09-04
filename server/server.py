import websockets
import asyncio
import random
import json
import time

from . import protocol
from .auth import Auth
from .db import Db
from .world import World
from .world import MOTIONS
from .world import MOVE_DIRS
from .world import DASH_MOTIONS


NEW = 'new'
GREETED = 'greeted'
AUTHED = 'authed'

OUTBOUND_QUEUE_SIZE = 256
SILENCE_TIMEOUT = 30.0
REAPER_INTERVAL = 5.0
TICK_INTERVAL = 1.0 / 20
MAX_COUNT = 4096
NPC_DEATH_DELAY = 4.0

RULES = {}
RULES['hp_max'] = 100.0
RULES['stamina_max'] = 100.0
RULES['hp_regen'] = 1.0
RULES['stamina_regen'] = 8.0
RULES['move_cost'] = 1
RULES['melee_cost'] = 2
RULES['dash_mult'] = 4


class Session():
    def __init__(self, websocket):
        self.websocket = websocket
        self.state = NEW
        self.player = None
        self.outbound = asyncio.Queue(maxsize=OUTBOUND_QUEUE_SIZE)
        self.writer_task = None
        self.last_recv = time.time()
        self.x = None
        self.y = None
        self.hp = RULES['hp_max']
        self.hp_max = RULES['hp_max']
        self.stamina = RULES['stamina_max']
        self.is_npc = False
        self.dead = False
        self.home = None

    def vitals(self):
        data = {}
        data['hp'] = round(self.hp, 1)
        data['stamina'] = round(self.stamina, 1)
        return data

    @property
    def name(self):
        if self.player is None: return None
        return self.player['name']

    def send(self, raw):
        # never blocks: a client that cannot drain its queue is dropped
        try:
            self.outbound.put_nowait(raw)
        except asyncio.QueueFull:
            self.kill()

    def kill(self):
        task = asyncio.get_running_loop().create_task(self.websocket.close())
        task.add_done_callback(lambda _: None)

    async def writer(self):
        while True:
            raw = await self.outbound.get()
            await self.websocket.send(raw)


class Npc():
    # duck-types the Session surface the server touches; wire methods are no-ops
    def __init__(self, name, x, y, hp, wander=False):
        self.name = name
        self.x = x
        self.y = y
        self.hp = hp
        self.hp_max = hp
        self.stamina = RULES['stamina_max']
        self.state = AUTHED
        self.is_npc = True
        self.dead = False
        self.respawn_at = 0.0
        self.home = (x, y)
        self.wander = wander
        self.next_act = 0.0

    def send(self, raw):
        pass

    def kill(self):
        pass

    def vitals(self):
        data = {}
        data['hp'] = round(self.hp, 1)
        data['stamina'] = round(self.stamina, 1)
        return data


class Server():
    def __init__(self, host, port, db_path, map_path, motd="welcome to gridmmo", npcs=True):
        self.host = host
        self.port = port
        self.motd = motd
        self.db = Db(db_path)
        self.auth = Auth(self.db)
        self.world = World(map_path)
        self.sessions = {}
        self.dirty = {}
        self.gone = []
        if npcs: self.spawn_npcs()

        self.handlers = {}
        self.handlers['hello'] = self.on_hello
        self.handlers['auth'] = self.on_auth
        self.handlers['ping'] = self.on_ping
        self.handlers['get_settings'] = self.on_get_settings
        self.handlers['set_settings'] = self.on_set_settings
        self.handlers['intent'] = self.on_intent

    def spawn_npcs(self):
        # training targets near spawn: a row of dummies (pool/gap/pierce testing)
        # and a wanderer that walks about
        sx, sy = self.world.spawn
        offsets = (3, 5, 7)
        i = 1
        for offset in offsets:
            spot = self.world.find_free_near(sx + offset, sy, self.occupied_set())
            if spot is None: continue
            npc = Npc(f"dummy_{i}", spot[0], spot[1], hp=30.0)
            self.sessions[npc.name] = npc
            i += 1

        spot = self.world.find_free_near(sx, sy + 5, self.occupied_set())
        if spot is not None:
            npc = Npc("walker", spot[0], spot[1], hp=60.0, wander=True)
            self.sessions[npc.name] = npc

    def act_npcs(self):
        now = time.time()
        for name in self.sessions:
            session = self.sessions[name]
            if not session.is_npc: continue
            if session.dead: continue
            if not session.wander: continue
            if now < session.next_act: continue

            session.next_act = now + random.uniform(0.5, 1.2)
            motion = random.choice(('h', 'j', 'k', 'l'))
            occupied = self.occupied_set(exclude=name)
            granted, x, y = self.world.resolve_move(session.x, session.y, motion, 1, occupied)
            if granted <= 0: continue
            session.x = x
            session.y = y
            self.mark_moved(session)

    def occupied_set(self, exclude=None):
        occupied = set()
        for name in self.sessions:
            if name == exclude: continue
            session = self.sessions[name]
            if session.dead: continue
            occupied.add((session.x, session.y))
        return occupied

    def occupied_sessions(self, exclude=None):
        occupied = {}
        for name in self.sessions:
            if name == exclude: continue
            session = self.sessions[name]
            if session.dead: continue
            occupied[(session.x, session.y)] = session
        return occupied

    def mark_moved(self, session):
        entry = self.dirty.get(session.name)
        if entry is None:
            entry = {}
            self.dirty[session.name] = entry
        entry['x'] = session.x
        entry['y'] = session.y

    def mark_hurt(self, session):
        self.mark_moved(session)
        self.dirty[session.name]['hp'] = round(session.hp, 1)
        self.dirty[session.name]['hp_max'] = session.hp_max

    def online_names(self):
        names = []
        for name in self.sessions:
            names.append(name)
        return names

    def broadcast(self, raw, exclude=None):
        for name in self.sessions:
            if name == exclude: continue
            self.sessions[name].send(raw)

    async def on_hello(self, session, seq, data):
        if session.state != NEW:
            session.send(protocol.error(seq, 'bad_state', "hello already done"))
            return

        proto = data.get('proto')
        if proto != protocol.PROTO_VERSION:
            session.send(protocol.error(seq, 'version_mismatch',
                                        f"server speaks proto {protocol.PROTO_VERSION}"))
            session.kill()
            return

        session.state = GREETED
        reply_data = {}
        reply_data['proto'] = protocol.PROTO_VERSION
        reply_data['motd'] = self.motd
        session.send(protocol.reply('hello_ok', seq, reply_data))

    async def on_auth(self, session, seq, data):
        if session.state != GREETED:
            session.send(protocol.error(seq, 'bad_state', "hello first, auth once"))
            return

        name = data.get('name')
        if not isinstance(name, str) or name == "" or len(name) > 32:
            session.send(protocol.error(seq, 'bad_request', "bad name"))
            return

        password = data.get('password')
        token = data.get('token')

        if isinstance(password, str):
            player = self.auth.login_password(name, password)
        elif isinstance(token, str):
            player = self.auth.login_token(name, token)
        else:
            session.send(protocol.error(seq, 'bad_request', "need password or token"))
            return

        if player is None:
            session.send(protocol.error(seq, 'bad_credentials', "wrong password or stale token"))
            return

        old = self.sessions.get(name)
        if old is not None and old.is_npc:
            session.send(protocol.error(seq, 'bad_request', "that name belongs to an npc"))
            return
        wanted = None
        if old is not None:
            wanted = (old.x, old.y)    # same player continues where it was
            session.hp = old.hp
            session.stamina = old.stamina
            kick_data = {}
            kick_data['reason'] = 'logged_in_elsewhere'
            old.send(protocol.event('kicked', kick_data))
            old.player = None    # its cleanup must not remove the new session
            old.kill()
        else:
            if player['hp'] is not None: session.hp = player['hp']
            if player['stamina'] is not None: session.stamina = player['stamina']

        if wanted is None and player['x'] is not None:
            wanted = (player['x'], player['y'])
        if wanted is None:
            wanted = self.world.spawn

        spot = self.world.find_free_near(wanted[0], wanted[1], self.occupied_set(exclude=name))
        if spot is None:
            session.send(protocol.error(seq, 'bad_state', "world is full"))
            session.kill()
            return

        session.player = player
        session.state = AUTHED
        session.x, session.y = spot
        self.sessions[name] = session
        self.mark_moved(session)

        joined_data = {}
        joined_data['name'] = name
        self.broadcast(protocol.event('player_joined', joined_data), exclude=name)

        welcome_data = {}
        welcome_data['player_id'] = player['id']
        welcome_data['token'] = player['token']
        welcome_data['settings'] = json.loads(player['settings'])
        welcome_data['online'] = self.online_names()
        welcome_data['rules'] = RULES
        session.send(protocol.reply('welcome', seq, welcome_data))

        map_data = {}
        map_data['x'] = 0
        map_data['y'] = 0
        map_data['w'] = self.world.width
        map_data['h'] = self.world.height
        map_data['tiles'] = self.world.flat_tiles()
        session.send(protocol.event('map', map_data))

        snapshot = []
        for other_name in self.sessions:
            other = self.sessions[other_name]
            if other.dead: continue
            entry = {}
            entry['name'] = other_name
            entry['x'] = other.x
            entry['y'] = other.y
            entry['hp'] = round(other.hp, 1)
            entry['hp_max'] = other.hp_max
            snapshot.append(entry)
        state_data = {}
        state_data['full'] = True
        state_data['players'] = snapshot
        session.send(protocol.event('state', state_data))
        print(f"[+] {name} logged in at {spot}", flush=True)

    async def on_ping(self, session, seq, data):
        if session.state == NEW:
            session.send(protocol.error(seq, 'bad_state', "hello first"))
            return

        pong_data = {}
        pong_data['t'] = data.get('t')
        session.send(protocol.reply('pong', seq, pong_data))

    async def on_get_settings(self, session, seq, data):
        if session.state != AUTHED:
            session.send(protocol.error(seq, 'bad_state', "auth first"))
            return

        player = self.db.get_player_by_name(session.name)
        reply_data = {}
        reply_data['settings'] = json.loads(player['settings'])
        session.send(protocol.reply('settings', seq, reply_data))

    async def on_set_settings(self, session, seq, data):
        if session.state != AUTHED:
            session.send(protocol.error(seq, 'bad_state', "auth first"))
            return

        settings = data.get('settings')
        if not isinstance(settings, dict):
            session.send(protocol.error(seq, 'bad_request', "settings must be an object"))
            return

        settings_json = json.dumps(settings)
        if len(settings_json) > protocol.MAX_SETTINGS_BYTES:
            session.send(protocol.error(seq, 'too_large', "settings over 16KiB"))
            return

        self.db.set_settings(session.player['id'], settings_json)
        session.send(protocol.reply('settings_ok', seq, {}))

    def intent_reply(self, session, seq, granted):
        reply_data = session.vitals()
        reply_data['granted'] = granted
        reply_data['x'] = session.x
        reply_data['y'] = session.y
        session.send(protocol.reply('intent_ok', seq, reply_data))

    def do_move(self, session, seq, motion, count):
        tiles = count
        if motion in DASH_MOTIONS: tiles = count * RULES['dash_mult']

        affordable = int(session.stamina // RULES['move_cost'])
        if tiles > affordable: tiles = affordable
        if tiles <= 0:
            self.intent_reply(session, seq, 0)
            return

        occupied = self.occupied_set(exclude=session.name)
        granted, x, y = self.world.resolve_move(session.x, session.y, motion, tiles, occupied)
        if granted > 0:
            session.x = x
            session.y = y
            session.stamina -= granted * RULES['move_cost']
            self.mark_moved(session)
        self.intent_reply(session, seq, granted)

    def respawn(self, session):
        session.hp = session.hp_max
        session.stamina = RULES['stamina_max']
        home = session.home
        if home is None: home = self.world.spawn
        spot = self.world.find_free_near(home[0], home[1],
                                         self.occupied_set(exclude=session.name))
        if spot is not None:
            session.x, session.y = spot
        self.mark_hurt(session)

        vitals_data = session.vitals()
        vitals_data['x'] = session.x
        vitals_data['y'] = session.y
        session.send(protocol.event('vitals', vitals_data))

    def npc_die(self, npc):
        # vanish from the world; the ticker revives it after a delay
        npc.dead = True
        npc.respawn_at = time.time() + NPC_DEATH_DELAY
        self.dirty.pop(npc.name, None)
        self.gone.append(npc.name)

    def revive_npcs(self):
        now = time.time()
        for name in self.sessions:
            npc = self.sessions[name]
            if not npc.is_npc: continue
            if not npc.dead: continue
            if now < npc.respawn_at: continue

            spot = self.world.find_free_near(npc.home[0], npc.home[1], self.occupied_set())
            if spot is None: continue    # stay dead until its home clears
            npc.dead = False
            npc.hp = npc.hp_max
            npc.x, npc.y = spot
            self.mark_hurt(npc)

    def do_melee(self, session, seq, motion, count):
        affordable = int(session.stamina // RULES['melee_cost'])
        pool = min(count, affordable)
        if pool <= 0:
            self.intent_reply(session, seq, 0)
            return

        session.stamina -= pool * RULES['melee_cost']    # the swing is committed, hit or whiff

        swing_data = {}
        swing_data['by'] = session.name
        swing_data['motion'] = motion
        swing_data['count'] = pool
        swing_data['x'] = session.x
        swing_data['y'] = session.y
        self.broadcast(protocol.event('melee', swing_data), exclude=session.name)

        occupied = self.occupied_sessions(exclude=session.name)
        targets = self.world.ray_targets(session.x, session.y, motion, pool, occupied)

        consumed = float(pool)
        prev_distance = 0
        for other, distance in targets:
            consumed -= max(distance - prev_distance - 1, 0)    # travel beyond adjacent costs pool
            prev_distance = distance
            if consumed <= 0: break

            damage = min(consumed, other.hp)
            consumed -= damage
            other.hp -= damage

            if other.hp <= 0:
                died_data = {}
                died_data['name'] = other.name
                died_data['by'] = session.name
                self.broadcast(protocol.event('died', died_data))
                print(f"[+] {other.name} was killed by {session.name}", flush=True)
                if other.is_npc: self.npc_die(other)
                else: self.respawn(other)
            else:
                self.mark_hurt(other)
                other.send(protocol.event('vitals', other.vitals()))

            if consumed <= 0: break

        self.intent_reply(session, seq, pool)

    async def on_intent(self, session, seq, data):
        if session.state != AUTHED:
            session.send(protocol.error(seq, 'bad_state', "auth first"))
            return

        op = data.get('op')
        if op != 'move' and op != 'melee':
            session.send(protocol.error(seq, 'bad_request', f"unknown op: {op}"))
            return

        motion = data.get('motion')
        allowed = MOVE_DIRS if op == 'move' else MOTIONS
        if motion not in allowed:
            session.send(protocol.error(seq, 'bad_request', f"bad motion for {op}: {motion}"))
            return

        count = data.get('count', 1)
        if isinstance(count, bool) or not isinstance(count, int) or count < 1 or count > MAX_COUNT:
            session.send(protocol.error(seq, 'bad_request', "count must be 1..4096"))
            return

        if op == 'move':
            self.do_move(session, seq, motion, count)
        else:
            self.do_melee(session, seq, motion, count)

    async def dispatch(self, session, raw):
        try:
            msg_type, seq, data = protocol.parse(raw)
        except protocol.BadFrame as e:
            session.send(protocol.error(0, 'bad_json', str(e)))
            session.kill()
            return

        handler = self.handlers.get(msg_type)
        if handler is None:
            session.send(protocol.error(seq, 'unknown_type', f"no such type: {msg_type}"))
            return

        await handler(session, seq, data)

    def cleanup(self, session):
        name = session.name
        if name is None: return
        if self.sessions.get(name) is not session: return

        del self.sessions[name]
        self.db.set_state(session.player['id'], session.x, session.y,
                          round(session.hp, 1), round(session.stamina, 1))
        self.db.touch_last_seen(session.player['id'])
        self.dirty.pop(name, None)
        self.gone.append(name)

        left_data = {}
        left_data['name'] = name
        self.broadcast(protocol.event('player_left', left_data))
        print(f"[+] {name} disconnected", flush=True)

    async def handle_connection(self, websocket):
        session = Session(websocket)
        session.writer_task = asyncio.get_running_loop().create_task(session.writer())

        try:
            async for raw in websocket:
                session.last_recv = time.time()
                await self.dispatch(session, raw)
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            session.writer_task.cancel()
            self.cleanup(session)

    def regen(self):
        for name in self.sessions:
            session = self.sessions[name]
            if session.is_npc: continue    # combat targets do not heal
            if session.hp < session.hp_max:
                session.hp = min(session.hp_max, session.hp + RULES['hp_regen'] * TICK_INTERVAL)
            if session.stamina < RULES['stamina_max']:
                session.stamina = min(RULES['stamina_max'],
                                      session.stamina + RULES['stamina_regen'] * TICK_INTERVAL)

    async def ticker(self):
        while True:
            await asyncio.sleep(TICK_INTERVAL)
            self.regen()
            self.revive_npcs()
            self.act_npcs()
            if len(self.dirty) == 0 and len(self.gone) == 0: continue

            dirty = self.dirty
            gone = self.gone
            self.dirty = {}
            self.gone = []

            for name in self.sessions:
                session = self.sessions[name]
                if session.is_npc: continue
                if session.state != AUTHED: continue

                players = []
                for moved_name in dirty:
                    if moved_name == name: continue    # own moves arrive via intent_ok
                    entry = {}
                    entry['name'] = moved_name
                    entry.update(dirty[moved_name])
                    players.append(entry)

                if len(players) == 0 and len(gone) == 0: continue

                state_data = {}
                if len(players) > 0: state_data['players'] = players
                if len(gone) > 0: state_data['gone'] = gone
                session.send(protocol.event('state', state_data))

    async def reaper(self):
        while True:
            await asyncio.sleep(REAPER_INTERVAL)
            now = time.time()
            stale = []
            for name in self.sessions:
                session = self.sessions[name]
                if session.is_npc: continue
                if now - session.last_recv > SILENCE_TIMEOUT: stale.append(session)
            for session in stale:
                print(f"[!] reaping silent connection: {session.name}", flush=True)
                session.kill()

    async def serve(self):
        reaper_task = asyncio.get_running_loop().create_task(self.reaper())
        ticker_task = asyncio.get_running_loop().create_task(self.ticker())
        async with websockets.serve(self.handle_connection, self.host, self.port,
                                    max_size=protocol.MAX_FRAME_BYTES):
            print(f"[+] serving on ws://{self.host}:{self.port}", flush=True)
            try:
                await asyncio.get_running_loop().create_future()
            finally:
                reaper_task.cancel()
                ticker_task.cancel()
