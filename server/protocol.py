import json


PROTO_VERSION = 1

MAX_FRAME_BYTES = 64 * 1024
MAX_SETTINGS_BYTES = 16 * 1024


class BadFrame(Exception):
    pass


def parse(raw):
    if isinstance(raw, bytes): raise BadFrame("binary frames not accepted in layer 1")
    if len(raw) > MAX_FRAME_BYTES: raise BadFrame("frame too large")

    try:
        message = json.loads(raw)
    except json.JSONDecodeError:
        raise BadFrame("not json")

    if not isinstance(message, dict): raise BadFrame("not an object")

    msg_type = message.get('type')
    if not isinstance(msg_type, str): raise BadFrame("missing type")

    seq = message.get('seq')
    if not isinstance(seq, int): raise BadFrame("missing seq")

    data = message.get('data', {})
    if not isinstance(data, dict): raise BadFrame("data not an object")

    return msg_type, seq, data


def reply(msg_type, seq, data):
    message = {}
    message['type'] = msg_type
    message['seq'] = seq
    message['data'] = data
    return json.dumps(message)


def event(msg_type, data):
    message = {}
    message['type'] = msg_type
    message['data'] = data
    return json.dumps(message)


def error(seq, code, text):
    data = {}
    data['code'] = code
    data['message'] = text
    return reply('error', seq, data)
