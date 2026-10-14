"""Keep console windows of background tools' child processes hidden on Windows.

A helper launched detached (no console of its own: Start-Process, a scheduled task, an
agent's background shell) gives every console child it starts a new console. With Windows
Terminal as the default terminal, each of those opens a visible window that takes focus on
the owner's desktop: the offload client's queue polls start pwsh and ssh every few seconds.

install() makes subprocess.Popen add CREATE_NO_WINDOW to children while this process has no
console window. A process that has one keeps sharing it with its children, so their output
still reaches that console. Children that ask for their own console or none
(CREATE_NEW_CONSOLE, DETACHED_PROCESS) are left alone.
"""

from __future__ import annotations

import os
import subprocess

CREATE_NO_WINDOW = 0x08000000
CREATE_NEW_CONSOLE = 0x00000010
DETACHED_PROCESS = 0x00000008

_installed = False


def _has_console() -> bool:
    try:
        import ctypes
        return bool(ctypes.windll.kernel32.GetConsoleWindow())
    except (AttributeError, OSError):
        return True


def install() -> None:
    """Idempotent; a no-op outside Windows."""
    global _installed
    if os.name != "nt" or _installed:
        return
    _installed = True
    original = subprocess.Popen.__init__

    def __init__(self, *args, **kwargs):
        flags = kwargs.get("creationflags") or 0
        if not flags & (CREATE_NEW_CONSOLE | DETACHED_PROCESS) and not _has_console():
            kwargs["creationflags"] = flags | CREATE_NO_WINDOW
        original(self, *args, **kwargs)

    subprocess.Popen.__init__ = __init__
