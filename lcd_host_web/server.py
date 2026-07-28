import io
import os
import json
import time
import uuid
import struct
import subprocess
import tempfile
import zipfile
from pathlib import Path
from threading import Timer, Lock, Thread

from flask import Flask, request, send_file, jsonify, make_response, abort

app = Flask(__name__, static_url_path='', static_folder='.')

ALLOWED_EXT = {'.png', '.jpg', '.jpeg', '.bmp', '.gif', '.tiff', '.webp', '.mp4', '.webm', '.mkv', '.avi', '.mov', '.flv', '.wmv'}
VIDEO_EXT = {'.mp4', '.webm', '.mkv', '.avi', '.mov', '.flv', '.wmv'}
def _find_ffmpeg():
    """Try system ffmpeg first, fall back to imageio-ffmpeg bundled binary.

    注意：imageio-ffmpeg 通常只带 ffmpeg，不带 ffprobe。
    无 ffprobe 时返回 None，调用方必须容错（时长等元数据可缺省）。
    """
    import shutil
    sys_ff = shutil.which('ffmpeg')
    sys_probe = shutil.which('ffprobe')
    if sys_ff and sys_probe:
        return sys_ff, sys_probe
    if sys_ff:
        # 仅有 ffmpeg 也可用转码；probe 稍后尝试
        pass
    try:
        import imageio_ffmpeg
        exe = Path(imageio_ffmpeg.get_ffmpeg_exe())
        if exe.is_file():
            # imageio 无独立 ffprobe；优先系统 ffprobe
            probe = sys_probe
            if not probe:
                # 同目录若用户自备了 ffprobe 也可
                cand = exe.parent / ('ffprobe.exe' if os.name == 'nt' else 'ffprobe')
                probe = str(cand) if cand.is_file() else None
            return str(exe), probe
    except Exception:
        pass
    if sys_ff:
        return sys_ff, sys_probe
    return 'ffmpeg', sys_probe

FFMPEG, FFPROBE = _find_ffmpeg()
_NO_WINDOW = 0x08000000  # CREATE_NO_WINDOW
# 同时只跑一个重转码任务（避免多任务抢 IO/显存）
_CONVERT_LOCK = Lock()
try:
    _CPU_COUNT = os.cpu_count() or 2
except Exception:
    _CPU_COUNT = 2
# 性能优先：多线程编解码/滤镜（上限 8，避免无意义线程爆炸）
FFMPEG_THREADS = max(2, min(8, _CPU_COUNT))
# 缩放质量：160x80 目标分辨率下 lanczos 成本很低，但边缘/文字明显更清晰
# （预览与烧录共用此缩放结果，改这里会同时影响两端画质）
SCALE_FLAGS = 'lanczos'
# GPU 硬解：启动时探测；可用环境变量 STM_IPS_HWACCEL=cuda|d3d11va|dxva2|qsv|off 覆盖
_HWACCEL = None  # None 表示纯 CPU
_HWACCEL_PROBED = False
# 最近一次实际转码是否用了硬解（供 /convert 回传）
_LAST_DECODE_ACCEL = 'cpu'
# 会话内最近一次成功的硬解后端（加速后续文件选型）
_LAST_GOOD_ACCEL = None
# 按文件属性缓存选型结果，避免同文件重复 file-probe
_HW_FILE_CACHE = {}


def _ffmpeg_common_args():
    """共享 ffmpeg 线程参数。"""
    return [
        '-threads', str(FFMPEG_THREADS),
        '-filter_threads', str(FFMPEG_THREADS),
        '-filter_complex_threads', str(FFMPEG_THREADS),
    ]


def _hw_success_markers(accel: str):
    """ffmpeg verbose 日志中表示“硬解真的起来了”的标记。

    注意：硬解失败时 ffmpeg 常静默回退 CPU 且 exit code=0，
    所以不能只看返回码，必须看日志。
    """
    a = (accel or '').lower()
    if a == 'cuda':
        return ('pix_fmt: cuda', 'NVDEC capabilities', 'h264_cuvid', 'hevc_cuvid')
    if a == 'd3d11va':
        return ('pix_fmt: d3d11', 'd3d11va')
    if a == 'dxva2':
        return ('pix_fmt: dxva2_vld', 'DXVA2')
    if a == 'qsv':
        return ('pix_fmt: qsv', 'qsv')
    return ()


def _hw_fail_markers():
    return (
        'Failed setup for format',
        'hwaccel initialisation returned error',
        'does not support the requested hwaccel',
        'Device creation failed',
        'No device available',
    )


def _probe_hwaccel_once(accel: str, sample: Path) -> bool:
    """对指定 accel 做真实硬解探测（verbose 日志校验）。"""
    cmd = [
        FFMPEG, '-hide_banner', '-v', 'verbose', '-nostdin',
        '-hwaccel', accel,
        '-i', str(sample),
        '-an', '-sn', '-dn',
        '-frames:v', '5',
        '-f', 'null', '-',
    ]
    try:
        r = _run_silent(cmd, capture_output=True, timeout=25, check=False, _low_priority=False)
    except Exception as e:
        print(f'[hwaccel] probe {accel} exception: {e}')
        return False
    err = (r.stderr or b'')
    if isinstance(err, bytes):
        text = err.decode('utf-8', errors='replace')
    else:
        text = str(err)
    # 失败标记优先
    for m in _hw_fail_markers():
        if m in text:
            print(f'[hwaccel] probe {accel} fail marker: {m}')
            return False
    if r.returncode != 0:
        print(f'[hwaccel] probe {accel} exit={r.returncode}')
        return False
    for m in _hw_success_markers(accel):
        if m in text:
            print(f'[hwaccel] probe {accel} OK ({m})')
            return True
    # 没看到成功标记：视为未真正硬解
    print(f'[hwaccel] probe {accel} exit=0 but no HW marker (likely silent CPU fallback)')
    return False


