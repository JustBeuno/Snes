"""End-to-end test of the built-in SMZ3 client against a real Archipelago server.

Player 1 = the C client (harness) using only the ROM title to log in.
Player 2 = this script, speaking the protocol directly.
"""
import asyncio, glob, json, os, socket, subprocess, sys, time, zlib, base64

AP = "/home/claude/work/Archipelago"
sys.path.insert(0, AP)
os.chdir(AP)
from Utils import restricted_loads  # noqa: E402

OUT = "/tmp/claude-0/aptest/out"
HARNESS = "/home/claude/work/test/harness"
CFGDIR = "/tmp/claude-0/aptest/cfg"
PORT = int(os.environ.get("AP_PORT", "38281"))
SCHEME = os.environ.get("AP_SCHEME", "")          # "", "ws://", "wss://"
CERT = os.environ.get("AP_CERT")                   # server cert (TLS test)
KEY = os.environ.get("AP_KEY")
HOSTNAME = os.environ.get("AP_HOST", "127.0.0.1")

ITEMS0, LOCS0 = 84000, 85000

md_path = glob.glob(OUT + "/*.archipelago")[0]
md = restricted_loads(zlib.decompress(open(md_path, "rb").read()[1:]))
names = {v[1]: k for k, v in md["connect_names"].items() if not k.startswith("W")}
p1_title = [base64.b64decode(k).decode() for k, v in md["connect_names"].items() if v == (0, 1) and k.startswith("W")][0]

locs = md["locations"]
p1_remote = [(loc, it) for loc, it in locs[2].items() if it[1] == 1]           # in P2's world, for P1
p1_finds = [(loc, it) for loc, it in locs[1].items() if it[1] == 2]            # in P1's world, for P2
start_inv = md["precollected_items"].get(1, [])
print(f"P1 title {p1_title}; P2 holds {len(p1_remote)} items for P1; P1 holds {len(p1_finds)} for P2; P1 start inventory {len(start_inv)}")

def ap_to_smz3_index(loc_id):
    idx = loc_id - LOCS0
    return idx + 34 if 256 + 196 <= idx <= 256 + 202 else idx


def start_server():
    args = [sys.executable, "MultiServer.py", "--host", "0.0.0.0", "--port", str(PORT), md_path]
    if CERT:
        args[2:2] = ["--cert", CERT, "--cert_key", KEY]
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.5).close()
            return proc
        except OSError:
            time.sleep(0.3)
    raise SystemExit("server did not start: " + proc.stdout.read())


