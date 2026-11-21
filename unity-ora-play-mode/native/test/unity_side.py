"""Unity's side of the loopback test: creates the link, starts host_side.js with --ora-link like Ora
starts the browser, sends CreateWindow-like requests and a large message, and reads the replies."""
import ctypes as C
import struct
import subprocess
import sys
import time

lib = C.CDLL(sys.argv[1])
addon, host_js = sys.argv[2], sys.argv[3]

class LinkConfig(C.Structure):
    _fields_ = [("structSize", C.c_uint32), ("flags", C.c_uint32), ("arenaBytes", C.c_uint32), ("role", C.c_uint32)]

class ChannelConfig(C.Structure):
    _fields_ = [("structSize", C.c_uint32), ("profile", C.c_uint32), ("u2hBytes", C.c_uint32), ("h2uBytes", C.c_uint32)]

class LinkState(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ("structSize", "unityState", "hostState", "refuseReason", "creatorAbi", "hostAbi",
                                          "creatorPid", "hostPid", "linkEpoch", "flags")] + \
               [("peerExited", C.c_int32), ("peerExitCode", C.c_int32)] + \
               [(n, C.c_uint64) for n in ("unityBeatNs", "hostNativeBeatNs", "hostLoopBeatNs", "nowNs", "creatorStartTime", "hostStartTime")]

class ViewState(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ("structSize", "seq", "flags", "bufferGen", "texW", "texH", "paintSeq", "loadState")] + \
               [("platformTex", C.c_uint64), ("hostBeatNs", C.c_uint64), ("audioState", C.c_uint32), ("zoom", C.c_float),
                ("r0", C.c_uint32), ("r1", C.c_uint32)]

lib.OraBridge_CreateLink.restype = C.c_int64
lib.OraBridge_TakeReady.restype = C.c_uint64
for f in ("OraBridge_SetUnityState", "OraBridge_WatchPeer", "OraBridge_GetLinkState", "OraBridge_OpenChannel", "OraBridge_Write",
          "OraBridge_Take", "OraBridge_GetViewState", "OraBridge_CloseChannel", "OraBridge_FlushSpill"):
    getattr(lib, f).argtypes = None
I64 = C.c_int64

assert lib.OraBridge_AbiVersion() == 0x00020001
cfg = LinkConfig(16, 0, 32 << 20, ord("e"))
name = C.create_string_buffer(128)
link = lib.OraBridge_CreateLink(C.byref(cfg), name, 128)
assert link > 0, link
print("unity: link", name.value.decode())

def head(op, cls, codec, req=0, a0=0, a1=0, typ=1, flags=0):
    return struct.pack("<IBBHIHBBIIII", 0, typ, cls, flags, 0, op, codec, 0, req, 0, a0, a1)

def write(ch, h, payload=b""):
    buf = C.create_string_buffer(payload, len(payload)) if payload else None
    return lib.OraBridge_Write(I64(link), ch, C.c_char_p(h), buf, 0, len(payload))

def take(ch):
    cap, out = 1 << 16, []
    while True:
        dst = C.create_string_buffer(cap)
        needed = C.c_int32(0)
        r = lib.OraBridge_Take(I64(link), ch, dst, cap, 64, C.byref(needed))
        if r == 0 and needed.value > 0:
            cap = needed.value * 2
            continue
        if r <= 0:
            return out, r
        raw, off = dst.raw[:r], 0
        while off + 32 <= r:
            rb, typ, cls, flags, seq, op, codec, _, req, ep, a0, a1 = struct.unpack_from("<IBBHIHBBIIII", raw, off)
            out.append(dict(op=op, cls=cls, codec=codec, req=req, a0=a0, seq=seq, payload=raw[off + 32:off + rb]))
            off += (rb + 7) & ~7

host = subprocess.Popen(["node", host_js, addon, "--ora-link", name.value.decode()])
lib.OraBridge_WatchPeer(I64(link), host.pid)
lib.OraBridge_SetUnityState(I64(link), 1)

st = LinkState(); st.structSize = C.sizeof(LinkState)
deadline = time.time() + 10
while time.time() < deadline:
    lib.OraBridge_GetLinkState(I64(link), C.byref(st))
    if st.hostState == 1:
        break
    time.sleep(0.02)
assert st.hostState == 1, "browser never attached"
print("unity: host attached, pid", st.hostPid)

chcfg = ChannelConfig(16, 2, 0, 0)
ch = lib.OraBridge_OpenChannel(I64(link), C.byref(chcfg))
assert ch > 256 and (ch & 0xff) == 1, ch
text = "https://example.com/ünï".encode("utf-16-le")
assert write(256, head(0x20, 0, 1, req=7, a0=ch), text) == 0       # CREATE_WINDOW REQ on the app channel
big = bytes(range(256)) * (12 * 4096)                                # 12 MiB page message
pair = struct.pack("<I", 3) + "abc".encode("utf-16-le") + big
assert write(ch, head(0x34, 2, 4, typ=2, flags=32), pair) == 0    # PAGE_SEND, UTF16_PAIR

got, seen = [], {}
deadline = time.time() + 15
while time.time() < deadline and len(got) < 3:
    ready = lib.OraBridge_TakeReady(I64(link))
    for slot in range(64):
        if ready >> slot & 1:
            cid = 256 if slot == 0 else ch
            recs, r = take(cid)
            got += recs
    time.sleep(0.01)
ops = sorted(r["op"] for r in got)
print("unity: got ops", [hex(o) for o in ops])
reply = next(r for r in got if r["op"] == 0x20)
assert reply["cls"] == 1 and reply["req"] == 7 and reply["payload"].decode("utf-16-le") == "window-ok", reply
echo = next(r for r in got if r["op"] == 0x60)
assert echo["payload"][4:4 + 6].decode("utf-16-le") == "abc" and len(echo["payload"]) == len(pair), len(echo["payload"])
vs = ViewState(); vs.structSize = C.sizeof(ViewState)
assert lib.OraBridge_GetViewState(I64(link), ch, C.byref(vs)) == 0 and vs.texW == 1024 and vs.texH == 768, (vs.texW, vs.texH)
print("unity: view state", vs.texW, "x", vs.texH, "bufferGen", vs.bufferGen)

lib.OraBridge_CloseChannel(I64(link), ch, 0)
lib.OraBridge_CloseLink(I64(link), 4)
host.wait(timeout=10)
assert host.returncode == 0, host.returncode
print("loopback OK")
