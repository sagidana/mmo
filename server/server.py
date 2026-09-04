import websockets
import asyncio
import random
import json
import time

from . import protocol
from .auth import Auth
from .db import Db
from .world import load_world
from .world import MOTIONS
from .world import MOVE_DIRS
from .world import DASH_MOTIONS
from .challenge import Challenge


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
RULES['defend_drain'] = 6.0
RULES['melee_windup'] = 0.03
RULES['windup_max'] = 1.0
RULES['melee_recover'] = 0.02
RULES['recover_max'] = 0.6

SPELLS = []
FIREBOLT = {}
FIREBOLT['name'] = 'firebolt'
FIREBOLT['cost'] = 3
FIREBOLT['speed'] = 9.0
FIREBOLT['range'] = 15
FIREBOLT['windup'] = 0.04
FIREBOLT['recover'] = 0.02
SPELLS.append(FIREBOLT)

SPELL_INDEX = {}
for _spell in SPELLS:
    SPELL_INDEX[_spell['name']] = _spell

OPPOSITE = {}
OPPOSITE['l'] = 'h'
OPPOSITE['h'] = 'l'
OPPOSITE['j'] = 'k'
OPPOSITE['k'] = 'j'
CHASER_WINDUP = 0.45

NPC_FIGURES = {}
NPC_FIGURES['dummy'] = 'slime'
NPC_FIGURES['walker'] = 'rat'
NPC_FIGURES['chaser'] = 'hound'
NPC_FIGURES['shooter'] = 'watcher'
NPC_FIGURES['mage'] = 'wisp'
MAGE_WINDUP = 0.6


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
        self.defending = False
        self.home = None
        self.instance = None
        self.figure = 'human'
        self.facing = 'j'
        self.pending = None
        self.recover_until = 0.0
        self.active_spell = SPELLS[0]['name']

    @property
    def name(self):
        if self.player is None: return None
        return self.player['name']

    def vitals(self):
        data = {}
        data['hp'] = round(self.hp, 1)
        data['stamina'] = round(self.stamina, 1)
        return data

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
    def __init__(self, name, x, y, hp, kind='dummy', damage=5.0):
        self.name = name
        self.x = x
        self.y = y
        self.hp = hp
        self.hp_max = hp
        self.stamina = RULES['stamina_max']
        self.state = AUTHED
        self.is_npc = True
        self.dead = False
        self.defending = False
        self.respawn_at = 0.0
        self.home = (x, y)
        self.kind = kind
        self.damage = damage
        self.figure = NPC_FIGURES.get(kind, 'slime')
        self.facing = 'j'
        self.pending = None
        self.recover_until = 0.0
        self.fire_dir = 'h'
        self.every = 2.5
        self.next_act = 0.0
        self.instance = None

    def send(self, raw):
        pass

    def kill(self):
        pass

    def vitals(self):
        data = {}
        data['hp'] = round(self.hp, 1)
        data['stamina'] = round(self.stamina, 1)
        return data


class Instance():
    # the unit of world scoping: its own occupancy, npcs, deltas, objective.
    # the open world is one shared instance; challenge mode makes one per player.
    def __init__(self, name, world, challenge=None):
        self.name = name
        self.world = world
        self.challenge = challenge
        self.members = {}
        self.dirty = {}
        self.gone = []
        self.bolts = []
        self.status = 'running'
        self.start_time = time.time()

    def elapsed(self):
        return time.time() - self.start_time

    def players(self):
        found = []
        for name in self.members:
            member = self.members[name]
            if member.is_npc: continue
            found.append(member)
        return found

    def alive_npcs(self):
        found = []
        for name in self.members:
            member = self.members[name]
            if not member.is_npc: continue
            if member.dead: continue
            found.append(member)
        return found


