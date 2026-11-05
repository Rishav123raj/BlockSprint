"""Prevent the machine from idle-sleeping while the recorder runs (Windows).

Uses ``SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)``, the mechanism media players
use: Windows will not go to sleep because of inactivity while the request is held, but the display
may still turn off. The request belongs to the calling thread and ends when that thread changes it
or exits, so it is released automatically if the recorder crashes.

It does not stop sleep that the user starts (Start > Sleep, closing the lid when the lid action is
"Sleep", pressing the power button) or shutdowns and update restarts.

On other platforms this is a no-op; on Linux use ``systemd-inhibit`` or a server that never sleeps.
"""

from __future__ import annotations

import sys

ES_CONTINUOUS = 0x80000000
ES_SYSTEM_REQUIRED = 0x00000001


def _set_state(flags: int) -> int:
    import ctypes

    return ctypes.windll.kernel32.SetThreadExecutionState(flags)


def supported() -> bool:
    return sys.platform == "win32"


def enable() -> bool:
    """Hold a "system required" request on the calling thread. Returns True if Windows accepted it."""
    if not supported():
        return False
    return _set_state(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0


def disable() -> None:
    """Release the request held by the calling thread."""
    if supported():
        _set_state(ES_CONTINUOUS)


class KeepAwake:
    """Context manager: ``with KeepAwake(): ...`` keeps the machine awake for the duration."""

    def __init__(self) -> None:
        self.active = False

    def __enter__(self) -> "KeepAwake":
        self.active = enable()
        return self

    def __exit__(self, *exc: object) -> None:
        if self.active:
            disable()
            self.active = False
