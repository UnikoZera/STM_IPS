import io
import os
import json
import re
import time
import uuid
import struct
import subprocess
import tempfile
import zipfile
import threading
from pathlib import Path
from threading import Timer, Lock, Thread

from fastapi import FastAPI, UploadFile, File, Form, Request
from fastapi import HTTPException
from fastapi.responses import JSONResponse, FileResponse, Response
from fastapi.middleware.cors import CORSMiddleware
try:
    from api.limits import MAX_UPLOAD_BYTES, MAX_OUTPUT_BYTES
    from container_format import pack_mjpeg as pack_mjpeg_container, pack_raw as pack_raw_container
    from ffmpeg_service import popen as ffmpeg_popen
    from job_store import ConversionContext, ConversionJobStore, DownloadStore
except ModuleNotFoundError:  # package import used by tests and embedding tools
    from .api.limits import MAX_UPLOAD_BYTES, MAX_OUTPUT_BYTES
    from .container_format import pack_mjpeg as pack_mjpeg_container, pack_raw as pack_raw_container
    from .ffmpeg_service import popen as ffmpeg_popen
    from .job_store import ConversionContext, ConversionJobStore, DownloadStore

app = FastAPI(title='STM IPS Host', docs_url=None, redoc_url=None, openapi_url=None)

# CORS：前端 fetch 走同源，这里仅兜底保留（file:// 或跨端口调试时用得上）。
app.add_middleware(
    CORSMiddleware,
    allow_origins=['*'],
    allow_methods=['GET', 'POST', 'OPTIONS'],
    allow_headers=['*'],
)

ALLOWED_EXT = {'.png', '.jpg', '.jpeg', '.bmp', '.gif', '.tiff', '.webp', '.mp4', '.webm', '.mkv', '.avi', '.mov', '.flv', '.wmv'}
VIDEO_EXT = {'.mp4', '.webm', '.mkv', '.avi', '.mov', '.flv', '.wmv', '.gif'}

# ============================================================================
#  FFmpeg 探测与配置
# ============================================================================

def _find_ffmpeg():
    """Try system ffmpeg first, fall back to imageio-ffmpeg bundled binary.

    注意：imageio-ffmpeg 通常只带 ffmpeg，不带 ffprobe。
    无 ffprobe 时返回 None，调用方必须容错（时长等元数据可缺省）。
    """
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

# ============================================================================
#  全局配置常量
# ============================================================================

_NO_WINDOW = 0x08000000  # Windows CREATE_NO_WINDOW for ffmpeg probes
_CONVERT_LOCK = Lock()   # 同时只跑一个重转码任务（避免多任务抢 IO/显存）
_CONVERSION_JOBS = ConversionJobStore(max_active=1)
_JOB_LOCAL = threading.local()
try:
    _CPU_COUNT = os.cpu_count() or 2
except Exception:
    _CPU_COUNT = 2
FFMPEG_THREADS = max(2, min(8, _CPU_COUNT))  # 多线程编解码/滤镜，上限 8
SCALE_FLAGS = 'lanczos'  # 缩放滤镜（预览与烧录共用，改这里同时影响两端画质）

# GPU 硬解状态（启动时探测；可用 STM_IPS_HWACCEL 环境变量覆盖）
_HWACCEL = None           # None = CPU
_HWACCEL_PROBED = False
_LAST_DECODE_ACCEL = 'cpu'   # 最近一次实际解码后端（供 /convert 回传）
_LAST_GOOD_ACCEL = None      # 最近一次成功的硬解后端
_HW_FILE_CACHE = {}          # 按文件属性缓存选型结果，避免重复 file-probe


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
    context = getattr(_JOB_LOCAL, 'context', None)
    return ffmpeg_popen(*args, context=context, **kwargs)


class ConversionCancelled(RuntimeError):
    pass


def _job_context() -> ConversionContext | None:
    return getattr(_JOB_LOCAL, 'context', None)


def _job_check_cancelled() -> None:
    context = _job_context()
    if context and context.cancelled:
        raise ConversionCancelled('Conversion cancelled')


