"""Read Scaleform text straight out of a running Street Fighter V.

Read-only verification harness for the native mod. It finds every Scaleform
text field, its paragraphs, its stage position and the tint it is drawn with,
then prints what a narrator would say. No writes, no injection.

    python tools/read_scaleform.py            # one snapshot
    python tools/read_scaleform.py --watch    # print selection changes
    python tools/read_scaleform.py --wide     # sweep every read-write page

The field offsets below are facts measured against the shipped build and were
first published in the SFV Access project's scaleform.py.
"""
from __future__ import annotations

import argparse
import ctypes
import struct
import sys
import time
from ctypes import wintypes

# --------------------------------------------------------------------- memory
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
PAGE_READWRITE = 0x04
PAGE_WRITECOPY = 0x08
PAGE_EXECUTE_READWRITE = 0x40
PAGE_EXECUTE_WRITECOPY = 0x80
READABLE = (PAGE_READWRITE, PAGE_WRITECOPY, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY)


class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong),
        ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD),
        ("__alignment1", wintypes.DWORD),
        ("RegionSize", ctypes.c_ulonglong),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
        ("__alignment2", wintypes.DWORD),
    ]


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
kernel32.VirtualQueryEx.argtypes = (
    wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MEMORY_BASIC_INFORMATION64), ctypes.c_size_t)
kernel32.ReadProcessMemory.argtypes = (
    wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))


def find_pid(name: str) -> int:
    """The game process, not the small Steam launcher of the same name."""
    import subprocess
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    pids = []
    for line in out.splitlines():
        fields = [f.strip('"') for f in line.split('","')]
        if len(fields) >= 5 and fields[0].lower() == name.lower():
            pids.append((int(fields[1]), int(fields[4].replace(",", "").replace(" K", "") or 0)))
    if not pids:
        return 0
    return max(pids, key=lambda p: p[1])[0]


class Mem:
    def __init__(self, pid: int) -> None:
        self.h = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.h:
            raise OSError("OpenProcess failed: " + ctypes.FormatError(ctypes.get_last_error()))

    def read(self, addr: int, size: int) -> bytes | None:
        if not addr or size <= 0:
            return None
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t()
        ok = kernel32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
        return buf.raw[: got.value] if ok else None

    def u64(self, addr: int) -> int:
        b = self.read(addr, 8)
        return struct.unpack("<Q", b)[0] if b and len(b) == 8 else 0

    def u32(self, addr: int) -> int:
        b = self.read(addr, 4)
        return struct.unpack("<I", b)[0] if b and len(b) == 4 else 0

    def regions(self, wide: bool):
        mbi = MEMORY_BASIC_INFORMATION64()
        addr = 0
        limit = 0x7FFFFFFFFFFF
        while addr < limit:
            if not kernel32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
                break
            size = mbi.RegionSize
            if (mbi.State == MEM_COMMIT and mbi.Protect in READABLE and size <= 0x100000
                    and (wide or (size > 0x10000 and size % 0x10000 == 0x1000))):
                yield mbi.BaseAddress, size
            addr = mbi.BaseAddress + max(size, 0x1000)


def module_base(pid: int) -> int:
    """The load address of the game exe, which ASLR moves on every run."""
    TH32CS_SNAPMODULE = 0x08
    TH32CS_SNAPMODULE32 = 0x10

    class MODULEENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
                    ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.c_void_p),
                    ("modBaseSize", wintypes.DWORD), ("hModule", wintypes.HMODULE),
                    ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260)]

    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Module32First.argtypes = (wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32))
    kernel32.Module32Next.argtypes = (wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32))
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    entry = MODULEENTRY32()
    entry.dwSize = ctypes.sizeof(MODULEENTRY32)
    ok = kernel32.Module32First(snap, ctypes.byref(entry))
    while ok:
        path = entry.szExePath.decode("mbcs", "replace").lower()
        if entry.szModule.decode("mbcs", "replace").lower() == "streetfighterv.exe" and "binaries" in path:
            return int(entry.modBaseAddr or 0)
        ok = kernel32.Module32Next(snap, ctypes.byref(entry))
    return 0


