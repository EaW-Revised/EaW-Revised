#!/usr/bin/env python3
"""Gate helper (runs ON the clean box, inside the virtual display): gives the game window the input focus and sends it an
Escape key press, which is the main menu's quit key, so the game is left through its own exit instead of a kill by `timeout`.
Plain python3 + libX11 through ctypes (XSendEvent; no xdotool or XTest needed). Prints how many windows got the key."""
from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys
import time

XK_ESCAPE = 0xFF1B


def main() -> int:
    x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XKeysymToKeycode.restype = ctypes.c_ubyte
    x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                               ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_uint)]
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
    x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    x11.XFlush.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(os.environ.get("DISPLAY", "").encode() or None)
    if not display:
        print("no display", file=sys.stderr)
        return 2
    root = x11.XDefaultRootWindow(display)
    keycode = x11.XKeysymToKeycode(display, XK_ESCAPE)
    root_return, parent_return = ctypes.c_ulong(), ctypes.c_ulong()
    children = ctypes.POINTER(ctypes.c_ulong)()
    count = ctypes.c_uint()
    x11.XQueryTree(display, root, ctypes.byref(root_return), ctypes.byref(parent_return), ctypes.byref(children), ctypes.byref(count))
    windows = [children[i] for i in range(count.value)]
    for window in windows:
        x11.XSetInputFocus(display, window, 2, 0)  # RevertToParent, CurrentTime
    x11.XFlush(display)
    time.sleep(1.0)
    for kind, mask in ((2, 1), (3, 2)):  # KeyPress/KeyPressMask, KeyRelease/KeyReleaseMask
        for window in windows:
            event = (ctypes.c_long * 24)()  # an XEvent is 24 longs; an XKeyEvent is laid out as below on x86_64
            event[0] = kind
            event[2] = 1                    # send_event
            event[4] = window
            event[5] = root
            event[10] = keycode << 32       # state 0, keycode
            event[11] = 1                   # same_screen
            x11.XSendEvent(display, window, 1, mask, ctypes.byref(event))
        x11.XFlush(display)
        time.sleep(0.5)
    print(f"sent Escape to {len(windows)} window(s)")
    return 0 if windows else 1


if __name__ == "__main__":
    sys.exit(main())
