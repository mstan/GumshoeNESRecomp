#!/usr/bin/env python3
"""Install follows on $24/$25/$C5/$2C so subsequent writes are captured."""
import socket, json


def cmd(c):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", 4372))
    s.sendall((json.dumps(c) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        buf += s.recv(65536)
    s.close()
    return json.loads(buf.decode().strip())


print("ping:", cmd({"cmd": "ping"}))
for a in (0x24, 0x25, 0xC5, 0x2C):
    r = cmd({"cmd": "follow", "addr": f"{a:04X}"})
    print(f"follow ${a:02X}:", r)
print("ready frame:", cmd({"cmd": "frame"}))