class Server():
    def __init__(self, host, port, db_path, map_path, motd="welcome to gridmmo",
                 npcs=True, challenge_path=None):
        self.host = host
        self.port = port
        self.motd = motd
        self.db = Db(db_path)
        self.auth = Auth(self.db)
        self.sessions = {}
        self.instances = {}

        self.challenge = None
        if challenge_path is not None:
            self.challenge = Challenge(challenge_path)
            RULES.update(self.challenge.rules)
        else:
            world = load_world(map_path)
            main = Instance('world', world)
            self.instances['world'] = main
            if npcs: self.spawn_world_npcs(main)

        self.handlers = {}
        self.handlers['hello'] = self.on_hello
        self.handlers['auth'] = self.on_auth
        self.handlers['ping'] = self.on_ping
        self.handlers['get_settings'] = self.on_get_settings
        self.handlers['set_settings'] = self.on_set_settings
        self.handlers['intent'] = self.on_intent
        self.handlers['retry'] = self.on_retry
        self.handlers['spell'] = self.on_spell

    # ---------------- npcs ----------------

    def add_npc(self, instance, npc):
        npc.instance = instance
        instance.members[npc.name] = npc

    def spawn_world_npcs(self, instance):
        # training targets near spawn: a row of dummies and a wanderer
        sx, sy = instance.world.spawn
        offsets = (3, 5, 7)
        i = 1
        for offset in offsets:
            spot = instance.world.find_free_near(sx + offset, sy, self.occupied_set(instance))
            if spot is None: continue
            self.add_npc(instance, Npc(f"dummy_{i}", spot[0], spot[1], hp=30.0))
            i += 1

        spot = instance.world.find_free_near(sx, sy + 5, self.occupied_set(instance))
        if spot is not None:
            self.add_npc(instance, Npc("walker", spot[0], spot[1], hp=60.0, kind='walker'))

    def spawn_challenge_npcs(self, instance):
        for spec in self.challenge.npcs:
            npc = Npc(spec['name'], spec['x'], spec['y'], hp=spec['hp'],
                      kind=spec['kind'], damage=spec['damage'])
            npc.fire_dir = spec['dir']
            npc.every = spec['every']
            if spec['figure'] is not None: npc.figure = spec['figure']
            if npc.kind == 'shooter': npc.next_act = time.time() + npc.every
            self.add_npc(instance, npc)

    def npc_names(self):
        names = set()
        if self.challenge is not None:
            names.update(self.challenge.npc_names)
            return names
        main = self.instances.get('world')
        if main is None: return names
        for name in main.members:
            if main.members[name].is_npc: names.add(name)
        return names

    # ---------------- scoping helpers ----------------

    def occupied_set(self, instance, exclude=None):
        occupied = set()
        for name in instance.members:
            if name == exclude: continue
            member = instance.members[name]
            if member.dead: continue
            occupied.add((member.x, member.y))
        return occupied

    def occupied_members(self, instance, exclude=None):
        occupied = {}
        for name in instance.members:
            if name == exclude: continue
            member = instance.members[name]
            if member.dead: continue
            occupied[(member.x, member.y)] = member
        return occupied

    def mark_moved(self, member):
        instance = member.instance
        entry = instance.dirty.get(member.name)
        if entry is None:
            entry = {}
            instance.dirty[member.name] = entry
        entry['x'] = member.x
        entry['y'] = member.y
        if member.is_npc: entry['figure'] = member.figure

    def mark_hurt(self, member):
        self.mark_moved(member)
        instance = member.instance
        instance.dirty[member.name]['hp'] = round(member.hp, 1)
        instance.dirty[member.name]['hp_max'] = member.hp_max

    def online_names(self):
        names = []
        for name in self.sessions:
            names.append(name)
        return names

    def broadcast(self, raw, exclude=None):
        # global, players only: presence and kicks
        for name in self.sessions:
            if name == exclude: continue
            self.sessions[name].send(raw)

    def instance_send(self, instance, raw, exclude=None):
        for member in instance.players():
            if member.name == exclude: continue
            member.send(raw)

    def challenge_event(self, instance, status):
        data = {}
        data['status'] = status
        data['objective'] = self.challenge.objective_text
        data['time'] = round(instance.elapsed(), 1)
        self.instance_send(instance, protocol.event('challenge', data))

    # ---------------- handshake ----------------

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

    def enter_instance(self, session, player):
        name = player['name']

        if self.challenge is not None:
            instance = Instance(name, self.challenge.world, self.challenge)
            self.instances[name] = instance
            self.spawn_challenge_npcs(instance)
            session.hp = self.challenge.player.get('hp', RULES['hp_max'])
            session.stamina = self.challenge.player.get('stamina', RULES['stamina_max'])
            wanted = instance.world.spawn
        else:
            instance = self.instances['world']
            wanted = None
            if player['x'] is not None: wanted = (player['x'], player['y'])
            if wanted is None: wanted = instance.world.spawn
            if player['hp'] is not None: session.hp = player['hp']
            if player['stamina'] is not None: session.stamina = player['stamina']

        spot = instance.world.find_free_near(wanted[0], wanted[1],
                                             self.occupied_set(instance, exclude=name))
        if spot is None: return None

        session.x, session.y = spot
        session.instance = instance
        instance.members[name] = session
        self.mark_moved(session)
        return instance

    async def on_auth(self, session, seq, data):
        if session.state != GREETED:
            session.send(protocol.error(seq, 'bad_state', "hello first, auth once"))
            return

        name = data.get('name')
        if not isinstance(name, str) or name == "" or len(name) > 32:
            session.send(protocol.error(seq, 'bad_request', "bad name"))
            return
        if name in self.npc_names():
            session.send(protocol.error(seq, 'bad_request', "that name belongs to an npc"))
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
        if old is not None:
            if self.challenge is None and old.instance is not None:
                player = dict(player)
                player['x'] = old.x    # same player continues where it was
                player['y'] = old.y
                player['hp'] = old.hp
                player['stamina'] = old.stamina
            kick_data = {}
            kick_data['reason'] = 'logged_in_elsewhere'
            old.send(protocol.event('kicked', kick_data))
            self.leave_instance(old)
            old.player = None    # its cleanup must not remove the new session
            old.kill()
            del self.sessions[name]

        session.player = player
        session.state = AUTHED
        instance = self.enter_instance(session, player)
        if instance is None:
            session.player = None
            session.send(protocol.error(seq, 'bad_state', "world is full"))
            session.kill()
            return
        self.sessions[name] = session

        joined_data = {}
        joined_data['name'] = name
        self.broadcast(protocol.event('player_joined', joined_data), exclude=name)

        welcome_data = {}
        welcome_data['player_id'] = player['id']
        welcome_data['token'] = player['token']
        welcome_data['settings'] = json.loads(player['settings'])
        welcome_data['online'] = self.online_names()
        welcome_data['rules'] = RULES
        welcome_data['spells'] = SPELLS
        welcome_data['active_spell'] = session.active_spell
        session.send(protocol.reply('welcome', seq, welcome_data))

        map_data = {}
        map_data['x'] = 0
        map_data['y'] = 0
        map_data['w'] = instance.world.width
        map_data['h'] = instance.world.height
        map_data['tiles'] = instance.world.flat_tiles()
        session.send(protocol.event('map', map_data))

        session.send(protocol.event('state', self.snapshot(instance)))

        if self.challenge is not None:
            self.challenge_event(instance, 'start')
        print(f"[+] {name} logged in at ({session.x}, {session.y})", flush=True)

    def snapshot(self, instance):
        entries = []
        for member_name in instance.members:
            member = instance.members[member_name]
            if member.dead: continue
            entry = {}
            entry['name'] = member_name
            entry['x'] = member.x
            entry['y'] = member.y
            entry['hp'] = round(member.hp, 1)
            entry['hp_max'] = member.hp_max
            entry['figure'] = member.figure
            if member.defending: entry['def'] = 1
            entries.append(entry)
        state_data = {}
        state_data['full'] = True
        state_data['players'] = entries
        return state_data

    # ---------------- plumbing ----------------

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

    # ---------------- combat + movement ----------------

    def busy(self, member):
        if member.pending is not None: return True
        if time.time() < member.recover_until: return True
        return False

    def intent_reply(self, session, seq, granted):
        reply_data = session.vitals()
        reply_data['granted'] = granted
        reply_data['x'] = session.x
        reply_data['y'] = session.y
        session.send(protocol.reply('intent_ok', seq, reply_data))

    def set_defending(self, member, on):
        if member.defending == on: return
        member.defending = on
        defend_data = {}
        defend_data['name'] = member.name
        defend_data['on'] = on
        self.instance_send(member.instance, protocol.event('defend', defend_data))

    def do_defend(self, session, seq, motion):
        if session.instance.status != 'running' or self.busy(session):
            self.intent_reply(session, seq, 0)
            return
        if motion == 'on':
            self.set_defending(session, True)
        else:
            self.set_defending(session, False)
        self.intent_reply(session, seq, 1)

    def do_face(self, session, seq, motion):
        # free and allowed while guarding: this is how you steer the shield
        if session.instance.status != 'running' or self.busy(session):
            self.intent_reply(session, seq, 0)
            return

        session.facing = motion
        face_data = {}
        face_data['name'] = session.name
        face_data['motion'] = motion
        self.instance_send(session.instance, protocol.event('face', face_data),
                           exclude=session.name)
        self.intent_reply(session, seq, 1)

    def do_move(self, session, seq, motion, count):
        if session.instance.status != 'running' or session.defending or self.busy(session):
            self.intent_reply(session, seq, 0)
            return

        tiles = count
        if motion in DASH_MOTIONS: tiles = count * RULES['dash_mult']

        affordable = int(session.stamina // RULES['move_cost'])
        if tiles > affordable: tiles = affordable
        if tiles <= 0:
            self.intent_reply(session, seq, 0)
            return

        instance = session.instance
        occupied = self.occupied_set(instance, exclude=session.name)
        granted, x, y = instance.world.resolve_move(session.x, session.y, motion, tiles, occupied)
        if granted > 0:
            session.x = x
            session.y = y
            session.facing = motion.lower()
            session.stamina -= granted * RULES['move_cost']
            self.mark_moved(session)
        self.intent_reply(session, seq, granted)

    def respawn(self, member):
        member.hp = member.hp_max
        member.stamina = RULES['stamina_max']
        member.pending = None
        member.recover_until = 0.0
        home = member.home
        if home is None: home = member.instance.world.spawn
        spot = member.instance.world.find_free_near(home[0], home[1],
                                                    self.occupied_set(member.instance,
                                                                      exclude=member.name))
        if spot is not None:
            member.x, member.y = spot
        self.mark_hurt(member)

        vitals_data = member.vitals()
        vitals_data['x'] = member.x
        vitals_data['y'] = member.y
        member.send(protocol.event('vitals', vitals_data))

    def npc_die(self, npc):
        instance = npc.instance
        npc.dead = True
        npc.respawn_at = time.time() + NPC_DEATH_DELAY
        instance.dirty.pop(npc.name, None)
        instance.gone.append(npc.name)

    def kill_member(self, victim, by_name):
        instance = victim.instance
        died_data = {}
        died_data['name'] = victim.name
        died_data['by'] = by_name
        self.instance_send(instance, protocol.event('died', died_data))
        print(f"[+] {victim.name} was killed by {by_name}", flush=True)

        if victim.is_npc:
            self.npc_die(victim)
            return
        if instance.challenge is not None:
            self.end_challenge(instance, 'lost')
            victim.send(protocol.event('vitals', victim.vitals()))
            return
        self.respawn(victim)

    def apply_damage(self, victim, damage, by_name):
        victim.hp -= damage
        if victim.hp <= 0:
            self.kill_member(victim, by_name)
        else:
            self.mark_hurt(victim)
            victim.send(protocol.event('vitals', victim.vitals()))

    def do_melee(self, session, seq, motion, count):
        if session.instance.status != 'running' or session.defending or self.busy(session):
            self.intent_reply(session, seq, 0)
            return

        affordable = int(session.stamina // RULES['melee_cost'])
        pool = min(count, affordable)
        if pool <= 0:
            self.intent_reply(session, seq, 0)
            return

        session.stamina -= pool * RULES['melee_cost']    # the swing is committed, hit or whiff
        session.facing = motion

        windup = min(pool * RULES['melee_windup'], RULES['windup_max'])
        pending = {}
        pending['kind'] = 'melee'
        pending['motion'] = motion
        pending['pool'] = pool
        pending['at'] = time.time() + windup
        session.pending = pending

        wind_data = {}
        wind_data['by'] = session.name
        wind_data['motion'] = motion
        wind_data['count'] = pool
        wind_data['x'] = session.x
        wind_data['y'] = session.y
        wind_data['strike_in'] = round(windup, 2)
        self.instance_send(session.instance, protocol.event('windup', wind_data))

        self.intent_reply(session, seq, pool)

    def resolve_melee(self, member, motion, pool):
        instance = member.instance

        swing_data = {}
        swing_data['by'] = member.name
        swing_data['motion'] = motion
        swing_data['count'] = pool
        swing_data['x'] = member.x
        swing_data['y'] = member.y
        self.instance_send(instance, protocol.event('melee', swing_data))

        occupied = self.occupied_members(instance, exclude=member.name)
        targets = instance.world.ray_targets(member.x, member.y, motion, pool, occupied)

        consumed = float(pool)
        prev_distance = 0
        for other, distance in targets:
            consumed -= max(distance - prev_distance - 1, 0)    # travel beyond adjacent costs pool
            prev_distance = distance
            if consumed <= 0: break

            blocks = other.defending
            if blocks and other.facing != OPPOSITE[motion]: blocks = False    # flanked

            if blocks:
                # a facing shield soaks the whole remaining pool: blocked points
                # cost 1 stamina each, the uncovered rest hits hp and breaks it
                blocked = min(consumed, other.stamina)
                other.stamina -= blocked
                damage = consumed - blocked
                consumed = 0
                if damage > 0 or other.stamina <= 0: self.set_defending(other, False)
                if damage > 0: self.apply_damage(other, damage, member.name)
                else: other.send(protocol.event('vitals', other.vitals()))
            else:
                damage = min(consumed, other.hp)
                consumed -= damage
                self.apply_damage(other, damage, member.name)

            if consumed <= 0: break

    def resolve_pending(self, instance):
        now = time.time()
        for name in list(instance.members):
            member = instance.members.get(name)
            if member is None: continue
            if member.pending is None: continue
            if instance.status != 'running' or member.dead:
                member.pending = None
                continue
            if now < member.pending['at']: continue

            pending = member.pending
            member.pending = None

            if pending['kind'] == 'melee':
                recover = min(pending['pool'] * RULES['melee_recover'], RULES['recover_max'])
                member.recover_until = now + recover
                self.resolve_melee(member, pending['motion'], pending['pool'])
            elif pending['kind'] == 'magic':
                spell = SPELL_INDEX[pending['spell']]
                recover = min(pending['pool'] * spell['recover'], RULES['recover_max'])
                member.recover_until = now + recover
                self.launch_bolt(member, pending['motion'], pending['pool'], pending['spell'])
            elif pending['kind'] == 'bite':
                dx, dy = MOTIONS[pending['motion']]
                occupied = self.occupied_members(instance, exclude=member.name)
                target = occupied.get((member.x + dx, member.y + dy))

                swing_data = {}
                swing_data['by'] = member.name
                swing_data['motion'] = pending['motion']
                swing_data['count'] = int(member.damage)
                swing_data['x'] = member.x
                swing_data['y'] = member.y
                self.instance_send(instance, protocol.event('melee', swing_data))

                if target is not None and not target.is_npc:
                    self.strike(target, member.damage, member.name, ray_dir=pending['motion'])

    async def on_intent(self, session, seq, data):
        if session.state != AUTHED:
            session.send(protocol.error(seq, 'bad_state', "auth first"))
            return

        op = data.get('op')
        ops = ('move', 'melee', 'defend', 'magic', 'face')
        if op not in ops:
            session.send(protocol.error(seq, 'bad_request', f"unknown op: {op}"))
            return

        motion = data.get('motion')
        if op == 'defend':
            if motion != 'on' and motion != 'off':
                session.send(protocol.error(seq, 'bad_request', "defend motion must be on/off"))
                return
            self.do_defend(session, seq, motion)
            return

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
        elif op == 'magic':
            self.do_magic(session, seq, motion, count)
        elif op == 'face':
            self.do_face(session, seq, motion)
        else:
            self.do_melee(session, seq, motion, count)

    # ---------------- challenge lifecycle ----------------

    def end_challenge(self, instance, status):
        instance.status = status
        self.challenge_event(instance, status)
        print(f"[+] challenge {status} for {instance.name} at {round(instance.elapsed(), 1)}s",
              flush=True)

    def check_objective(self, instance):
        if instance.challenge is None: return
        if instance.status != 'running': return

        objective = instance.challenge.objective
        if objective[0] == 'kill_all':
            if len(instance.alive_npcs()) == 0: self.end_challenge(instance, 'won')
        elif objective[0] == 'survive':
            if instance.elapsed() >= objective[1]: self.end_challenge(instance, 'won')
        elif objective[0] == 'reach':
            for member in instance.players():
                if member.x == objective[1] and member.y == objective[2]:
                    self.end_challenge(instance, 'won')
                    break

    def reset_instance(self, instance):
        stale = []
        for name in instance.members:
            if instance.members[name].is_npc: stale.append(name)
        for name in stale:
            del instance.members[name]
        self.spawn_challenge_npcs(instance)

        instance.dirty = {}
        instance.gone = []
        instance.bolts = []
        instance.status = 'running'
        instance.start_time = time.time()

        for member in instance.players():
            member.hp = self.challenge.player.get('hp', RULES['hp_max'])
            member.stamina = self.challenge.player.get('stamina', RULES['stamina_max'])
            member.defending = False
            member.dead = False
            member.pending = None
            member.recover_until = 0.0
            spot = instance.world.find_free_near(instance.world.spawn[0],
                                                 instance.world.spawn[1],
                                                 self.occupied_set(instance, exclude=member.name))
            if spot is not None:
                member.x, member.y = spot

            vitals_data = member.vitals()
            vitals_data['x'] = member.x
            vitals_data['y'] = member.y
            member.send(protocol.event('vitals', vitals_data))
            member.send(protocol.event('state', self.snapshot(instance)))
        self.challenge_event(instance, 'start')

    async def on_spell(self, session, seq, data):
        if session.state != AUTHED:
            session.send(protocol.error(seq, 'bad_state', "auth first"))
            return

        name = data.get('spell')
        if name not in SPELL_INDEX:
            session.send(protocol.error(seq, 'bad_request', f"unknown spell: {name}"))
            return

        session.active_spell = name
        reply_data = {}
        reply_data['spell'] = name
        session.send(protocol.reply('spell_ok', seq, reply_data))

    def do_magic(self, session, seq, motion, count):
        if session.instance.status != 'running' or session.defending or self.busy(session):
            self.intent_reply(session, seq, 0)
            return

        spell = SPELL_INDEX[session.active_spell]
        affordable = int(session.stamina // spell['cost'])
        pool = min(count, affordable)
        if pool <= 0:
            self.intent_reply(session, seq, 0)
            return

        session.stamina -= pool * spell['cost']
        session.facing = motion

        windup = min(pool * spell['windup'], RULES['windup_max'])
        pending = {}
        pending['kind'] = 'magic'
        pending['motion'] = motion
        pending['pool'] = pool
        pending['spell'] = spell['name']
        pending['at'] = time.time() + windup
        session.pending = pending

        wind_data = {}
        wind_data['by'] = session.name
        wind_data['motion'] = motion
        wind_data['count'] = spell['range']    # the aim line, not the pool
        wind_data['x'] = session.x
        wind_data['y'] = session.y
        wind_data['strike_in'] = round(windup, 2)
        self.instance_send(session.instance, protocol.event('windup', wind_data))

        self.intent_reply(session, seq, pool)

    def launch_bolt(self, member, motion, pool, spell_name):
        spell = SPELL_INDEX[spell_name]
        dx, dy = MOTIONS[motion]

        bolt = {}
        bolt['by'] = member.name
        bolt['motion'] = motion
        bolt['x'] = member.x
        bolt['y'] = member.y
        bolt['dx'] = dx
        bolt['dy'] = dy
        bolt['speed'] = spell['speed']
        bolt['range'] = spell['range']
        bolt['damage'] = float(pool)
        bolt['traveled'] = 0.0
        member.instance.bolts.append(bolt)

        bolt_data = {}
        bolt_data['by'] = member.name
        bolt_data['motion'] = motion
        bolt_data['x'] = member.x
        bolt_data['y'] = member.y
        bolt_data['speed'] = spell['speed']
        bolt_data['range'] = spell['range']
        bolt_data['count'] = pool
        self.instance_send(member.instance, protocol.event('bolt', bolt_data))

    def advance_bolts(self, instance):
        if len(instance.bolts) == 0: return

        survivors = []
        for bolt in instance.bolts:
            occupied = None
            before = int(bolt['traveled'])
            bolt['traveled'] += bolt['speed'] * TICK_INTERVAL
            after = int(bolt['traveled'])
            done = bolt['traveled'] >= bolt['range']

            hit = False
            step = before + 1
            while step <= min(after, bolt['range']):
                tx = bolt['x'] + bolt['dx'] * step
                ty = bolt['y'] + bolt['dy'] * step
                if instance.world.tile(tx, ty) != 0:
                    hit = True    # fizzles on the wall
                    break
                if occupied is None:
                    occupied = self.occupied_members(instance, exclude=bolt['by'])
                target = occupied.get((tx, ty))
                if target is not None:
                    self.strike(target, bolt['damage'], bolt['by'], ray_dir=bolt['motion'])
                    hit = True
                    break
                step += 1

            if not hit and not done: survivors.append(bolt)
        instance.bolts = survivors

    async def on_retry(self, session, seq, data):
        if session.state != AUTHED:
            session.send(protocol.error(seq, 'bad_state', "auth first"))
            return
        if self.challenge is None:
            session.send(protocol.error(seq, 'bad_state', "not a challenge server"))
            return

        self.reset_instance(session.instance)
        session.send(protocol.reply('retry_ok', seq, {}))

    # ---------------- dispatch + lifecycle ----------------

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

    def leave_instance(self, session):
        instance = session.instance
        if instance is None: return
        session.instance = None

        if instance.challenge is not None:
            if self.instances.get(instance.name) is instance: del self.instances[instance.name]
            return
        member_name = None
        for name in instance.members:
            if instance.members[name] is session: member_name = name
        if member_name is None: return
        del instance.members[member_name]
        instance.dirty.pop(member_name, None)
        instance.gone.append(member_name)

    def cleanup(self, session):
        name = session.name
        if name is None: return
        if self.sessions.get(name) is not session: return

        del self.sessions[name]
        self.leave_instance(session)
        if self.challenge is None:
            self.db.set_state(session.player['id'], session.x, session.y,
                              round(session.hp, 1), round(session.stamina, 1))
        self.db.touch_last_seen(session.player['id'])

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

    # ---------------- tick ----------------

    def regen(self):
        for name in self.sessions:
            session = self.sessions[name]
            if session.instance is None: continue
            if session.instance.status != 'running': continue
            if session.hp < session.hp_max:
                session.hp = min(session.hp_max, session.hp + RULES['hp_regen'] * TICK_INTERVAL)
            if session.defending:
                session.stamina -= RULES['defend_drain'] * TICK_INTERVAL
                if session.stamina <= 0:
                    session.stamina = 0
                    self.set_defending(session, False)
                continue
            if session.stamina < RULES['stamina_max']:
                session.stamina = min(RULES['stamina_max'],
                                      session.stamina + RULES['stamina_regen'] * TICK_INTERVAL)

    def revive_npcs(self, instance):
        # open-world training targets come back; challenge npcs stay dead
        if instance.challenge is not None: return
        now = time.time()
        for name in instance.members:
            npc = instance.members[name]
            if not npc.is_npc: continue
            if not npc.dead: continue
            if now < npc.respawn_at: continue

            spot = instance.world.find_free_near(npc.home[0], npc.home[1],
                                                 self.occupied_set(instance))
            if spot is None: continue    # stay dead until its home clears
            npc.dead = False
            npc.hp = npc.hp_max
            npc.x, npc.y = spot
            self.mark_hurt(npc)

    def strike(self, target, damage, by_name, ray_dir=None):
        blocks = target.defending
        if blocks and ray_dir is not None:
            if target.facing != OPPOSITE[ray_dir]: blocks = False    # flanked
        if blocks:
            blocked = min(damage, target.stamina)
            target.stamina -= blocked
            rest = damage - blocked
            if rest > 0 or target.stamina <= 0: self.set_defending(target, False)
            if rest > 0: self.apply_damage(target, min(rest, target.hp), by_name)
            else: target.send(protocol.event('vitals', target.vitals()))
            return
        self.apply_damage(target, min(damage, target.hp), by_name)

    def npc_shoot(self, instance, npc):
        # a beam down a fixed line: the first player standing on it is hit;
        # anyone in a side hole is off the line and safe
        swing_data = {}
        swing_data['by'] = npc.name
        swing_data['motion'] = npc.fire_dir
        swing_data['count'] = 40
        swing_data['x'] = npc.x
        swing_data['y'] = npc.y
        self.instance_send(instance, protocol.event('melee', swing_data))

        occupied = self.occupied_members(instance, exclude=npc.name)
        targets = instance.world.ray_targets(npc.x, npc.y, npc.fire_dir, 200, occupied)
        for other, distance in targets:
            if other.is_npc: continue
            self.strike(other, npc.damage, npc.name, ray_dir=npc.fire_dir)
            return

    def npc_chase(self, instance, npc):
        target = None
        best = None
        for member in instance.players():
            distance = abs(member.x - npc.x) + abs(member.y - npc.y)
            if best is None or distance < best:
                best = distance
                target = member

        if target is None: return
        if best == 1:
            # adjacent: telegraph, then bite the locked direction
            if npc.pending is not None: return
            motion = 'l'
            if target.x < npc.x: motion = 'h'
            if target.y < npc.y: motion = 'k'
            if target.y > npc.y: motion = 'j'
            npc.facing = motion

            pending = {}
            pending['kind'] = 'bite'
            pending['motion'] = motion
            pending['pool'] = 0
            pending['at'] = time.time() + CHASER_WINDUP
            npc.pending = pending

            wind_data = {}
            wind_data['by'] = npc.name
            wind_data['motion'] = motion
            wind_data['count'] = 1
            wind_data['x'] = npc.x
            wind_data['y'] = npc.y
            wind_data['strike_in'] = CHASER_WINDUP
            self.instance_send(instance, protocol.event('windup', wind_data))

            npc.next_act = time.time() + CHASER_WINDUP + random.uniform(0.5, 0.9)
            return

        # step toward the target, larger axis first, sidestep if blocked
        motions = []
        dx = target.x - npc.x
        dy = target.y - npc.y
        if abs(dx) >= abs(dy):
            if dx > 0: motions.append('l')
            if dx < 0: motions.append('h')
            if dy > 0: motions.append('j')
            if dy < 0: motions.append('k')
        else:
            if dy > 0: motions.append('j')
            if dy < 0: motions.append('k')
            if dx > 0: motions.append('l')
            if dx < 0: motions.append('h')

        occupied = self.occupied_set(instance, exclude=npc.name)
        for motion in motions:
            granted, x, y = instance.world.resolve_move(npc.x, npc.y, motion, 1, occupied)
            if granted <= 0: continue
            npc.x = x
            npc.y = y
            self.mark_moved(npc)
            return

    def npc_mage(self, instance, npc):
        target = None
        best = None
        for member in instance.players():
            distance = abs(member.x - npc.x) + abs(member.y - npc.y)
            if best is None or distance < best:
                best = distance
                target = member
        if target is None: return

        spell = SPELL_INDEX['firebolt']
        motion = None
        dist = 0
        if npc.x == target.x and npc.y != target.y:
            motion = 'j' if target.y > npc.y else 'k'
            dist = abs(target.y - npc.y)
        elif npc.y == target.y and npc.x != target.x:
            motion = 'l' if target.x > npc.x else 'h'
            dist = abs(target.x - npc.x)

        if motion is not None and dist <= spell['range']:
            npc.facing = motion
            pending = {}
            pending['kind'] = 'magic'
            pending['motion'] = motion
            pending['pool'] = int(npc.damage)
            pending['spell'] = 'firebolt'
            pending['at'] = time.time() + MAGE_WINDUP
            npc.pending = pending

            wind_data = {}
            wind_data['by'] = npc.name
            wind_data['motion'] = motion
            wind_data['count'] = spell['range']
            wind_data['x'] = npc.x
            wind_data['y'] = npc.y
            wind_data['strike_in'] = MAGE_WINDUP
            self.instance_send(instance, protocol.event('windup', wind_data))

            npc.next_act = time.time() + MAGE_WINDUP + npc.every
            return

        # not aligned: one step along the smaller offset to line up
        motions = []
        dx = target.x - npc.x
        dy = target.y - npc.y
        if abs(dx) <= abs(dy):
            if dx > 0: motions.append('l')
            if dx < 0: motions.append('h')
            if dy > 0: motions.append('j')
            if dy < 0: motions.append('k')
        else:
            if dy > 0: motions.append('j')
            if dy < 0: motions.append('k')
            if dx > 0: motions.append('l')
            if dx < 0: motions.append('h')

        occupied = self.occupied_set(instance, exclude=npc.name)
        for motion in motions:
            granted, x, y = instance.world.resolve_move(npc.x, npc.y, motion, 1, occupied)
            if granted <= 0: continue
            npc.x = x
            npc.y = y
            self.mark_moved(npc)
            break
        npc.next_act = time.time() + 0.5

    def act_npcs(self, instance):
        if instance.status != 'running': return
        now = time.time()
        for name in instance.members:
            member = instance.members[name]
            if not member.is_npc: continue
            if member.dead: continue
            if member.pending is not None: continue
            if now < member.next_act: continue

            if member.kind == 'walker':
                member.next_act = now + random.uniform(0.5, 1.2)
                motion = random.choice(('h', 'j', 'k', 'l'))
                occupied = self.occupied_set(instance, exclude=name)
                granted, x, y = instance.world.resolve_move(member.x, member.y, motion, 1, occupied)
                if granted <= 0: continue
                member.x = x
                member.y = y
                self.mark_moved(member)
            elif member.kind == 'chaser':
                member.next_act = now + random.uniform(0.4, 0.7)
                self.npc_chase(instance, member)
            elif member.kind == 'shooter':
                member.next_act = now + member.every
                self.npc_shoot(instance, member)
            elif member.kind == 'mage':
                self.npc_mage(instance, member)

    def flush_instance(self, instance):
        if len(instance.dirty) == 0 and len(instance.gone) == 0: return

        dirty = instance.dirty
        gone = instance.gone
        instance.dirty = {}
        instance.gone = []

        for member in instance.players():
            entries = []
            for moved_name in dirty:
                if moved_name == member.name: continue    # own moves arrive via intent_ok
                entry = {}
                entry['name'] = moved_name
                entry.update(dirty[moved_name])
                entries.append(entry)

            if len(entries) == 0 and len(gone) == 0: continue

            state_data = {}
            if len(entries) > 0: state_data['players'] = entries
            if len(gone) > 0: state_data['gone'] = gone
            member.send(protocol.event('state', state_data))

    async def ticker(self):
        while True:
            await asyncio.sleep(TICK_INTERVAL)
            self.regen()
            for name in list(self.instances):
                instance = self.instances.get(name)
                if instance is None: continue
                self.revive_npcs(instance)
                self.act_npcs(instance)
                self.resolve_pending(instance)
                self.advance_bolts(instance)
                self.check_objective(instance)
                self.flush_instance(instance)

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
            mode = "open world"
            if self.challenge is not None: mode = f"challenge ({self.challenge.objective_text})"
            print(f"[+] serving on ws://{self.host}:{self.port} — {mode}", flush=True)
            try:
                await asyncio.get_running_loop().create_future()
            finally:
                reaper_task.cancel()
                ticker_task.cancel()
