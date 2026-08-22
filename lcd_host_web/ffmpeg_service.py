"""FFmpeg process boundary used by conversion jobs."""

from __future__ import annotations

import os
import subprocess


NO_WINDOW = 0x08000000


def popen(command, *, context=None, **kwargs):
    """Start FFmpeg without a console and register it for cancellation."""
    if os.name == 'nt' and hasattr(subprocess, 'STARTUPINFO'):
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        kwargs.setdefault('startupinfo', startup)
        kwargs.setdefault('creationflags', NO_WINDOW)
    kwargs.pop('_low_priority', None)
    process = subprocess.Popen(command, **kwargs)
    if context is not None:
        context.register_process(process)
    return process