def _job_report(progress: float, processed_frames: int | None = None,
                phase: str | None = None, detail: str | None = None) -> None:
    context = _job_context()
    if context:
        context.report(progress, processed_frames, phase, detail)


def _job_frame_progress(frame_count: int) -> None:
    context = _job_context()
    if not context:
        return
    if context.total_frames:
        ratio = min(0.99, frame_count / context.total_frames)
        progress = 10.0 + ratio * 80.0
        detail = '正在转换...'
    else:
        progress = min(89.0, 10.0 + frame_count * 0.25)
        detail = '正在转换...'
    context.report(progress, frame_count, 'converting', detail)


HERE = Path(__file__).parent

# 静态资源：CSS/JS 拆分为独立文件，统一挂载到 /static（frozen 时指向 _MEIPASS）。
_STATIC_DIR = HERE / 'static'
if _STATIC_DIR.is_dir():
    from fastapi.staticfiles import StaticFiles
    app.mount('/static', StaticFiles(directory=str(_STATIC_DIR)), name='static')

# ============================================================================
#  下载管理：临时文件注册、过期清理
# ============================================================================

# 注意：不截断视频帧。此阈值仅用于"JSON 内联 hex 的安全上限"，超大结果走 download_id 二进制。
INLINE_HEX_MAX_BYTES = 256 * 1024
DOWNLOAD_TTL = 300        # seconds before temp files are cleaned

TMP = Path(tempfile.gettempdir()) / 'stm_ips_dl'
TMP.mkdir(parents=True, exist_ok=True)

# download_id -> {path, name, mtime, frame_count, width, height, frame_size, fps}
_DOWNLOAD_STORE = DownloadStore(TMP, ttl_seconds=DOWNLOAD_TTL)
# Compatibility view for the existing conversion pipeline. All cleanup and
# lookups are owned by DownloadStore and protected by its lock.
_DL_REG = _DOWNLOAD_STORE.entries


def _schedule_cleanup():
    _DOWNLOAD_STORE.schedule()


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


# Raw RGB565 container (self-describing, mirrors MJPEG header layout):
#   Header (14B):
#     [0..3]  Magic "RAW5"
#     [4..5]  frame_count (uint16 LE)
#     [6..7]  width       (uint16 LE)
#     [8..9]  height      (uint16 LE)
#     [10..13] reserved
#   Body: concatenated big-endian RGB565 frames, each width*height*2 bytes
RAW_MAGIC = b'RAW5'


def _write_raw_header(fobj, frame_count: int, width: int, height: int):
    """Write 14B raw RGB565 container header (frame_count can be patched later)."""
    fobj.write(RAW_MAGIC)
    fobj.write(struct.pack('<H', frame_count & 0xFFFF))
    fobj.write(struct.pack('<H', width & 0xFFFF))
    fobj.write(struct.pack('<H', height & 0xFFFF))
    fobj.write(b'\x00' * 4)


def _pack_raw_payload(frames_be: list, width: int, height: int) -> bytes:
    """Pack one or more BE RGB565 frames into RAW5 container."""
    return pack_raw_container(frames_be, width, height)


# ----- helpers ---------------------------------------------------------------

