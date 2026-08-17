# -*- mode: python ; coding: utf-8 -*-
# 由 build.py 重新生成更可靠；此文件作为参考模板保留。
# 推荐：python build.py

import os
from pathlib import Path

try:
    import imageio_ffmpeg
    _ff = Path(imageio_ffmpeg.get_ffmpeg_exe())
    _ff_binaries = [(_ff.as_posix(), 'imageio_ffmpeg/binaries')] if _ff.is_file() else []
except Exception:
    _ff_binaries = []

block_cipher = None
here = Path(SPECPATH)

a = Analysis(
    [str(here / 'launcher.py')],
    pathex=[str(here)],
    binaries=_ff_binaries,
    datas=[
        (str(here / 'index.html'), '.'),
        (str(here / 'static'), 'static'),
        (str(here / 'server.py'), '.'),
    ],
    hiddenimports=['fastapi', 'uvicorn', 'multipart', 'imageio_ffmpeg', 'imageio_ffmpeg.binaries'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='STM_IPS_Host',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