# ---------------------------------------------------------------- Scaleform
DOCVIEW_VTABLE = 0x34C8568
STYLED_TEXT_VTABLE = 0x34C8530
DOCVIEW_TEXT = 0x10
DOCVIEW_LISTENER = 0x20
LISTENER_OWNER = -0x98
TEXT_PARAGRAPHS = 0x18
TEXT_PARAGRAPH_COUNT = 0x20
PARAGRAPH_CHARS = 0x00
PARAGRAPH_RUNS = 0x20
PARAGRAPH_RUN_COUNT = 0x28
DISPLAY_PARENT = 0x38
DISPLAY_RENDER_NODE = 0x48
DISPLAY_NAME = 0x70
DISPLAY_CHILDREN = 0xD8
DISPLAY_CHILD_COUNT = 0xE0
RENDER_NODE_DATA = 0x10
NODE_FLAGS = 0x0A
NODE_MATRIX = 0x10
NODE_CXFORM = 0x50
NODE_BOUNDS = 0x70
NODE_VISIBLE = 0x0001
MOVIE_ROOT_FLAGS = 0x1800
TWIPS = 20.0
MAX_PARAGRAPHS = 512
MAX_DEPTH = 40
GOLD = (1.0, 0.89, 0.549)


def compose(outer, inner):
    a, b = outer, inner
    return (a[0] * b[0] + a[1] * b[3], a[0] * b[1] + a[1] * b[4],
            a[0] * b[2] + a[1] * b[5] + a[2], a[3] * b[0] + a[4] * b[3],
            a[3] * b[1] + a[4] * b[4], a[3] * b[2] + a[4] * b[5] + a[5])


class Field:
    def __init__(self, text, x, y, tint, hidden, rooted, docview, chain):
        self.text, self.x, self.y, self.tint = text, x, y, tint
        self.hidden, self.rooted, self.docview, self.chain = hidden, rooted, docview, chain

    @property
    def gold(self):
        return all(abs(c - g) <= 0.05 for c, g in zip(self.tint, GOLD))

    @property
    def shown(self):
        return (not self.hidden and self.rooted and self.tint[3] > 0.01
                and 0 <= self.x < 1920 and 0 <= self.y < 1080 and self.text.strip()
                and not placeholder(self.text))


def placeholder(text: str) -> bool:
    letters = [c for c in text if not c.isspace()]
    if len(letters) >= 3 and set(letters) == {"w"}:
        return True
    if len(letters) < 8:
        return False
    return sum(c in "wW" for c in letters) >= 0.8 * len(letters)