def _probe(path: str) -> dict:
    """可选元数据探测；无 ffprobe 时返回空默认值。"""
    if not FFPROBE:
        # imageio-ffmpeg bundles ffmpeg but normally not ffprobe. The input
        # header still exposes duration, which is enough for useful progress.
        try:
            r = _run_silent(
                [FFMPEG, '-hide_banner', '-i', path],
                capture_output=True, text=True, check=False, timeout=15,
            )
            text = (r.stderr or '') + (r.stdout or '')
            match = re.search(
                r'Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)',
                text,
            )
            duration = 0.0
            if match:
                hours, minutes, seconds = match.groups()
                duration = int(hours) * 3600 + int(minutes) * 60 + float(seconds)
            return {'fps': 0, 'nb_frames': 0, 'duration': duration}
        except Exception:
            return {'fps': 0, 'nb_frames': 0, 'duration': 0}
    cmd = [FFPROBE, '-v', 'error', '-select_streams', 'v:0',
           '-show_entries', 'stream=nb_frames,duration,r_frame_rate,avg_frame_rate:format=duration',
           '-of', 'json', path]
    r = _run_silent(cmd, capture_output=True, text=True, check=True)
    data = json.loads(r.stdout)
    s = data.get('streams', [{}])[0]

    def _parse_fps(val: str) -> float:
        if not val or val in ('N/A', '0', '0/0'):
            return 0
        if '/' in val:
            n, d = val.split('/')
            return float(n) / float(d) if float(d) else 0
        return float(val)

    def _parse_number(val) -> float:
        try:
            number = float(val)
            return number if number >= 0 else 0
        except (TypeError, ValueError):
            return 0

    fps = _parse_fps(s.get('r_frame_rate', '0/1'))
    if fps <= 0:
        fps = _parse_fps(s.get('avg_frame_rate', '0/1'))
    fps = max(fps, 1)  # minimum 1 fps

    nf = int(_parse_number(s.get('nb_frames', 0)))
    duration = _parse_number(s.get('duration'))
    if not duration:
        duration = _parse_number(data.get('format', {}).get('duration'))
    return {'fps': fps, 'nb_frames': nf, 'duration': duration}


def _estimate_output_frames(source_meta: dict, output_fps: float) -> int:
    """Estimate frames after the output fps filter without a second full scan."""
    duration = float(source_meta.get('duration') or 0)
    source_frames = int(source_meta.get('nb_frames') or 0)
    source_fps = float(source_meta.get('fps') or 0)
    if duration > 0 and output_fps > 0:
        return max(1, int(duration * output_fps + 0.999))
    if source_frames > 0 and source_fps > 0 and output_fps > 0:
        return max(1, int(source_frames * output_fps / source_fps + 0.999))
    return max(0, source_frames)


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


