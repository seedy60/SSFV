"""Focus the game and send real keystrokes or clicks. For testing the narrator."""
import ctypes
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.FindWindowW.restype = wintypes.HWND
user32.SendInput.argtypes = (wintypes.UINT, ctypes.c_void_p, ctypes.c_int)


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


VK = {"enter": 0x0D, "esc": 0x1B, "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27,
      "space": 0x20, "tab": 0x09}


def window():
    return user32.FindWindowW(None, "StreetFighterV")


def focus():
    hwnd = window()
    foreground = user32.GetForegroundWindow()
    mine = ctypes.windll.kernel32.GetCurrentThreadId()
    other = user32.GetWindowThreadProcessId(foreground, None)
    user32.AttachThreadInput(mine, other, True)
    user32.SetForegroundWindow(hwnd)
    user32.BringWindowToTop(hwnd)
    user32.SetFocus(hwnd)
    user32.AttachThreadInput(mine, other, False)
    time.sleep(0.4)
    return user32.GetForegroundWindow() == hwnd


def key_down(vk):
    data = INPUT(type=1)
    data.ki.wVk = vk
    data.ki.wScan = user32.MapVirtualKeyW(vk, 0)
    user32.SendInput(1, ctypes.byref(data), ctypes.sizeof(INPUT))


def key_up(vk):
    data = INPUT(type=1)
    data.ki.wVk = vk
    data.ki.wScan = user32.MapVirtualKeyW(vk, 0)
    data.ki.dwFlags = 2
    user32.SendInput(1, ctypes.byref(data), ctypes.sizeof(INPUT))


def press(name, ctrl=False, shift=False):
    vk = VK[name] if name in VK else ord(name.upper())
    if ctrl:
        key_down(0x11)
    if shift:
        key_down(0x10)
    key_down(vk)
    time.sleep(0.05)
    key_up(vk)
    if shift:
        key_up(0x10)
    if ctrl:
        key_up(0x11)
    time.sleep(0.35)


def click(x, y, window_size=(1536, 864)):
    width, height = window_size
    px = int(x * 65535 / width)
    py = int(y * 65535 / height)
    for flags in (0x8001, 0x8002, 0x8004):
        data = INPUT(type=0)
        data.mi.dx = px
        data.mi.dy = py
        data.mi.dwFlags = flags
        user32.SendInput(1, ctypes.byref(data), ctypes.sizeof(INPUT))
        time.sleep(0.08)


if __name__ == "__main__":
    print("focused:", focus())
    for arg in sys.argv[1:]:
        parts = arg.split("+")
        name = parts[-1]
        print("press", arg)
        press(name, ctrl="ctrl" in parts, shift="shift" in parts)
