"""Talk to the hub's web server: read entity states, press buttons, flip switches.

    python tools/hubctl.py state [substring]        # all states (from the SSE snapshot), filtered
    python tools/hubctl.py press "Mode Test A ..."  # POST /button/<name>/press   (ENTITY-NAME form)
    python tools/hubctl.py switch "Timed Mode ..." on|off

Entity-name URLs, not object ids: a wrong object id returns an EMPTY BODY, not an
error, so every call here prints the HTTP status and the byte count of the reply.
"""
import base64, json, re, socket, sys, urllib.parse, urllib.request

HUB = "http://192.168.178.91"
# web_server: auth: was added as Tier-1 hardening; every request now needs
# HTTP Basic auth or the hub returns 401 with an empty body (looks identical
# to the "wrong object id" empty-body case this file already warns about).
AUTH_HEADER = "Basic " + base64.b64encode(b"admin:Xu0MWohuDbZg5BKh").decode()

def snapshot(seconds=6):
    """The SSE stream opens with one `state` event per entity; read for a few seconds."""
    req = urllib.request.Request(HUB + "/events", headers={"Authorization": AUTH_HEADER})
    out = {}
    s = urllib.request.urlopen(req, timeout=seconds + 3)
    s.fp.raw._sock.settimeout(seconds)
    buf = b""
    try:
        while True:
            chunk = s.read1(65536) if hasattr(s, "read1") else s.read(65536)
            if not chunk:
                break
            buf += chunk
    except (socket.timeout, TimeoutError, OSError):
        pass
    for m in re.finditer(rb"data: (\{.*\})\r?\n", buf):
        try:
            d = json.loads(m.group(1))
        except ValueError:
            continue
        if "name_id" in d:
            out[d["name_id"]] = d.get("state", d.get("value"))
    return out

def post(kind, name, action):
    url = f"{HUB}/{kind}/{urllib.parse.quote(name)}/{action}"
    req = urllib.request.Request(url, data=b"", method="POST", headers={"Authorization": AUTH_HEADER})
    with urllib.request.urlopen(req, timeout=10) as r:
        body = r.read()
        print(f"POST {url} -> HTTP {r.status}, {len(body)} bytes")

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "state":
        filt = sys.argv[2].lower() if len(sys.argv) > 2 else ""
        st = snapshot()
        print(f"{len(st)} entities in snapshot")
        for k, v in sorted(st.items()):
            if filt in k.lower():
                print(f"{k} = {v}")
    elif cmd == "press":
        post("button", sys.argv[2], "press")
    elif cmd == "cover":
        # python tools/hubctl.py cover "RollladenWohnzimmer2" open|close|stop|<position 0..1>
        arg = sys.argv[3]
        post("cover", sys.argv[2], arg if arg in ("open", "close", "stop") else f"set?position={arg}")
    elif cmd == "switch":
        post("switch", sys.argv[2], "turn_on" if sys.argv[3] == "on" else "turn_off")