@app.post('/convert')
def convert(file: UploadFile = File(None),
            width: str = Form('160'),
            height: str = Form('80'),
            fps: str = Form('30'),
            swap: str = Form('0'),
            brightness: str = Form('100'),
            codec: str = Form('mjpeg'),
            quality: str = Form('70')):
    if not file:
        return JSONResponse({'error': 'No file uploaded'}, status_code=400)
    if not file.filename:
        return JSONResponse({'error': 'Empty filename'}, status_code=400)

    try:
        width = int(width)
        height = int(height)
        fps = max(1, min(60, float(fps)))
        swap = swap == '1'
        brightness = max(10, min(300, float(brightness)))
        codec = codec.strip().lower()
        if codec not in ('mjpeg', 'raw'):
            codec = 'mjpeg'
        if codec == 'mjpeg':
            quality = max(1, min(100, int(quality)))
        else:
            quality = 0
    except ValueError:
        return JSONResponse({'error': 'Invalid parameters'}, status_code=400)

    if not (1 <= width <= 1024) or not (1 <= height <= 1024):
        return JSONResponse({'error': 'Dimensions out of range (1-1024)'}, status_code=400)
    ext = Path(file.filename).suffix.lower()
    is_video = ext in VIDEO_EXT
    if ext not in ALLOWED_EXT:
        return JSONResponse({'error': f'Unsupported file type: {ext}'}, status_code=400)
    declared_size = getattr(file, 'size', None)
    if declared_size is not None and declared_size > MAX_UPLOAD_BYTES:
        return JSONResponse({'error': f'Upload exceeds {MAX_UPLOAD_BYTES} bytes'}, status_code=413)

    # 串行化重转码，避免多请求叠加把 CPU 打满
    if not _CONVERT_LOCK.acquire(blocking=False):
        return JSONResponse({'error': '已有转换任务进行中，请稍后再试'}, status_code=429)

    global _LAST_DECODE_ACCEL
    _LAST_DECODE_ACCEL = 'cpu'

    with tempfile.NamedTemporaryFile(delete=False, suffix=ext) as fin:
        total_bytes = 0
        while True:
            chunk = file.file.read(1024 * 1024)
            if not chunk:
                break
            total_bytes += len(chunk)
            if total_bytes > MAX_UPLOAD_BYTES:
                fin.close()
                try:
                    os.unlink(fin.name)
                except OSError:
                    pass
                _CONVERT_LOCK.release()
                return JSONResponse({'error': f'Upload exceeds {MAX_UPLOAD_BYTES} bytes'}, status_code=413)
            fin.write(chunk)
        in_path = fin.name

    try:
        _job_report(2, phase='preparing', detail='正在准备输入文件...')
        _job_check_cancelled()
        context = _job_context()
        if context:
            try:
                source_meta = _probe(in_path)
                context.total_frames = (
                    _estimate_output_frames(source_meta, fps) if is_video else 1
                )
            except Exception:
                context.total_frames = 0
        _job_report(6, phase='preparing', detail='正在检查解码环境...')
        _job_check_cancelled()
        _detect_hwaccel()
        _job_report(10, phase='converting', detail='开始转换...')
        endian = '<' if swap else '>'
        if is_video:
            if codec == 'mjpeg':
                result = _process_video_mjpeg(in_path, width, height, fps, quality)
            else:
                result = _process_video_raw(in_path, width, height, fps, endian, brightness)
        else:
            if codec == 'mjpeg':
                result = _process_image_mjpeg(in_path, width, height, quality)
            else:
                result = _process_image_raw(in_path, width, height, endian, brightness)
        _job_report(92, phase='finalizing', detail='正在写入转换结果...')
        _job_check_cancelled()
    except ConversionCancelled:
        return JSONResponse({'error': 'Conversion cancelled'}, status_code=499)
    except subprocess.CalledProcessError as e:
        msg = e.stderr.decode('utf-8', errors='replace')[-300:] if e.stderr else str(e)
        return JSONResponse({'error': f'ffmpeg error: {msg}'}, status_code=500)
    except Exception as e:
        return JSONResponse({'error': str(e)}, status_code=500)
    finally:
        _CONVERT_LOCK.release()
        try:
            os.unlink(in_path)
        except OSError:
            pass

    _job_report(97, phase='finalizing', detail='正在校验转换结果...')
    result_id = result.get('download_id')
    result_entry = _DL_REG.get(result_id) if result_id else None
    if result_entry:
        try:
            output_size = Path(result_entry['path']).stat().st_size
        except OSError:
            output_size = 0
        if output_size > MAX_OUTPUT_BYTES:
            try:
                os.unlink(result_entry['path'])
            except OSError:
                pass
            _DL_REG.pop(result_id, None)
            return JSONResponse({'error': f'Output exceeds {MAX_OUTPUT_BYTES} bytes'}, status_code=413)

    result['original_name'] = file.filename
    result['hex'] = result.pop('preview_hex', '')
    if codec == 'raw':
        # 烧录 payload 恒为 big-endian RGB565；preview 也统一按 big 解析
        result['endian'] = 'big'
    result['ffmpeg_threads'] = FFMPEG_THREADS
    # 报告“实际用了什么”，不是“探测到什么”
    result['hwaccel'] = _LAST_DECODE_ACCEL or (_detect_hwaccel() or 'cpu')
    result['hwaccel_available'] = _detect_hwaccel() or 'cpu'
    _job_report(100, phase='completed', detail='转换完成')
    return result


class _MemoryUpload:
    """Minimal UploadFile-compatible object for background conversion jobs."""
    def __init__(self, filename: str, data: bytes):
        self.filename = filename
        self.file = io.BytesIO(data)
        self.size = len(data)


class _PathUpload:
    """UploadFile-compatible disk-backed input for queued jobs."""
    def __init__(self, filename: str, path: str, size: int):
        self.filename = filename
        self.file = open(path, 'rb')
        self.size = size


