import argparse
import asyncio

from .server import Server


def main():
    parser = argparse.ArgumentParser(description="gridmmo server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=4000)
    parser.add_argument("--db", default="gridmmo.db")
    parser.add_argument("--map", default="server/map.txt")
    args = parser.parse_args()

    server = Server(args.host, args.port, args.db, args.map)
    try:
        asyncio.run(server.serve())
    except KeyboardInterrupt:
        print("[+] ctrl-c, closing server")


if __name__ == "__main__":
    main()