def _detect_hwaccel():
    """探测可用硬件解码。优先 NVIDIA CUDA，其次 Windows D3D11VA。"""
    global _HWACCEL, _HWACCEL_PROBED
    if _HWACCEL_PROBED:
        return _HWACCEL
    _HWACCEL_PROBED = True

    override = (os.environ.get('STM_IPS_HWACCEL') or '').strip().lower()
    if override in ('0', 'off', 'none', 'cpu', 'false'):
        _HWACCEL = None
        print('[hwaccel] disabled by STM_IPS_HWACCEL')
        return None

    # 强制指定时也要做一次真实探测；失败则回退继续自动选
    forced = None
    if override in ('cuda', 'd3d11va', 'dxva2', 'qsv', 'vulkan', 'opencl'):
        forced = override

    candidates = []
    if forced:
        candidates.append(forced)
    else:
        try:
            import shutil
            if shutil.which('nvidia-smi'):
                candidates.append('cuda')
        except Exception:
            pass
        if os.name == 'nt':
            candidates.extend(['d3d11va', 'dxva2'])
        # qsv 在本机探测常失败，放最后
        candidates.append('qsv')
    # 去重保序
    seen = set()
    candidates = [c for c in candidates if not (c in seen or seen.add(c))]

    # 用 720p 样片更接近真实视频；极小分辨率有时探测不稳定
    sample = TMP / '_hw_probe.mp4'
    try:
        TMP.mkdir(parents=True, exist_ok=True)
        _run_silent(
            [FFMPEG, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y',
             '-f', 'lavfi', '-i', 'testsrc=size=1280x720:rate=30:duration=0.5',
             '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-t', '0.5', str(sample)],
            capture_output=True, timeout=40, check=True,
            _low_priority=False,
        )
    except Exception as e:
        print(f'[hwaccel] probe sample create failed: {e}')
        _HWACCEL = None
        return None

    for accel in candidates:
        if _probe_hwaccel_once(accel, sample):
            _HWACCEL = accel
            print(f'[hwaccel] enabled: {accel}')
            break
    else:
        _HWACCEL = None
        print('[hwaccel] none available, using CPU decode')

    try:
        sample.unlink(missing_ok=True)
    except Exception:
        pass
    return _HWACCEL


def _ffmpeg_hwaccel_prefix(accel=None):
    """解码侧 GPU 加速参数。

    实测本机：
      - 仅 `-hwaccel cuda` 可稳定 NVDEC
      - `-hwaccel_output_format cuda` / `-extra_hw_frames` 反而容易 init fail 并静默回退
    因此这里只用最稳的 plain `-hwaccel`。
    """
    if accel is None:
        accel = _detect_hwaccel()
    if not accel:
        return []
    # cuda 指定 device 0，避免笔记本独显/核显切换时选错设备
    if accel == 'cuda':
        return ['-hwaccel', 'cuda', '-hwaccel_device', '0']
    if accel == 'd3d11va':
        return ['-hwaccel', 'd3d11va']
    return ['-hwaccel', accel]


def _hw_try_order():
    """实际转码时的尝试顺序：探测首选 + 其它可用后备 + CPU。"""
    primary = _detect_hwaccel()
    order = []
    if primary:
        order.append(primary)
    # 即使 primary 是 cuda，也保留 d3d11va 作为后备（部分片源 NVDEC 不吃）
    for extra in ('cuda', 'd3d11va', 'dxva2'):
        if extra not in order:
            # 只追加“探测阶段验证过”或与 primary 同类的；避免盲试拖时间
            # 这里允许再试一次：代价是失败会 empty/秒退
            if primary is None or extra != primary:
                # 对 primary=cuda 时额外试 d3d11va；primary=None 时按序全试
                if primary is None or (primary == 'cuda' and extra == 'd3d11va') or (primary == 'd3d11va' and extra == 'cuda'):
                    order.append(extra)
    order.append(None)  # CPU
    # 去重保序
    seen = set()
    out = []
    for x in order:
        key = x if x is not None else 'cpu'
        if key not in seen:
            seen.add(key)
            out.append(x)
    return out


def _file_cache_key(in_path: str):
    try:
        st = os.stat(in_path)
        return (str(in_path), int(st.st_size), int(st.st_mtime))
    except OSError:
        return (str(in_path), 0, 0)


def _file_hwaccel_works(in_path: str, accel: str, frames: int = 2) -> bool:
    """对【当前输入文件】做硬解冒烟测试（尽量短：默认 2 帧）。

    全局探测只能证明“机器支持该后端”，不能保证每个片源都能 NVDEC。
    实测：部分 H.264 会 CUDA_ERROR_INVALID_VALUE 并静默软解。
    """
    if not accel:
        return True
    cmd = [FFMPEG, '-hide_banner', '-v', 'verbose', '-nostdin']
    cmd += _ffmpeg_hwaccel_prefix(accel)
    cmd += [
        '-i', str(in_path),
        '-an', '-sn', '-dn',
        '-frames:v', str(max(1, frames)),
        '-f', 'null', '-',
    ]
    try:
        r = _run_silent(cmd, capture_output=True, timeout=40, check=False, _low_priority=False)
    except Exception as e:
        print(f'[hwaccel] file-probe {accel} exception: {e}')
        return False
    err = r.stderr or b''
    text = err.decode('utf-8', errors='replace') if isinstance(err, (bytes, bytearray)) else str(err)
    hit_fails = [m for m in _hw_fail_markers() if m in text]
    if hit_fails:
        # 只摘真正的错误行，不要匹配普通 "requested hwaccel method xxx"
        line = ''
        for ln in text.splitlines():
            low = ln.lower()
            if ('failed setup' in low or 'cuda_error' in low or 'initialisation returned error' in low
                    or 'device creation failed' in low or 'no device available' in low
                    or 'does not support the requested hwaccel' in low):
                line = ln.strip()
                break
        print(f'[hwaccel] file-probe {accel} FAIL on this file: marker={hit_fails[0]!r} line={line or text[:200]!r}')
        return False
    if r.returncode != 0:
        print(f'[hwaccel] file-probe {accel} exit={r.returncode}')
        return False
    # 成功标记：pix_fmt: cuda / d3d11 等
    if any(m in text for m in _hw_success_markers(accel)):
        print(f'[hwaccel] file-probe {accel} OK on this file')
        return True
    print(f'[hwaccel] file-probe {accel} exit=0 but no HW marker (silent soft decode?)')
    return False


def _select_hwaccel_for_file(in_path: str):
    """为当前文件选择硬解后端；全部失败返回 None（CPU）。带缓存。"""
    global _LAST_GOOD_ACCEL
    key = _file_cache_key(in_path)
    if key in _HW_FILE_CACHE:
        cached = _HW_FILE_CACHE[key]
        print(f'[hwaccel] select(cache): {cached or "cpu"}')
        return cached

    # 优先试：上次成功后端 → 全局探测顺序
    order = []
    if _LAST_GOOD_ACCEL:
        order.append(_LAST_GOOD_ACCEL)
    for a in _hw_try_order():
        if a not in order:
            order.append(a)

    for accel in order:
        if accel is None:
            print('[hwaccel] select: cpu (no HW backend left)')
            _HW_FILE_CACHE[key] = None
            return None
        if _file_hwaccel_works(in_path, accel):
            print(f'[hwaccel] select: {accel}')
            _LAST_GOOD_ACCEL = accel
            _HW_FILE_CACHE[key] = accel
            # 缓存别无限涨
            if len(_HW_FILE_CACHE) > 64:
                try:
                    _HW_FILE_CACHE.pop(next(iter(_HW_FILE_CACHE)))
                except Exception:
                    _HW_FILE_CACHE.clear()
            return accel
    print('[hwaccel] select: cpu')
    _HW_FILE_CACHE[key] = None
    return None