@app.post('/convert/jobs')
def create_conversion_job(file: UploadFile = File(None),
                          width: str = Form('160'), height: str = Form('80'),
                          fps: str = Form('30'), swap: str = Form('0'),
                          brightness: str = Form('100'), codec: str = Form('mjpeg'),
                          quality: str = Form('70')):
    """Queue conversion and return a pollable job id."""
    if not file or not file.filename:
        return JSONResponse({'error': 'No file uploaded'}, status_code=400)
    ext = Path(file.filename).suffix.lower() or '.bin'
    with tempfile.NamedTemporaryFile(delete=False, suffix=ext) as staged:
        staged_path = staged.name
        upload_size = 0
        while True:
            chunk = file.file.read(1024 * 1024)
            if not chunk:
                break
            upload_size += len(chunk)
            if upload_size > MAX_UPLOAD_BYTES:
                staged.close()
                try:
                    os.unlink(staged_path)
                except OSError:
                    pass
                return JSONResponse({'error': f'Upload exceeds {MAX_UPLOAD_BYTES} bytes'}, status_code=413)
            staged.write(chunk)

    def worker(context: ConversionContext):
        _JOB_LOCAL.context = context
        upload = _PathUpload(file.filename, staged_path, upload_size)
        try:
            response = convert(
                upload, width, height, fps, swap,
                brightness, codec, quality,
            )
            if isinstance(response, JSONResponse):
                detail = response.body.decode('utf-8', errors='replace')
                raise RuntimeError(detail)
            return response
        finally:
            upload.file.close()
            try:
                os.unlink(staged_path)
            except OSError:
                pass
            _JOB_LOCAL.context = None

    try:
        job_id = _CONVERSION_JOBS.submit(worker)
    except RuntimeError as exc:
        try:
            os.unlink(staged_path)
        except OSError:
            pass
        return JSONResponse({'error': str(exc)}, status_code=429)
    return JSONResponse({'job_id': job_id, 'status': 'queued'})


@app.get('/convert/jobs/{job_id}')
def get_conversion_job(job_id: str):
    job = _CONVERSION_JOBS.get(job_id)
    if not job:
        raise HTTPException(404, detail='Conversion job not found')
    return job


@app.post('/convert/jobs/{job_id}/cancel')
def cancel_conversion_job(job_id: str):
    if not _CONVERSION_JOBS.cancel(job_id):
        raise HTTPException(409, detail='Conversion job cannot be cancelled')
    return {'job_id': job_id, 'status': 'cancelling'}


# ============================================================================
#  RAW5 容器：图片处理
# ============================================================================


def _process_image_raw(in_path: str, width: int, height: int,
                       endian: str = '>', brightness: float = 100.0) -> dict:
    """Export raw RGB565 image as RAW5 container (14B header + BE pixels)."""
    gen = _stream_frames(in_path, width, height, vframes=1)
    try:
        rgb, fw, fh = next(gen)
    except StopIteration:
        raise RuntimeError('ffmpeg produced no output')

    # MCU native flash format is big-endian RGB565
    be_frame = _convert_to_rgb565(rgb, fw, fh, '>', brightness)
    payload = _pack_raw_payload([be_frame], fw, fh)
    # preview uses same BE frame (frontend renders with endian=big)
    preview = be_frame

    name = Path(in_path).stem
    did = _save_temp(name, payload, fw, fh, 1, 30)

    result = {
        'type': 'image',
        'download_id': did,
        'width': fw,
        'height': fh,
        'frame_count': 1,
        'frame_size': fw * fh * 2,
        'fps': 30,
        'quality': 0,
        'codec': 'raw',
        'endian': 'big',
    }
    return _attach_payload_fields(result, payload, preview)


# ============================================================================
#  MJPEG 容器格式：编解码辅助函数
# ============================================================================

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
                _job_check_cancelled()
                count += 1
                yield frame
        except ConversionCancelled:
            raise
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


def _pack_mjpeg(frames: list, width: int, height: int) -> bytes:
    """Pack MJPEG frames into the W25Q file format (small image path)."""
    return pack_mjpeg_container(frames, width, height)


# ============================================================================
#  MJPEG 容器：视频处理
# ============================================================================


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
    try:
        with open(out_path, 'wb') as out_f:
            _write_mjpeg_header(out_f, 0, width, height)
            for jpeg_bytes in _stream_mjpeg_frames(in_path, width, height,
                                                    fps=output_fps,
                                                    quality=ffmpeg_q):
                _job_check_cancelled()
                out_f.write(struct.pack('<I', len(jpeg_bytes)))
                out_f.write(jpeg_bytes)
                count += 1
                _job_frame_progress(count)
                # MJPEG frame_count 字段是 uint16，超过 65535 需拒绝（格式限制，不是人为截断）
                if count > 0xFFFF:
                    raise RuntimeError('MJPEG 容器 frame_count 为 uint16，最多 65535 帧；请降低 FPS 或缩短视频')
    except Exception:
        try:
            out_path.unlink()
        except OSError:
            pass
        raise

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
    compressed = _pack_mjpeg(frames, width, height)
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


