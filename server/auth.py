import hashlib
import secrets


def hash_password(password, salt_hex):
    salt = bytes.fromhex(salt_hex)
    digest = hashlib.scrypt(password.encode(), salt=salt, n=16384, r=8, p=1)
    return digest.hex()


def new_salt():
    return secrets.token_hex(16)


def new_token():
    return secrets.token_hex(16)


class Auth():
    def __init__(self, db):
        self.db = db

    def login_password(self, name, password):
        player = self.db.get_player_by_name(name)

        if player is None:
            salt = new_salt()
            pass_hash = hash_password(password, salt)
            player = self.db.create_player(name, salt, pass_hash)
        else:
            expected = player['pass_hash']
            actual = hash_password(password, player['salt'])
            if not secrets.compare_digest(expected, actual): return None

        token = new_token()
        self.db.set_token(player['id'], token)
        player['token'] = token
        return player

    def login_token(self, name, token):
        player = self.db.get_player_by_name(name)
        if player is None: return None
        if player['token'] is None: return None
        if not secrets.compare_digest(player['token'], token): return None
        return player