def _build_vf(width: int, height: int, fps: float = 0) -> str:
    if fps and fps > 0:
        return f'fps={fps},scale={width}:{height}:flags={SCALE_FLAGS}'
    return f'scale={width}:{height}:flags={SCALE_FLAGS}'


def _run_silent(*args, **kwargs):
    """Run subprocess without showing a console window."""
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    kwargs.setdefault('startupinfo', si)
    kwargs.setdefault('creationflags', _NO_WINDOW)
    kwargs.pop('_low_priority', None)  # 兼容旧调用，忽略
    return subprocess.run(*args, **kwargs)


def _popen_silent(*args, **kwargs):
    """Popen subprocess without showing a console window."""
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    kwargs.setdefault('startupinfo', si)
    kwargs.setdefault('creationflags', _NO_WINDOW)
    kwargs.pop('_low_priority', None)
    return subprocess.Popen(*args, **kwargs)


HERE = Path(__file__).parent
# 注意：不截断视频帧。此阈值仅用于“JSON 内联 hex 的安全上限”，超大结果走 download_id 二进制。
INLINE_HEX_MAX_BYTES = 256 * 1024
DOWNLOAD_TTL = 300        # seconds before temp files are cleaned

TMP = Path(tempfile.gettempdir()) / 'stm_ips_dl'
TMP.mkdir(parents=True, exist_ok=True)

# download_id -> {path, name, mtime, frame_count, width, height, frame_size, fps}
_DL_REG = {}
_DL_TIMER = None


def _dl_cleanup():
    now = time.time()
    dead = [k for k, v in _DL_REG.items() if now - v['mtime'] > DOWNLOAD_TTL]
    for k in dead:
        ent = _DL_REG.pop(k, None)
        if ent:
            try:
                os.unlink(ent['path'])
            except OSError:
                pass
    if _DL_REG:
        _schedule_cleanup()


def _schedule_cleanup():
    global _DL_TIMER
    if _DL_TIMER:
        _DL_TIMER.cancel()
    _DL_TIMER = Timer(DOWNLOAD_TTL + 10, _dl_cleanup)
    _DL_TIMER.daemon = True
    _DL_TIMER.start()


# ----- block compression (4x4 block, 2-color + bitmap) -----------------------
# Format:
#   [MAGIC "BL" 2B] [VER 1B] [QUALITY 1B] [BLOCK_SIZE(=4) 1B] [unused 1B]
#   Block data (6B per 4x4 block):
#     color0_hi, color0_lo (RGB565 big-endian)
#     color1_hi, color1_lo (RGB565 big-endian)
#     bitmap_hi, bitmap_lo   (16 bits, MSB=pixel#0)
#   For each pixel: bitmap_bit=0 -> color0, =1 -> color1

BL_MAGIC = b'BL'
BL_VERSION = 1
BLOCK_SIZE = 4  # 4x4 pixels per block

# quality -> (lvl, bw, bh, npix, idx_bytes, rm, gm, bm)
# 4-color block: 2 base + 2 interpolated, 2-bit index/pixel
_BLK_CFG = {
    10: (1, 2, 2, 4,  1, 0x1F, 0x3F, 0x1F),
     9: (4, 4, 2, 8,  2, 0x1F, 0x3F, 0x1F),
     7: (2, 4, 4, 16, 4, 0x1F, 0x3F, 0x1F),
}

_LVL_TO_PROFILE = {
    0: (0, 0, 0, 0),     # raw passthrough
    4: (4, 2, 8,  2),    # 8px x 2bit = 2B indices
    1: (2, 2, 4,  1),    # 4px x 2bit = 1B indices
    2: (4, 4, 16, 4),    # 16px x 2bit = 4B indices
    3: (8, 4, 32, 8),    # 32px x 2bit = 8B indices
}


