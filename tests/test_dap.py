#!/usr/bin/env python3
"""DAP smoke test: drive myp_debug (DAP server) against a -g compiled MYP
program, verifying launch / breakpoint / continue / stack / variables."""
import json
import os
import subprocess
import sys
import time

MYPCC = os.environ.get("MYPCC", "./build/mypc")
DAP = os.environ.get("MYP_DEBUG", "./build/myp_debug")

TMP = "/tmp/myp_dap_test"
os.makedirs(TMP, exist_ok=True)
SRC = os.path.join(TMP, "dap.myp")
EXE = os.path.join(TMP, "dap.out")

SRC_TEXT = """
// DAP test program: break at line 5 in foo(), then main.
int foo(int a, int b) {
    int sum = a + b;      // line 5
    return sum;
}
int main() {
    int x = foo(20, 22);  // line 9
    int y = 7;
    return x + y;
}
"""
with open(SRC, "w") as f:
    f.write(SRC_TEXT)

rc = subprocess.run([MYPCC, "-g", SRC, "-o", EXE], capture_output=True, timeout=30)
print(f"[dap-test] compile rc={rc.returncode}", flush=True)
if rc.returncode != 0:
    print("FAIL: compile -g", rc.stderr.decode()[:300])
    sys.exit(1)

print("[dap-test] spawning myp_debug", flush=True)
_errf = open("/tmp/myp_dap_test/dap_debug.log", "w")
proc = subprocess.Popen(
    [DAP],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=_errf,
    bufsize=0,
)


def send(msg):
    body = json.dumps(msg).encode()
    proc.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    proc.stdin.flush()


_recv_buf = b""


def recv(timeout=10):
    """Read one DAP frame from myp_debug using raw fd reads. Keeps a module-
    level buffer so multiple frames read in one chunk are not lost."""
    global _recv_buf
    import select
    fd = proc.stdout.fileno()
    end = time.time() + timeout
    while time.time() < end:
        if b"\r\n\r\n" not in _recv_buf:
            r, _, _ = select.select([fd], [], [], 1)
            if r:
                chunk = os.read(fd, 4096)
                if not chunk:
                    return None
                _recv_buf += chunk
                continue
        while b"\r\n\r\n" in _recv_buf:
            head, rest = _recv_buf.split(b"\r\n\r\n", 1)
            cl = head.find(b"Content-Length:")
            if cl < 0:
                _recv_buf = rest
                continue
            n = int(head[cl + 15:].strip())
            if len(rest) >= n:
                body = rest[:n].decode()
                _recv_buf = rest[n:]
                try:
                    return json.loads(body)
                except Exception:
                    return {"raw": body}
            else:
                break  # need more bytes for body
        if not _recv_buf and time.time() >= end:
            break
    return None


def wait_event(name, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        m = recv()
        if m is None:
            return None
        if m.get("type") == "event" and m.get("event") == name:
            return m
    return None


def request(cmd, args, expect_response=True):
    send({"seq": 1, "type": "request", "command": cmd, "arguments": args})
    while True:
        m = recv()
        if m is None:
            return None
        if m.get("type") == "response":
            return m
        # skip events


results = []
PASS = 0
FAIL = 0


def check(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  PASS: {name}")
    else:
        FAIL += 1
        print(f"  FAIL: {name} {detail}")

# initialize
r = request("initialize", {})
check("initialize response", r and r.get("success") is True)
ev = wait_event("initialized")
check("initialized event", ev is not None)

# launch
r = request("launch", {"program": EXE})
check("launch response", r and r.get("success") is True)

# setBreakpoints at line 5 of the .myp source
r = request("setBreakpoints", {"source": {"path": SRC}, "breakpoints": [{"line": 5}]})
bps = (r or {}).get("body", {}).get("breakpoints", [])
check("setBreakpoints", bool(bps) and bps[0].get("verified") is True)

r = request("configurationDone", {})
check("configurationDone", r and r.get("success") is True)

# continue -> expect stopped at breakpoint
r = request("continue", {})
check("continue response", r and r.get("success") is True)
ev = wait_event("stopped", timeout=15)
check("stopped at breakpoint", ev is not None)
reason = (ev or {}).get("body", {}).get("reason")
check("stopped reason == breakpoint", reason == "breakpoint", f"got {reason}")

# stack trace
r = request("stackTrace", {"threadId": 1})
frames = (r or {}).get("body", {}).get("stackFrames", [])
check("stackTrace has frames", len(frames) > 0, f"got {len(frames)}")
hit_line = frames[0].get("line") if frames else None
check("top frame at line 5 (foo)", hit_line == 5, f"got {hit_line}")

# scopes + variables
r = request("scopes", {"frameId": 0})
scopes = (r or {}).get("body", {}).get("scopes", [])
check("scopes has Locals", len(scopes) > 0)
ref = scopes[0].get("variablesReference", 0) if scopes else 0
r = request("variables", {"variablesReference": ref})
vars_list = (r or {}).get("body", {}).get("variables", [])
names = {v.get("name") for v in vars_list}
check("locals has 'a'", "a" in names, f"got {sorted(names)}")
check("locals has 'sum'", "sum" in names, f"got {sorted(names)}")

# evaluate
r = request("evaluate", {"expression": "a + b", "context": "hover"})
val = (r or {}).get("body", {}).get("result") if r else None
check("evaluate a+b == 42", val == "42", f"got {val!r}")

# disconnect
r = request("disconnect", {})
check("disconnect response", r and r.get("success") is True)

try:
    proc.stdin.close()
except Exception:
    pass
proc.terminate()

print(f"=== summary: dap PASS={PASS} FAIL={FAIL} ===")
sys.exit(0 if FAIL == 0 else 1)
