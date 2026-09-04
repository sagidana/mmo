from .world import World


OBJECTIVES = ('kill_all', 'survive', 'reach')


def parse_npc(line):
    tokens = line.split()
    if len(tokens) < 2: raise ValueError(f"bad npc line: {line}")

    spec = {}
    spec['kind'] = tokens[0]
    x_str, y_str = tokens[1].split(',')
    spec['x'] = int(x_str)
    spec['y'] = int(y_str)
    spec['hp'] = 30.0
    spec['damage'] = 5.0
    spec['dir'] = 'h'
    spec['every'] = 2.5
    spec['figure'] = None

    for token in tokens[2:]:
        key, value = token.split('=')
        if key == 'hp': spec['hp'] = float(value)
        if key == 'damage': spec['damage'] = float(value)
        if key == 'dir': spec['dir'] = value
        if key == 'every': spec['every'] = float(value)
        if key == 'figure': spec['figure'] = value
    return spec


class Challenge():
    def __init__(self, path):
        self.rules = {}
        self.player = {}
        self.npcs = []
        self.objective = ('kill_all',)
        self.objective_text = "kill all enemies"

        map_lines = []
        section = None
        with open(path) as challenge_file:
            lines = challenge_file.readlines()

        for raw in lines:
            line = raw.rstrip('\n')

            if section == 'map':
                map_lines.append(line)
                continue

            stripped = line.strip()
            if stripped == "": continue
            if stripped.startswith(';'): continue
            if stripped.startswith('[') and stripped.endswith(']'):
                section = stripped[1:-1]
                continue
            if ';' in stripped:
                stripped = stripped.split(';')[0].strip()

            if section == 'rules':
                key, value = stripped.split('=')
                self.rules[key.strip()] = float(value.strip())
            elif section == 'player':
                key, value = stripped.split('=')
                self.player[key.strip()] = float(value.strip())
            elif section == 'npcs':
                self.npcs.append(parse_npc(stripped))
            elif section == 'objective':
                self.__parse_objective(stripped)

        if len(map_lines) == 0: raise ValueError(f"challenge {path} has no [map] section")
        self.world = World(map_lines)

        self.npc_names = []
        counters = {}
        for spec in self.npcs:
            kind = spec['kind']
            counters[kind] = counters.get(kind, 0) + 1
            spec['name'] = f"{kind}_{counters[kind]}"
            self.npc_names.append(spec['name'])

    def __parse_objective(self, line):
        tokens = line.split()
        kind = tokens[0]
        if kind not in OBJECTIVES: raise ValueError(f"unknown objective: {line}")

        if kind == 'kill_all':
            self.objective = ('kill_all',)
            self.objective_text = "kill all enemies"
        elif kind == 'survive':
            seconds = float(tokens[1])
            self.objective = ('survive', seconds)
            self.objective_text = f"survive {int(seconds)}s"
        elif kind == 'reach':
            x_str, y_str = tokens[1].split(',')
            self.objective = ('reach', int(x_str), int(y_str))
            self.objective_text = f"reach {x_str},{y_str}"
