import json
import socket
import sys
from pathlib import Path

host = "127.0.0.1"
port = 9876
if len(sys.argv) < 2:
    raise SystemExit("usage: mcp_exec_blender.py <python-file>")
src = Path(sys.argv[1]).read_text(encoding="utf-8")
code = (
    "exec(compile(open(r'" + str(Path(sys.argv[1]).resolve()).replace("\\", "/") +
    "', encoding='utf-8').read(), '" + Path(sys.argv[1]).name +
    "', 'exec'), {'__name__':'__main__', '__file__': r'" +
    str(Path(sys.argv[1]).resolve()).replace("\\", "/") + "'})"
)
payload = {"type": "execute_code", "params": {"code": code}}
with socket.create_connection((host, port), 5) as s:
    s.sendall(json.dumps(payload).encode("utf-8"))
    s.settimeout(180)
    data = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
        try:
            json.loads(data.decode("utf-8"))
            break
        except Exception:
            continue
print(data.decode("utf-8", errors="replace"))