def _compress_block(rgb565: bytes, w: int, h: int, quality: int) -> bytes:
    """Moderate speedup: preallocate, reuse buffers, fewer Python list ops."""
    _, bw, bh, npix, idx_bytes, rm, gm, bm = _BLK_CFG[quality]
    bxc = (w + bw - 1) // bw
    byc = (h + bh - 1) // bh
    blk_bytes = 4 + idx_bytes
    out = bytearray(bxc * byc * blk_bytes)
    oi = 0

    r5s = [0] * npix
    g6s = [0] * npix
    b5s = [0] * npix
    bright = [0] * npix
    get = rgb565.__getitem__

    for by in range(byc):
        y0 = by * bh
        for bx in range(bxc):
            x0 = bx * bw
            pi = 0
            for py in range(bh):
                iy = y0 + py
                row_ok = iy < h
                row_base = iy * w
                for px in range(bw):
                    ix = x0 + px
                    if row_ok and ix < w:
                        off = (row_base + ix) * 2
                        val = (get(off) << 8) | get(off + 1)
                        r5s[pi] = (val >> 11) & 0x1F
                        g6s[pi] = (val >> 5) & 0x3F
                        b5s[pi] = val & 0x1F
                    else:
                        r5s[pi] = 0
                        g6s[pi] = 0
                        b5s[pi] = 0
                    pi += 1

            rsum = gsum = bsum = 0
            for i in range(npix):
                r = r5s[i]; g = g6s[i]; b = b5s[i]
                rsum += r; gsum += g; bsum += b
                bright[i] = r * 19595 + g * 38470 + b * 7471
            med = sorted(bright)[npix // 2]

            c0n = c1n = 0
            c0r = c0g = c0b = 0
            c1r = c1g = c1b = 0
            for i in range(npix):
                if bright[i] < med:
                    c0n += 1
                    c0r += r5s[i]; c0g += g6s[i]; c0b += b5s[i]
                else:
                    c1n += 1
                    c1r += r5s[i]; c1g += g6s[i]; c1b += b5s[i]

            fbr, fbg, fbb = rsum // npix, gsum // npix, bsum // npix
            if c0n:
                c0r //= c0n; c0g //= c0n; c0b //= c0n
            else:
                c0r, c0g, c0b = fbr, fbg, fbb
            if c1n:
                c1r //= c1n; c1g //= c1n; c1b //= c1n
            else:
                c1r, c1g, c1b = fbr, fbg, fbb

            c0r &= rm; c0g &= gm; c0b &= bm
            c1r &= rm; c1g &= gm; c1b &= bm

            c2r = (c0r * 2 + c1r) // 3
            c2g = (c0g * 2 + c1g) // 3
            c2b = (c0b * 2 + c1b) // 3
            c3r = (c0r + c1r * 2) // 3
            c3g = (c0g + c1g * 2) // 3
            c3b = (c0b + c1b * 2) // 3

            c0v = (c0r << 11) | (c0g << 5) | c0b
            c1v = (c1r << 11) | (c1g << 5) | c1b
            out[oi] = (c0v >> 8) & 0xFF
            out[oi + 1] = c0v & 0xFF
            out[oi + 2] = (c1v >> 8) & 0xFF
            out[oi + 3] = c1v & 0xFF
            oi += 4

            indices = 0
            for i in range(npix):
                pr, pg, pb = r5s[i], g6s[i], b5s[i]
                d0 = (pr - c0r) * (pr - c0r) + (pg - c0g) * (pg - c0g) + (pb - c0b) * (pb - c0b)
                d1 = (pr - c1r) * (pr - c1r) + (pg - c1g) * (pg - c1g) + (pb - c1b) * (pb - c1b)
                d2 = (pr - c2r) * (pr - c2r) + (pg - c2g) * (pg - c2g) + (pb - c2b) * (pb - c2b)
                d3 = (pr - c3r) * (pr - c3r) + (pg - c3g) * (pg - c3g) + (pb - c3b) * (pb - c3b)
                best_i = 0
                best_d = d0
                if d1 < best_d:
                    best_d, best_i = d1, 1
                if d2 < best_d:
                    best_d, best_i = d2, 2
                if d3 < best_d:
                    best_i = 3
                indices = (indices << 2) | best_i

            shift = 8 * (idx_bytes - 1)
            for _ in range(idx_bytes):
                out[oi] = (indices >> shift) & 0xFF
                oi += 1
                shift -= 8
    return bytes(out)


def _decompress_block(data: bytes, w: int, h: int, lvl: int) -> bytes:
    if lvl == 0:
        return data  # raw passthrough
    bw, bh, npix, idx_bytes = _LVL_TO_PROFILE[lvl]
    blk_bytes = 4 + idx_bytes
    bxc = (w + bw - 1) // bw
    byc = (h + bh - 1) // bh
    out = bytearray(w * h * 2)
    off = 0
    for by in range(byc):
        for bx in range(bxc):
            c0h, c0l = data[off], data[off+1]
            c1h, c1l = data[off+2], data[off+3]
            # read base colours as 16-bit
            def _lerp(a, b, n, d): return (a*(d-n)+b*n)//d
            c0v = (c0h << 8) | c0l; c1v = (c1h << 8) | c1l
            c0r = (c0v>>11)&0x1F; c0g = (c0v>>5)&0x3F; c0b = c0v&0x1F
            c1r = (c1v>>11)&0x1F; c1g = (c1v>>5)&0x3F; c1b = c1v&0x1F
            c2v = ((_lerp(c0r,c1r,1,3)<<11)|(_lerp(c0g,c1g,1,3)<<5)|_lerp(c0b,c1b,1,3))
            c3v = ((_lerp(c0r,c1r,2,3)<<11)|(_lerp(c0g,c1g,2,3)<<5)|_lerp(c0b,c1b,2,3))
            c2h = (c2v>>8)&0xFF; c2l = c2v&0xFF
            c3h = (c3v>>8)&0xFF; c3l = c3v&0xFF
            cols = [(c0h,c0l),(c1h,c1l),(c2h,c2l),(c3h,c3l)]

            indices = 0
            for b in range(idx_bytes):
                indices = (indices << 8) | data[off+4+b]
            off += blk_bytes
            for pi in range(npix):
                px = bx * bw + (pi % bw)
                py = by * bh + (pi // bw)
                if px >= w or py >= h: continue
                ci = (indices >> (2*(npix-1-pi))) & 3
                out[(py*w+px)*2], out[(py*w+px)*2+1] = cols[ci][0], cols[ci][1]
    return bytes(out)


def compress_frame(rgb565_data: bytes, width: int, height: int,
                   quality: int) -> bytes:
    if quality >= 11:
        header = BL_MAGIC + bytes([BL_VERSION, quality, 0, 0])
        return header + rgb565_data
    lvl = _BLK_CFG[quality][0]
    compressed = _compress_block(rgb565_data, width, height, quality)
    header = BL_MAGIC + bytes([BL_VERSION, quality, lvl, 0])
    return header + compressed


def decompress_frame(compressed: bytes, pixel_count: int) -> bytes:
    if compressed[:2] != BL_MAGIC:
        raise ValueError('Not BL')
    lvl = compressed[4]
    block_data = compressed[6:]
    import math
    area = pixel_count
    for gw, gh in [(160, 80), (80, 160), (320, 240), (240, 320),
                   (128, 128), (64, 64), (32, 32), (16, 16)]:
        if gw * gh == area:
            return _decompress_block(block_data, gw, gh, lvl)
    guess_w = int(math.isqrt(area))
    while area % guess_w != 0:
        guess_w -= 1
    return _decompress_block(block_data, guess_w, area // guess_w, lvl)


def _check_ffmpeg():
    try:
        _run_silent([FFMPEG, '-version'], capture_output=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        raise RuntimeError(
            'ffmpeg 未找到，请安装 ffmpeg 并加入 PATH。\n'
            '下载: https://ffmpeg.org/download.html'
        )


_check_ffmpeg()


def _to_be(data: bytes) -> bytes:
    """Convert RGB565 from little-endian to big-endian (byte-swap each pixel)."""
    out = bytearray(len(data))
    for i in range(0, len(data), 2):
        out[i] = data[i + 1]
        out[i + 1] = data[i]
    return bytes(out)


# ----- helpers ---------------------------------------------------------------

def _probe(path: str) -> dict:
    """可选元数据探测；无 ffprobe 时返回空默认值。"""
    if not FFPROBE:
        return {'fps': 0, 'nb_frames': 0}
    cmd = [FFPROBE, '-v', 'error', '-select_streams', 'v:0',
           '-show_entries', 'stream=nb_frames,r_frame_rate,avg_frame_rate',
           '-of', 'json', path]
    r = _run_silent(cmd, capture_output=True, text=True, check=True)
    data = json.loads(r.stdout)
    s = data.get('streams', [{}])[0]

    def _parse_fps(val: str) -> float:
        if '/' in val:
            n, d = val.split('/')
            return float(n) / float(d) if float(d) else 0
        return float(val)

    fps = _parse_fps(s.get('r_frame_rate', '0/1'))
    if fps <= 0:
        fps = _parse_fps(s.get('avg_frame_rate', '0/1'))
    fps = max(fps, 1)  # minimum 1 fps

    nf = int(s.get('nb_frames', 0))
    if nf == 0:
        cmd2 = [FFPROBE, '-v', 'error', '-select_streams', 'v:0',
                '-count_frames', '-show_entries', 'stream=nb_read_frames',
                '-of', 'csv=p=0', path]
        r2 = _run_silent(cmd2, capture_output=True, text=True)
        try:
            nf = int(r2.stdout.strip())
        except ValueError:
            nf = 0
    return {'fps': fps, 'nb_frames': nf}


def _stream_frames(in_path: str, width: int, height: int,
                   fps: float = 0, vframes: int = 0):
    """Yield (rgb_bytes) for each frame via ffmpeg raw RGB24 pipe.

    先对【当前文件】选择可用硬解后端，再正式抽帧；避免 silent soft-fallback。
    """
    global _LAST_DECODE_ACCEL
    vf = _build_vf(width, height, fps)
    frame_bytes = width * height * 3
    # 按当前片源选后端：cuda 失败则 d3d11va，再失败才 cpu
    selected = _select_hwaccel_for_file(in_path)
    modes = [selected] if selected is not None else [None]
    # 若选定的是 GPU，仍允许失败后 CPU 兜底（但不会在 GPU silent soft 时误收）
    if selected is not None:
        modes.append(None)
    print(f'[hwaccel] raw selected={selected or "cpu"} order={[m or "cpu" for m in modes]} ffmpeg={FFMPEG}')

    def _run_pipe(accel_name):
        cmd = [FFMPEG, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y']
        if accel_name:
            cmd += _ffmpeg_hwaccel_prefix(accel_name)
        cmd += ['-i', in_path]
        cmd += _ffmpeg_common_args()
        cmd += ['-an', '-sn', '-dn',
                '-vf', vf,
                '-f', 'rawvideo', '-pix_fmt', 'rgb24']
        if vframes > 0:
            cmd += ['-frames:v', str(vframes)]
        cmd.append('pipe:1')
        print(f'[hwaccel] raw spawn accel={accel_name or "cpu"}')
        return _popen_silent(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    for accel_name in modes:
        proc = _run_pipe(accel_name)
        err_bucket = []
        t = Thread(target=_drain_stderr, args=(proc, err_bucket), daemon=True)
        t.start()
        got = 0
        try:
            while True:
                data = proc.stdout.read(frame_bytes)
                if not data or len(data) < frame_bytes:
                    break
                got += 1
                yield data, width, height
        finally:
            try:
                if proc.poll() is None:
                    proc.wait(timeout=5)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
            t.join(timeout=3)

        text = _stderr_text(err_bucket)
        if got > 0:
            # 已经选定并通过 file-probe 的后端，正式转码按请求的 accel 记账；
            # 若仍出现 fail marker，说明运行期异常，标记 cpu 但数据仍可用。
            if accel_name and not any(m in text for m in _hw_fail_markers()):
                _LAST_DECODE_ACCEL = accel_name
                print(f'[hwaccel] stream_frames used {accel_name}, frames={got}')
            else:
                _LAST_DECODE_ACCEL = 'cpu' if not accel_name else accel_name
                # 若请求 GPU 且出现 fail marker，仍报告 cpu（诚实）
                if accel_name and any(m in text for m in _hw_fail_markers()):
                    _LAST_DECODE_ACCEL = 'cpu'
                    print(f'[hwaccel] stream_frames requested {accel_name} but fail-marker; frames={got}')
                else:
                    print(f'[hwaccel] stream_frames used {_LAST_DECODE_ACCEL}, frames={got}')
            return
        if accel_name:
            print(f'[hwaccel] stream_frames empty with {accel_name}, try next; err={(text or "")[:180]!r}')
            continue
    _LAST_DECODE_ACCEL = 'cpu'


def _rgb_to_rgb565(r: int, g: int, b: int, endian: str = '>',
                   brightness: float = 100.0) -> bytes:
    if brightness != 100.0:
        ratio = brightness / 100.0
        r = min(255, max(0, round(r * ratio)))
        g = min(255, max(0, round(g * ratio)))
        b = min(255, max(0, round(b * ratio)))
    p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return struct.pack(f'{endian}H', p)


def _convert_to_rgb565(rgb_pixels: bytes, w: int, h: int,
                       endian: str = '>', brightness: float = 100.0) -> bytes:
    """Convert packed RGB888 -> RGB565 with pre-sized buffer (avoid O(n) realloc)."""
    n = len(rgb_pixels) // 3
    out = bytearray(n * 2)
    get = rgb_pixels.__getitem__
    be = (endian != '<')
    oi = 0
    if brightness == 100.0:
        for i in range(0, n * 3, 3):
            r = get(i)
            g = get(i + 1)
            b = get(i + 2)
            p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            if be:
                out[oi] = p >> 8
                out[oi + 1] = p & 0xFF
            else:
                out[oi] = p & 0xFF
                out[oi + 1] = p >> 8
            oi += 2
    else:
        rq = int(brightness * 2.56 + 0.5)
        for i in range(0, n * 3, 3):
            r = (get(i) * rq) >> 8
            g = (get(i + 1) * rq) >> 8
            b = (get(i + 2) * rq) >> 8
            if r > 255: r = 255
            if g > 255: g = 255
            if b > 255: b = 255
            p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            if be:
                out[oi] = p >> 8
                out[oi + 1] = p & 0xFF
            else:
                out[oi] = p & 0xFF
                out[oi + 1] = p >> 8
            oi += 2
    return bytes(out)


def _attach_payload_fields(result: dict, compressed: bytes = b'', preview_raw: bytes = b'',
                           compressed_size: int = None) -> dict:
    """Attach download/preview fields. Never put multi-MB hex into JSON (browser OOM)."""
    result = dict(result)
    # 仅给前端缩略图用的少量预览；完整视频预览由前端对 compressed 按需解码
    result['preview_hex'] = preview_raw.hex() if preview_raw else ''
    size = compressed_size if compressed_size is not None else len(compressed or b'')
    result['compressed_bytes'] = size
    if compressed and size <= INLINE_HEX_MAX_BYTES and len(compressed) == size:
        result['compressed_hex'] = compressed.hex()
        result['inline_omitted'] = False
    else:
        # 大结果：只走 download_id 二进制，JSON 不带 hex（避免 ~2x 膨胀打爆浏览器）
        result['compressed_hex'] = ''
        result['inline_omitted'] = True
    return result


def _save_temp(name: str, data: bytes,
               width: int, height: int, fcount: int,
               fps: float) -> str:
    did = uuid.uuid4().hex
    path = TMP / did
    path.write_bytes(data)
    _DL_REG[did] = {
        'path': str(path), 'name': name,
        'mtime': time.time(),
        'width': width, 'height': height,
        'frame_count': fcount, 'frame_size': width * height * 2,
        'fps': fps,
    }
    _schedule_cleanup()
    return did


# ----- endpoints -------------------------------------------------------------

@app.after_request
def add_cors(resp):
    resp.headers['Access-Control-Allow-Origin'] = '*'
    resp.headers['Access-Control-Allow-Headers'] = '*'
    resp.headers['Access-Control-Allow-Methods'] = 'GET,POST,OPTIONS'
    return resp


@app.route('/convert', methods=['POST', 'OPTIONS'])
def convert():
    if request.method == 'OPTIONS':
        return make_response()
    if 'file' not in request.files:
        return jsonify({'error': 'No file uploaded'}), 400

    file = request.files['file']
    if not file.filename:
        return jsonify({'error': 'Empty filename'}), 400

    try:
        width = int(request.form.get('width', 160))
        height = int(request.form.get('height', 80))
        fps = max(1, min(60, float(request.form.get('fps', 30))))
        swap = request.form.get('swap', '0') == '1'
        brightness = max(10, min(300, float(request.form.get('brightness', 100))))
        codec = request.form.get('codec', 'mjpeg').strip().lower()
        if codec == 'mjpeg':
            quality = max(1, min(100, int(request.form.get('quality', 70))))
        else:
            quality = max(7, min(11, int(request.form.get('quality', 7))))
    except ValueError:
        return jsonify({'error': 'Invalid parameters'}), 400

    if not (1 <= width <= 1024) or not (1 <= height <= 1024):
        return jsonify({'error': 'Dimensions out of range (1-1024)'}), 400
    ext = Path(file.filename).suffix.lower()
    is_video = ext in VIDEO_EXT
    if ext not in ALLOWED_EXT:
        return jsonify({'error': f'Unsupported file type: {ext}'}), 400

    # 串行化重转码，避免多请求叠加把 CPU 打满
    if not _CONVERT_LOCK.acquire(blocking=False):
        return jsonify({'error': '已有转换任务进行中，请稍后再试'}), 429

    global _LAST_DECODE_ACCEL
    _LAST_DECODE_ACCEL = 'cpu'

    body = file.read()

    with tempfile.NamedTemporaryFile(delete=False, suffix=ext) as fin:
        fin.write(body)
        in_path = fin.name

    try:
        # 确保启动后至少探测过一次
        _detect_hwaccel()
        endian = '<' if swap else '>'
        if is_video:
            if codec == 'mjpeg':
                result = _process_video_mjpeg(in_path, width, height, fps, quality)
            else:
                result = _process_video(in_path, width, height, fps, endian, brightness, quality)
        else:
            if codec == 'mjpeg':
                result = _process_image_mjpeg(in_path, width, height, quality)
            else:
                result = _process_image(in_path, width, height, endian, brightness, quality)
    except subprocess.CalledProcessError as e:
        msg = e.stderr.decode('utf-8', errors='replace')[-300:] if e.stderr else str(e)
        return jsonify({'error': f'ffmpeg error: {msg}'}), 500
    except Exception as e:
        return jsonify({'error': str(e)}), 500
    finally:
        _CONVERT_LOCK.release()
        try:
            os.unlink(in_path)
        except OSError:
            pass

    result['original_name'] = file.filename
    result['hex'] = result.pop('preview_hex', '')
    if codec != 'mjpeg':
        result['endian'] = 'little' if swap else 'big'
    result['ffmpeg_threads'] = FFMPEG_THREADS
    # 报告“实际用了什么”，不是“探测到什么”
    result['hwaccel'] = _LAST_DECODE_ACCEL or (_detect_hwaccel() or 'cpu')
    result['hwaccel_available'] = _detect_hwaccel() or 'cpu'
    return jsonify(result)


def _process_image(in_path: str, width: int, height: int,
                   endian: str = '>', brightness: float = 100.0, quality: int = 8) -> dict:
    gen = _stream_frames(in_path, width, height, vframes=1)
    try:
        rgb, fw, fh = next(gen)
    except StopIteration:
        raise RuntimeError('ffmpeg produced no output')

    data = _convert_to_rgb565(rgb, fw, fh, endian, brightness)

    # compression always uses big-endian (MCU native)
    comp_data = _to_be(data) if endian == '<' else data
    compressed = compress_frame(comp_data, fw, fh, quality)

    # 单图也落盘，方便统一 download_id 路径
    name = Path(in_path).stem
    did = _save_temp(name, compressed, fw, fh, 1, 30)

    result = {
        'type': 'image',
        'download_id': did,
        'width': fw,
        'height': fh,
        'frame_count': 1,
        'frame_size': len(data),
        'fps': 30,
        'quality': quality,
        'codec': 'bl',
    }
    return _attach_payload_fields(result, compressed, data)


def _drain_stderr(proc, bucket: list):
    """后台吸干 stderr，避免 verbose 日志塞满管道导致 ffmpeg 卡死。"""
    try:
        data = proc.stderr.read() if proc.stderr else b''
        bucket.append(data if isinstance(data, (bytes, bytearray)) else (data or b'').encode())
    except Exception:
        bucket.append(b'')


def _stderr_text(bucket: list) -> str:
    if not bucket:
        return ''
    raw = bucket[0] if bucket else b''
    if isinstance(raw, (bytes, bytearray)):
        return raw.decode('utf-8', errors='replace')
    return str(raw)


def _classify_hw_use(accel_name, text: str) -> str:
    """根据 ffmpeg verbose 日志判断是否真正硬解。"""
    if not accel_name:
        return 'cpu'
    if any(m in text for m in _hw_fail_markers()):
        return 'cpu'
    if any(m in text for m in _hw_success_markers(accel_name)):
        return accel_name
    # 无明确成功标记：不能谎称 GPU
    return 'cpu'


def _iter_mjpeg_from_proc(proc):
    """从 ffmpeg mjpeg pipe 拆帧 yield（不负责 wait/kill）。"""
    buf = bytearray()
    READ_CHUNK = 64 * 1024
    while True:
        chunk = proc.stdout.read(READ_CHUNK)
        if not chunk:
            break
        buf.extend(chunk)
        while True:
            try:
                soi = buf.index(b'\xff\xd8')
            except ValueError:
                if len(buf) > 1:
                    del buf[:-1]
                break
            if soi > 0:
                del buf[:soi]
            try:
                eoi = buf.index(b'\xff\xd9', 2)
            except ValueError:
                if len(buf) > 8 * 1024 * 1024:
                    del buf[:-2]
                break
            frame = bytes(buf[:eoi + 2])
            del buf[:eoi + 2]
            if len(frame) > 2:
                yield frame
    if len(buf) > 2 and buf[0] == 0xFF and buf[1] == 0xD8:
        try:
            eoi = buf.index(b'\xff\xd9', 2)
            yield bytes(buf[:eoi + 2])
        except ValueError:
            pass


def _stream_mjpeg_frames(in_path: str, width: int, height: int,
                          fps: float = 0, vframes: int = 0,
                          quality: int = 80):
    """Stream JPEG frames from ffmpeg mjpeg pipe (GPU decode when possible)."""
    global _LAST_DECODE_ACCEL
    vf = _build_vf(width, height, fps)
    qv = str(max(2, min(31, quality)))
    # 关键：对当前文件先选能真正硬解的后端，再全量转码
    selected = _select_hwaccel_for_file(in_path)
    modes = [selected] if selected is not None else [None]
    if selected is not None:
        modes.append(None)  # 运行期异常时 CPU 兜底
    print(f'[hwaccel] mjpeg selected={selected or "cpu"} order={[m or "cpu" for m in modes]} ffmpeg={FFMPEG}')

    def _start(accel_name):
        # 正式转码用 error 级；硬解靠 plain -hwaccel
        cmd = [FFMPEG, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y']
        if accel_name:
            cmd += _ffmpeg_hwaccel_prefix(accel_name)
        cmd += ['-i', in_path]
        cmd += _ffmpeg_common_args()
        cmd += ['-an', '-sn', '-dn',
                '-vf', vf,
                '-q:v', qv,
                '-pix_fmt', 'yuvj420p',
                '-f', 'image2pipe', '-c:v', 'mjpeg']
        if vframes > 0:
            cmd += ['-frames:v', str(vframes)]
        cmd.append('pipe:1')
        print(f'[hwaccel] mjpeg spawn accel={accel_name or "cpu"}: {" ".join(cmd[:14])} ...')
        return _popen_silent(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    for accel_name in modes:
        proc = _start(accel_name)
        err_bucket = []
        t = Thread(target=_drain_stderr, args=(proc, err_bucket), daemon=True)
        t.start()
        count = 0
        try:
            for frame in _iter_mjpeg_from_proc(proc):
                count += 1
                yield frame
        except Exception as e:
            if accel_name:
                print(f'[hwaccel] mjpeg exception with {accel_name}: {e}')
                try:
                    if proc.poll() is None:
                        proc.kill()
                except Exception:
                    pass
                t.join(timeout=2)
                continue
            raise
        finally:
            try:
                if proc.poll() is None:
                    proc.wait(timeout=3)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
            t.join(timeout=3)

        text = _stderr_text(err_bucket)
        rc = proc.returncode
        if count > 0:
            if accel_name and not any(m in text for m in _hw_fail_markers()):
                _LAST_DECODE_ACCEL = accel_name
                _LAST_GOOD_ACCEL = accel_name
                print(f'[hwaccel] mjpeg used {accel_name}, frames={count}, rc={rc}')
            else:
                # 已通过 file-probe 仍出现 fail marker：极少数运行期回退
                if accel_name and any(m in text for m in _hw_fail_markers()):
                    _LAST_DECODE_ACCEL = 'cpu'
                    print(f'[hwaccel] mjpeg requested {accel_name} but runtime fail-marker; frames={count}, err={(text or "")[:220]!r}')
                else:
                    _LAST_DECODE_ACCEL = 'cpu' if not accel_name else accel_name
                    if accel_name:
                        _LAST_GOOD_ACCEL = accel_name
                    print(f'[hwaccel] mjpeg used {_LAST_DECODE_ACCEL}, frames={count}, rc={rc}')
            return
        if accel_name:
            print(f'[hwaccel] mjpeg empty with {accel_name}, try next; rc={rc} err={(text or "")[:180]!r}')
            continue
    _LAST_DECODE_ACCEL = 'cpu'


def _write_mjpeg_header(fobj, frame_count: int, width: int, height: int):
    """Write 14B MJPEG container header (frame_count can be patched later)."""
    fobj.write(b'MJPG')
    fobj.write(struct.pack('<H', frame_count & 0xFFFF))
    fobj.write(struct.pack('<H', width & 0xFFFF))
    fobj.write(struct.pack('<H', height & 0xFFFF))
    fobj.write(b'\x00' * 4)


def _pack_mjpeg(frames: list, width: int, height: int, quality: int) -> bytes:
    """Pack MJPEG frames into the W25Q file format (small image path)."""
    buf = io.BytesIO()
    _write_mjpeg_header(buf, len(frames), width, height)
    for frame in frames:
        buf.write(struct.pack('<I', len(frame)))
        buf.write(frame)
    return buf.getvalue()


def _process_video_mjpeg(in_path: str, width: int, height: int,
                          output_fps: float = 30, quality: int = 80) -> dict:
    """Process full video with MJPEG: stream frames to disk, no frame truncation."""
    output_fps = max(1, min(60, output_fps))
    _q = max(1, min(100, quality))
    # user 1-100 (higher=better) -> ffmpeg -q:v 31-2 (lower=better)
    ffmpeg_q = max(2, min(31, round(32 - _q / 100 * 30)))

    did = uuid.uuid4().hex
    out_path = TMP / did
    count = 0

    # 流式写入：header 先占位，帧逐个 append，最后回填 frame_count
    with open(out_path, 'wb') as out_f:
        _write_mjpeg_header(out_f, 0, width, height)
        for jpeg_bytes in _stream_mjpeg_frames(in_path, width, height,
                                                fps=output_fps,
                                                quality=ffmpeg_q):
            out_f.write(struct.pack('<I', len(jpeg_bytes)))
            out_f.write(jpeg_bytes)
            count += 1
            # MJPEG frame_count 字段是 uint16，超过 65535 需拒绝（格式限制，不是人为截断）
            if count > 0xFFFF:
                raise RuntimeError('MJPEG 容器 frame_count 为 uint16，最多 65535 帧；请降低 FPS 或缩短视频')

    if count == 0:
        try:
            out_path.unlink()
        except OSError:
            pass
        raise RuntimeError('No frames extracted')

    # patch frame_count
    with open(out_path, 'r+b') as out_f:
        out_f.seek(4)
        out_f.write(struct.pack('<H', count))

    compressed_size = out_path.stat().st_size
    original_duration = 0
    if FFPROBE:
        try:
            dur_cmd = [FFPROBE, '-v', 'error', '-show_entries', 'format=duration',
                       '-of', 'csv=p=0', in_path]
            dur_r = _run_silent(dur_cmd, capture_output=True, text=True, timeout=10)
            original_duration = float(dur_r.stdout.strip())
        except Exception:
            original_duration = 0

    name = Path(in_path).stem
    _DL_REG[did] = {
        'path': str(out_path), 'name': name,
        'mtime': time.time(),
        'frame_count': count, 'width': width, 'height': height,
        'frame_size': width * height * 2, 'fps': output_fps,
    }
    _schedule_cleanup()

    # 小文件才内联 hex；大文件只给 download_id（完整 payload 在磁盘，帧不截断）
    compressed = b''
    if compressed_size <= INLINE_HEX_MAX_BYTES:
        compressed = out_path.read_bytes()

    result = {
        'type': 'video',
        'download_id': did,
        'width': width,
        'height': height,
        'frame_count': count,
        'frame_size': width * height * 2,
        'fps': output_fps,
        'original_duration': original_duration,
        'quality': quality,
        'codec': 'mjpeg',
        'endian': 'big',
        'truncated': False,
    }
    return _attach_payload_fields(result, compressed, b'', compressed_size=compressed_size)


def _process_image_mjpeg(in_path: str, width: int, height: int,
                          quality: int = 80) -> dict:
    """Process a single image with MJPEG compression."""
    _q = max(1, min(100, quality))
    ffmpeg_q = max(2, min(31, round(32 - _q / 100 * 30)))
    frames = []
    for jpeg_bytes in _stream_mjpeg_frames(in_path, width, height,
                                            vframes=1, quality=ffmpeg_q):
        frames.append(jpeg_bytes)
    if not frames:
        raise RuntimeError('No frame extracted from image')
    compressed = _pack_mjpeg(frames, width, height, quality)
    name = Path(in_path).stem
    did = _save_temp(name, compressed, width, height, 1, 30)
    result = {
        'type': 'image',
        'download_id': did,
        'width': width,
        'height': height,
        'frame_count': 1,
        'frame_size': width * height * 2,
        'fps': 30,
        'quality': quality,
        'codec': 'mjpeg',
        'endian': 'big',
        'truncated': False,
    }
    return _attach_payload_fields(result, compressed, b'')


def _process_video(in_path: str, width: int, height: int,
                   output_fps: float = 30, endian: str = '>',
                   brightness: float = 100.0, quality: int = 8) -> dict:
    """Full BL video export: stream-compress frames to disk, no truncation."""
    output_fps = max(1, min(60, output_fps))
    did = uuid.uuid4().hex
    out_path = TMP / did
    count = 0
    fs_raw = width * height * 2
    # 仅保留首帧作为缩略图预览（完整预览由前端按需解码 compressed）
    thumb = b''

    # compress always big-endian; thumb follows requested endian
    with open(out_path, 'wb') as out_f:
        for rgb, fw, fh in _stream_frames(in_path, width, height, fps=output_fps):
            be_frame = _convert_to_rgb565(rgb, fw, fh, '>', brightness)
            out_f.write(compress_frame(be_frame, width, height, quality))
            if count == 0:
                thumb = _to_be(be_frame) if endian == '<' else be_frame
            count += 1

    if count == 0:
        try:
            out_path.unlink()
        except OSError:
            pass
        raise RuntimeError('No frames extracted')

    compressed_size = out_path.stat().st_size
    frame_size_approx = compressed_size // count if count else 0
    name = Path(in_path).stem
    _DL_REG[did] = {
        'path': str(out_path), 'name': name,
        'mtime': time.time(),
        'frame_count': count, 'width': width, 'height': height,
        'frame_size': frame_size_approx, 'fps': output_fps,
    }
    _schedule_cleanup()

    compressed = b''
    if compressed_size <= INLINE_HEX_MAX_BYTES:
        compressed = out_path.read_bytes()

    result = {
        'type': 'video',
        'download_id': did,
        'width': width,
        'height': height,
        'frame_count': count,
        'frame_size': fs_raw,
        'fps': output_fps,
        'quality': quality,
        'codec': 'bl',
        'truncated': False,
    }
    return _attach_payload_fields(result, compressed, thumb, compressed_size=compressed_size)


@app.route('/download/<download_id>', methods=['GET', 'OPTIONS'])
@app.route('/download', methods=['POST', 'OPTIONS'])
def download(download_id=None):
    if request.method == 'OPTIONS':
        return make_response()

    # POST with JSON body (legacy, for single images)
    if request.method == 'POST' and download_id is None:
        data = request.get_json(force=True)
        if not data:
            return jsonify({'error': 'No data'}), 400
        raw = bytes.fromhex(data.get('hex', '')) if data.get('hex') else b''
        if not raw:
            return jsonify({'error': 'No content'}), 400
        w = data.get('width', 160)
        h = data.get('height', 80)
        fcount = data.get('frame_count', 1)
        name = data.get('name', 'output')
        buf = io.BytesIO()
        if fcount == 1:
            buf.write(raw)
            fname = f'{name}.bin'
        else:
            fs = w * h * 2
            with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as zf:
                for i in range(fcount):
                    zf.writestr(f'frame_{i:04d}.bin', raw[i * fs:(i + 1) * fs])
                zf.writestr('_info.txt',
                            f'width={w}\nheight={h}\nframes={fcount}\nframe_size={fs}')
            fname = f'{name}.zip'
        buf.seek(0)
        return send_file(buf, as_attachment=True, download_name=fname)

    # GET with download_id: pure binary payload only (no trailing text!)
    if download_id:
        ent = _DL_REG.get(download_id)
        if not ent:
            abort(404, description='Download not found or expired')
        ent['mtime'] = time.time()  # extend lifetime
        path = Path(ent['path'])
        if not path.is_file():
            abort(404, description='Download file missing')
        w, h = ent['width'], ent['height']
        name = ent['name']
        fname = f'{name}_{w}x{h}_qc.bin'
        return send_file(
            path,
            as_attachment=True,
            download_name=fname,
            mimetype='application/octet-stream',
            conditional=True,
        )

    return jsonify({'error': 'No download ID'}), 400


@app.route('/', methods=['GET', 'OPTIONS'])
def index():
    if request.method == 'OPTIONS':
        return make_response()
    return app.send_static_file('lcd_host_web.html')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='STM IPS Video Processor')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=5000)
    parser.add_argument('--debug', action='store_true')
    args = parser.parse_args()
    print(f'ffmpeg binary: {FFMPEG}')
    # 启动时探测一次 GPU，避免首个请求卡住
    try:
        accel = _detect_hwaccel()
        print(f'HW accel: {accel or "cpu"}')
        print(f'HW try order: {[m or "cpu" for m in _hw_try_order()]}')
    except Exception as e:
        print(f'HW accel probe error: {e}')
    print(f'Server: http://{args.host}:{args.port}')
    app.run(host=args.host, port=args.port, debug=args.debug)