# ============================================================================
#  RAW5 容器：视频处理
# ============================================================================


def _process_video_raw(in_path: str, width: int, height: int,
                       output_fps: float = 30, endian: str = '>',
                       brightness: float = 100.0) -> dict:
    """Export RAW5 container: 14B header + concatenated BE RGB565 frames."""
    del endian  # payload/preview always big-endian for MCU/path consistency
    output_fps = max(1, min(60, output_fps))
    did = uuid.uuid4().hex
    out_path = TMP / did
    count = 0
    fs_raw = width * height * 2
    # 仅保留首帧作为缩略图预览（不含容器头）
    thumb = b''

    try:
        with open(out_path, 'wb') as out_f:
            # 先写占位 header，最后回填 frame_count
            _write_raw_header(out_f, 0, width, height)
            for rgb, fw, fh in _stream_frames(in_path, width, height, fps=output_fps):
                _job_check_cancelled()
                be_frame = _convert_to_rgb565(rgb, fw, fh, '>', brightness)
                out_f.write(be_frame)
                if count == 0:
                    thumb = be_frame
                count += 1
                _job_frame_progress(count)
                if count > 0xFFFF:
                    raise RuntimeError('RAW5 frame_count 为 uint16，最多 65535 帧；请降低 FPS 或缩短视频')
    except Exception:
        try:
            out_path.unlink()
        except OSError:
            pass
        raise

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
    name = Path(in_path).stem
    _DL_REG[did] = {
        'path': str(out_path), 'name': name,
        'mtime': time.time(),
        'frame_count': count, 'width': width, 'height': height,
        'frame_size': fs_raw, 'fps': output_fps,
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
        'quality': 0,
        'codec': 'raw',
        'endian': 'big',
        'truncated': False,
    }
    return _attach_payload_fields(result, compressed, thumb, compressed_size=compressed_size)


@app.get('/download/{download_id}')
def download(download_id: str):
    # GET with download_id: pure binary payload only (no trailing text!)
    ent = _DOWNLOAD_STORE.touch(download_id)
    if not ent:
        raise HTTPException(404, detail='Download not found or expired')
    path = Path(ent['path'])
    if not path.is_file():
        raise HTTPException(404, detail='Download file missing')
    w, h = ent['width'], ent['height']
    name = ent['name']
    fname = f'{name}_{w}x{h}_qc.bin'
    return FileResponse(
        path,
        filename=fname,
        media_type='application/octet-stream',
    )


@app.post('/download')
async def download_post(request: Request):
    # POST with JSON body (legacy, for single images)
    data = await request.json()
    if not data:
        return JSONResponse({'error': 'No data'}, status_code=400)
    hex_data = data.get('hex', '') or ''
    if len(hex_data) > MAX_OUTPUT_BYTES * 2:
        return JSONResponse({'error': 'Download payload exceeds configured limit'}, status_code=413)
    raw = bytes.fromhex(hex_data) if hex_data else b''
    if not raw:
        return JSONResponse({'error': 'No content'}, status_code=400)
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
    from urllib.parse import quote
    cd = f"attachment; filename*=UTF-8''{quote(fname)}"
    return Response(
        content=buf.getvalue(),
        media_type='application/octet-stream',
        headers={'Content-Disposition': cd},
    )


@app.get('/')
def index():
    resp = FileResponse(
        HERE / 'index.html',
        headers={
            # 禁用 HTML 缓存：bat 每次启动都复用固定端口，浏览器默认
            # 缓存会让 UI 修改无法生效；必须 no-cache。
            'Cache-Control': 'no-cache, no-store, must-revalidate',
            'Pragma': 'no-cache',
            'Expires': '0',
        },
    )
    return resp


if __name__ == '__main__':
    import argparse
    import uvicorn
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
    uvicorn.run(app, host=args.host, port=args.port, log_level='debug' if args.debug else 'info')
