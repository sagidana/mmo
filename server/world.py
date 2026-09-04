FLOOR = 0
WALL = 1

TILE_CHARS = {}
TILE_CHARS['.'] = FLOOR
TILE_CHARS['#'] = WALL
TILE_CHARS['S'] = FLOOR

MOTIONS = {}
MOTIONS['h'] = (-1, 0)
MOTIONS['l'] = (1, 0)
MOTIONS['k'] = (0, -1)
MOTIONS['j'] = (0, 1)


class World():
    def __init__(self, map_path):
        self.tiles = []
        self.spawn = None

        lines = []
        with open(map_path) as map_file:
            for line in map_file.readlines():
                line = line.rstrip('\n')
                if line == "": continue
                lines.append(line)

        self.height = len(lines)
        self.width = 0
        for line in lines:
            if len(line) > self.width: self.width = len(line)

        y = 0
        for line in lines:
            row = []
            x = 0
            for char in line:
                if char not in TILE_CHARS: char = '#'
                row.append(TILE_CHARS[char])
                if char == 'S' and self.spawn is None: self.spawn = (x, y)
                x += 1
            while len(row) < self.width:
                row.append(WALL)
            self.tiles.append(row)
            y += 1

        if self.spawn is None: raise ValueError(f"map {map_path} has no S spawn tile")

    def tile(self, x, y):
        if x < 0 or y < 0: return WALL
        if x >= self.width or y >= self.height: return WALL
        return self.tiles[y][x]

    def flat_tiles(self):
        flat = []
        for row in self.tiles:
            for tile in row:
                flat.append(tile)
        return flat

    def resolve_move(self, x, y, motion, count, occupied):
        dx, dy = MOTIONS[motion]
        granted = 0
        while granted < count:
            nx = x + dx
            ny = y + dy
            if self.tile(nx, ny) != FLOOR: break
            if (nx, ny) in occupied: break
            x = nx
            y = ny
            granted += 1
        return granted, x, y

    def find_free_near(self, x, y, occupied):
        # breadth-first from the desired tile to the nearest free one
        queue = [(x, y)]
        seen = set()
        seen.add((x, y))
        while len(queue) > 0:
            cx, cy = queue.pop(0)
            if self.tile(cx, cy) == FLOOR and (cx, cy) not in occupied: return cx, cy
            for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
                nx = cx + dx
                ny = cy + dy
                if (nx, ny) in seen: continue
                if nx < -1 or ny < -1: continue
                if nx > self.width or ny > self.height: continue
                seen.add((nx, ny))
                queue.append((nx, ny))
        return None