class Reader:
    def __init__(self, mem: Mem, base: int) -> None:
        self.mem, self.base = mem, base

    def docviews(self, wide=False) -> list[int]:
        want = self.base + DOCVIEW_VTABLE
        found = []
        for addr, size in self.mem.regions(wide):
            raw = self.mem.read(addr, size)
            if not raw:
                continue
            n = len(raw) // 8
            for i in range(n):
                if struct.unpack_from("<Q", raw, i * 8)[0] == want:
                    found.append(addr + i * 8)
        return found

    def field_text(self, docview: int) -> str | None:
        styled = self.mem.u64(docview + DOCVIEW_TEXT)
        head = self.mem.read(styled, 0x28) if styled else None
        if not head or len(head) < 0x28:
            return None
        if struct.unpack_from("<Q", head)[0] != self.base + STYLED_TEXT_VTABLE:
            return None
        data, count = struct.unpack_from("<QQ", head, TEXT_PARAGRAPHS)
        if count > MAX_PARAGRAPHS or (count and not data):
            return None
        arr = self.mem.read(data, count * 8) if count else b""
        parts = []
        for i in range(count):
            para = struct.unpack_from("<Q", arr, i * 8)[0]
            if not para:
                continue
            head = self.mem.read(para, 0x30)
            if not head:
                continue
            chars, size = struct.unpack_from("<QQ", head, PARAGRAPH_CHARS)
            if not chars or not size or size > 4096:
                continue
            raw = self.mem.read(chars, size * 2)
            if not raw:
                continue
            text = raw.decode("utf-16-le", "replace")
            if text[size - 1:size] not in ("\0", "\r") or "\0" in text[: size - 1]:
                return None
            parts.append(text.rstrip("\0"))
        return "\n".join(p.rstrip("\r") for p in parts)

    def node(self, obj: int):
        links = self.mem.read(obj + DISPLAY_PARENT, DISPLAY_NAME + 8 - DISPLAY_PARENT)
        if not links or len(links) < DISPLAY_NAME + 8 - DISPLAY_PARENT:
            return None, None
        parent = struct.unpack_from("<Q", links)[0]
        entry = struct.unpack_from("<Q", links, DISPLAY_RENDER_NODE - DISPLAY_PARENT)[0]
        data = self.mem.u64(entry + RENDER_NODE_DATA) if entry else 0
        raw = self.mem.read(data, NODE_BOUNDS + 0x20) if data else None
        if not raw:
            return None, parent
        flags = struct.unpack_from("<H", raw, NODE_FLAGS)[0]
        m = struct.unpack_from("<8f", raw, NODE_MATRIX)
        cx = struct.unpack_from("<4f", raw, NODE_CXFORM)
        if not all(abs(v) < 1e7 for v in m + cx):
            return None, parent
        if cx[3] <= 0.01 and not any(struct.unpack_from("<8f", raw, NODE_BOUNDS)):
            cx = (cx[0], cx[1], cx[2], 1.0)
        return ((m[0], m[1], m[3], m[4], m[5], m[7]), cx, flags), parent

    def place(self, docview: int) -> Field | None:
        listener = self.mem.u64(docview + DOCVIEW_LISTENER)
        obj = self.mem.u64(listener + LISTENER_OWNER) if listener else 0
        if not obj:
            return None
        nodes, chain = [], []
        while obj and obj not in chain and len(chain) < MAX_DEPTH:
            node, parent = self.node(obj)
            if node is None:
                break
            nodes.append(node)
            chain.append(obj)
            obj = parent
        if not chain:
            return None
        world = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
        tint = [1.0, 1.0, 1.0, 1.0]
        for matrix, cx, _ in reversed(nodes):
            world = compose(world, matrix)
            tint = [t * c for t, c in zip(tint, cx)]
        hidden = any(not flags & NODE_VISIBLE for _m, _cx, flags in nodes[1:])
        rooted = nodes[-1][2] & MOVIE_ROOT_FLAGS == MOVIE_ROOT_FLAGS
        text = self.field_text(docview)
        if text is None:
            return None
        return Field(text, world[2] / TWIPS, world[5] / TWIPS, tuple(tint), hidden, rooted,
                     docview, tuple(chain))

    def items(self, wide=False) -> list[Field]:
        out = []
        for dv in self.docviews(wide):
            f = self.place(dv)
            if f is not None:
                out.append(f)
        return out


def reading_order(items: list[Field]) -> list[Field]:
    return sorted(items, key=lambda it: (round(it.y / 12), it.x))


def footer(items: list[Field]) -> Field | None:
    return next((it for it in items if it.y >= 960 and it.x < 400), None)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--watch", action="store_true")
    ap.add_argument("--wide", action="store_true")
    ap.add_argument("--all", action="store_true", help="include hidden and off-stage fields")
    args = ap.parse_args()

    pid = find_pid("StreetFighterV.exe")
    if not pid:
        print("StreetFighterV.exe is not running.")
        return 1
    mem = Mem(pid)
    base = module_base(pid)
    if not base:
        print("could not find the game module")
        return 1
    print(f"pid {pid}, base 0x{base:x}")
    reader = Reader(mem, base)

    last = None
    while True:
        started = time.perf_counter()
        fields = [f for f in reader.items(args.wide) if f.text.strip()]
        shown = [f for f in fields if f.shown] if not args.all else fields
        shown = reading_order(shown)
        selected = [f for f in shown if f.gold]
        foot = footer(shown)
        key = (tuple(f.text for f in selected), foot.text if foot else None)
        elapsed = (time.perf_counter() - started) * 1000
        if not args.watch or key != last:
            print(f"\n--- {len(fields)} fields, {len(shown)} shown, {elapsed:.0f} ms ---")
            for f in shown:
                mark = ">" if f.gold else " "
                state = "hidden" if f.hidden else ("loose" if not f.rooted else "")
                print(f"{mark} {f.x:7.0f},{f.y:7.0f}  tint={f.tint[0]:.2f},{f.tint[1]:.2f},{f.tint[2]:.2f}"
                      f"  {state:6s} {f.text!r}")
            print("SAY:", " | ".join([f.text for f in selected] + ([foot.text] if foot and foot.text.strip() else [])))
            last = key
        if not args.watch:
            return 0
        time.sleep(0.2)


if __name__ == "__main__":
    sys.exit(main() or 0)
