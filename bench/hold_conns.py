"""Open N keep-alive connections, issue one request on each, hold them, then reset them."""
import socket
import struct
import sys
import time

n, hold = int(sys.argv[1]), float(sys.argv[2])
socks = []
for _ in range(n):
    s = socket.create_connection(("10.0.0.2", 80))
    s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    socks.append(s)
for s in socks:
    s.recv(4096)
print(f"holding {len(socks)} connections", flush=True)
time.sleep(hold)
for s in socks:
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()
