import sqlite3
import time


SCHEMA = """
CREATE TABLE IF NOT EXISTS players (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE NOT NULL,
    salt TEXT NOT NULL,
    pass_hash TEXT NOT NULL,
    token TEXT,
    settings TEXT NOT NULL DEFAULT '{}',
    created_at REAL NOT NULL,
    last_seen REAL NOT NULL
);
"""


class Db():
    def __init__(self, path):
        self.conn = sqlite3.connect(path)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute(SCHEMA)
        self.conn.commit()
        self.__migrate()

    def __migrate(self):
        # layer 2: position columns (NULL = never spawned)
        try:
            self.conn.execute("ALTER TABLE players ADD COLUMN x INTEGER")
            self.conn.execute("ALTER TABLE players ADD COLUMN y INTEGER")
            self.conn.commit()
        except sqlite3.OperationalError:
            pass

    def close(self):
        self.conn.close()

    def get_player_by_name(self, name):
        cursor = self.conn.execute("SELECT * FROM players WHERE name = ?", (name,))
        row = cursor.fetchone()
        if row is None: return None
        return dict(row)

    def create_player(self, name, salt, pass_hash):
        now = time.time()
        self.conn.execute("""INSERT INTO players (name, salt, pass_hash, created_at, last_seen)
                             VALUES (?, ?, ?, ?, ?)""",
                          (name, salt, pass_hash, now, now))
        self.conn.commit()
        return self.get_player_by_name(name)

    def set_token(self, player_id, token):
        self.conn.execute("UPDATE players SET token = ? WHERE id = ?", (token, player_id))
        self.conn.commit()

    def set_settings(self, player_id, settings_json):
        self.conn.execute("UPDATE players SET settings = ? WHERE id = ?", (settings_json, player_id))
        self.conn.commit()

    def set_position(self, player_id, x, y):
        self.conn.execute("UPDATE players SET x = ?, y = ? WHERE id = ?", (x, y, player_id))
        self.conn.commit()

    def touch_last_seen(self, player_id):
        self.conn.execute("UPDATE players SET last_seen = ? WHERE id = ?", (time.time(), player_id))
        self.conn.commit()
