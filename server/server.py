import websockets
import asyncio
import json
import time

from . import protocol
from .auth import Auth
from .db import Db
from .world import World
from .world import MOTIONS


NEW = 'new'
GREETED = 'greeted'
AUTHED = 'authed'

OUTBOUND_QUEUE_SIZE = 256
SILENCE_TIMEOUT = 30.0
REAPER_INTERVAL = 5.0
TICK_INTERVAL = 1.0 / 20
MAX_COUNT = 4096


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


class Server():
    def __init__(self, host, port, db_path, map_path, motd="welcome to gridmmo"):
        self.host = host
        self.port = port
        self.motd = motd
        self.db = Db(db_path)
        self.auth = Auth(self.db)
        self.world = World(map_path)
        self.sessions = {}
        self.dirty = {}
        self.gone = []

        self.handlers = {}
        self.handlers['hello'] = self.on_hello
        self.handlers['auth'] = self.on_auth
        self.handlers['ping'] = self.on_ping
        self.handlers['get_settings'] = self.on_get_settings
        self.handlers['set_settings'] = self.on_set_settings
        self.handlers['intent'] = self.on_intent

    def occupied_set(self, exclude=None):
        occupied = set()
        for name in self.sessions:
            if name == exclude: continue
            session = self.sessions[name]
            occupied.add((session.x, session.y))
        return occupied

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
        wanted = None
        if old is not None:
            wanted = (old.x, old.y)    # same player continues where it was
            kick_data = {}
            kick_data['reason'] = 'logged_in_elsewhere'
            old.send(protocol.event('kicked', kick_data))
            old.player = None    # its cleanup must not remove the new session
            old.kill()

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
        self.dirty[name] = spot

        joined_data = {}
        joined_data['name'] = name
        self.broadcast(protocol.event('player_joined', joined_data), exclude=name)

        welcome_data = {}
        welcome_data['player_id'] = player['id']
        welcome_data['token'] = player['token']
        welcome_data['settings'] = json.loads(player['settings'])
        welcome_data['online'] = self.online_names()
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
            entry = {}
            entry['name'] = other_name
            entry['x'] = other.x
            entry['y'] = other.y
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

    async def on_intent(self, session, seq, data):
        if session.state != AUTHED:
            session.send(protocol.error(seq, 'bad_state', "auth first"))
            return

        op = data.get('op')
        if op != 'move':
            session.send(protocol.error(seq, 'bad_request', f"unknown op: {op}"))
            return

        motion = data.get('motion')
        if motion not in MOTIONS:
            session.send(protocol.error(seq, 'bad_request', f"unknown motion: {motion}"))
            return

        count = data.get('count', 1)
        if isinstance(count, bool) or not isinstance(count, int) or count < 1 or count > MAX_COUNT:
            session.send(protocol.error(seq, 'bad_request', "count must be 1..4096"))
            return

        occupied = self.occupied_set(exclude=session.name)
        granted, x, y = self.world.resolve_move(session.x, session.y, motion, count, occupied)
        if granted > 0:
            session.x = x
            session.y = y
            self.dirty[session.name] = (x, y)

        reply_data = {}
        reply_data['granted'] = granted
        reply_data['x'] = x
        reply_data['y'] = y
        session.send(protocol.reply('intent_ok', seq, reply_data))

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
        self.db.set_position(session.player['id'], session.x, session.y)
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

    async def ticker(self):
        while True:
            await asyncio.sleep(TICK_INTERVAL)
            if len(self.dirty) == 0 and len(self.gone) == 0: continue

            dirty = self.dirty
            gone = self.gone
            self.dirty = {}
            self.gone = []

            for name in self.sessions:
                session = self.sessions[name]
                if session.state != AUTHED: continue

                players = []
                for moved_name in dirty:
                    if moved_name == name: continue    # own moves arrive via intent_ok
                    entry = {}
                    entry['name'] = moved_name
                    entry['x'] = dirty[moved_name][0]
                    entry['y'] = dirty[moved_name][1]
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
