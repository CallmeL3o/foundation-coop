# Listens on 127.0.0.1:<port> and prints every datagram with a timestamp, for <seconds>.
import socket, sys, time
port, secs = int(sys.argv[1]), float(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", port)); s.settimeout(0.5)
t0 = time.time(); n = 0
while time.time() - t0 < secs:
    try: data, _ = s.recvfrom(65535)
    except socket.timeout: continue
    n += 1; print(f"{time.time()-t0:7.2f}s {data.decode(errors='replace')}", flush=True)
print(f"done, {n} datagrams", flush=True)
