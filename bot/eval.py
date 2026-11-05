# Send a Lua snippet to a test mod's dev channel and print the reply.
# usage: python eval.py [--port 27791] "<lua code>"   or   python eval.py [--port 27792] -f snippet.lua
import socket, sys, random
args = sys.argv[1:]
port = 27791
if args[:1] == ["--port"]:
    port = int(args[1]); args = args[2:]
src = open(args[1]).read() if args[0] == "-f" else args[0]
rid = random.randint(1, 10**6)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(5)
s.sendto(f"EVAL {rid}\n{src}".encode(), ("127.0.0.1", port))
try:
    while True:
        data, _ = s.recvfrom(65535); txt = data.decode(errors="replace")
        if txt.startswith(f"RES {rid} "): print(txt[len(f'RES {rid} '):]); break
except socket.timeout:
    print("TIMEOUT (game not in a level, or mod not loaded)"); sys.exit(1)