class Harness:
    def __init__(self):
        os.makedirs(CFGDIR, exist_ok=True)
        with open(CFGDIR + "/archipelago.cfg", "w") as f:
            f.write(f"server={SCHEME}{HOSTNAME}:{PORT}\npassword=\n")
            if CERT:
                f.write(f"ca_file={CERT}\n")
        self.p = subprocess.Popen([HARNESS, p1_title, CFGDIR], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.lines = []
        os.set_blocking(self.p.stdout.fileno(), False)

    def send(self, s):
        self.p.stdin.write(s + "\n"); self.p.stdin.flush()

    def pump(self):
        while True:
            l = self.p.stdout.readline()
            if not l:
                return
            l = l.rstrip()
            self.lines.append(l)
            if l.startswith(("OSD", "INBOX", "OUTBOX", "RELOADED", "BYE")) or "[Archipelago]" in l:
                print("  harness|", l)

    async def wait_for(self, text, timeout=20):
        end = time.time() + timeout
        start = len(self.lines)
        while time.time() < end:
            self.pump()
            for l in self.lines[start:]:
                if text in l:
                    return l
            await asyncio.sleep(0.1)
        raise AssertionError(f"timed out waiting for {text!r}")

    async def inbox(self):
        mark = len(self.lines)
        self.send("inbox")
        await self.wait_for("OUTBOX")
        il = [l for l in self.lines[mark:] if l.startswith("INBOX")][-1]
        ol = [l for l in self.lines[mark:] if l.startswith("OUTBOX")][-1]
        entries = [tuple(map(int, e.split("/"))) for e in il.split(":", 1)[1].split()]
        return entries, ol


async def p2_session(ws_url, ssl_ctx):
    import websockets
    ws = await websockets.connect(ws_url, ssl=ssl_ctx, max_size=None)
    received, prints = [], []

    async def reader():
        async for raw in ws:
            for cmd in json.loads(raw):
                if cmd["cmd"] == "ReceivedItems":
                    received.extend(cmd["items"])
                elif cmd["cmd"] == "PrintJSON":
                    prints.append(cmd)
    info = json.loads(await ws.recv())[0]
    await ws.send(json.dumps([{"cmd": "Connect", "password": "", "name": "Friend", "version": info["version"],
                               "tags": [], "items_handling": 7, "uuid": "p2", "game": "SMZ3", "slot_data": False}]))
    task = asyncio.create_task(reader())
    return ws, received, prints, task


async def main():
    import ssl
    server = start_server()
    try:
        h = Harness()
        await h.wait_for("connected as TestPlayer", 40)
        print("PASS: logged in from the ROM title alone")

        ssl_ctx = None
        url = f"ws://127.0.0.1:{PORT}"
        if CERT:
            ssl_ctx = ssl.create_default_context(cafile=CERT)
            url = f"wss://{HOSTNAME}:{PORT}"
        ws, p2_recv, p2_prints, task = await p2_session(url, ssl_ctx)

        # --- P2 finds P1's items -> should land in the ROM inbox ---
        await ws.send(json.dumps([{"cmd": "LocationChecks", "locations": [l for l, _ in p1_remote]}]))
        expected = [(2, it[0] - ITEMS0) for _, it in p1_remote]
        for _ in range(100):
            entries, _ = await h.inbox()
            if len(entries) >= len(start_inv) + len(expected):
                break
            await asyncio.sleep(0.3)
        got = [e for e in entries if e[0] == 2]
        assert sorted(got) == sorted(expected), (got, expected)
        print(f"PASS: {len(got)} items from player 2 written to the game inbox with correct ids")

        # --- P1 finds P2's items -> P2 should receive them ---
        sample = p1_finds[:6]
        extra = [x for x in p1_finds if 256 + 196 <= x[0] - LOCS0 <= 256 + 202][:1]
        sample += [x for x in extra if x not in sample]
        for loc, _ in sample:
            h.send(f"check {ap_to_smz3_index(loc)}")
        for _ in range(100):
            if len({r['location'] for r in p2_recv if r['player'] == 1}) >= len(sample):
                break
            await asyncio.sleep(0.2)
        got_locs = {r["location"] for r in p2_recv if r["player"] == 1}
        assert {l for l, _ in sample} <= got_locs, ({l for l, _ in sample} - got_locs)
        print(f"PASS: {len(sample)} checks from the game reached player 2"
              + (" (incl. a renumbered location)" if extra else ""))
        await h.wait_for("TestPlayer sent", 10)
        print("PASS: on-screen item messages rendered with names")

        # --- reload core: no duplicate items ---
        before, _ = await h.inbox()
        h.send("reload")
        await h.wait_for("connected as TestPlayer", 40)
        await asyncio.sleep(2)
        after, ol = await h.inbox()
        assert before == after, (before, after)
        print("PASS: core restart does not duplicate items")

        # --- server restart: client reconnects on its own ---
        server.terminate(); server.wait()
        await h.wait_for("reconnecting", 20)
        server = start_server()
        await h.wait_for("connected as TestPlayer", 60)
        print("PASS: reconnected after the server went away")
        ws, p2_recv, p2_prints, task = await p2_session(url, ssl_ctx)

        # --- goal ---
        h.send("goal")
        for _ in range(100):
            if any(p.get("type") == "Goal" for p in p2_prints):
                break
            await asyncio.sleep(0.2)
        assert any(p.get("type") == "Goal" for p in p2_prints), "no goal broadcast"
        print("PASS: goal completion reported to the server")

        h.send("quit")
        await h.wait_for("BYE", 10)
        print("PASS: clean shutdown")
        print("ALL TESTS PASSED")
    finally:
        server.terminate()


asyncio.run(main())
