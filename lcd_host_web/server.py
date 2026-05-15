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
from threading import Timer

from flask import Flask, request, send_file, jsonify, make_response, abort

app = Flask(__name__, static_url_path='', static_folder='.')

ALLOWED_EXT = {'.png', '.jpg', '.jpeg', '.bmp', '.gif', '.tiff', '.webp', '.mp4'}
FFMPEG = 'ffmpeg'
FFPROBE = 'ffprobe'
HERE = Path(__file__).parent
PREVIEW_FRAMES = 999999  # include all frames in preview
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


def _check_ffmpeg():
    try:
        subprocess.run([FFMPEG, '-version'], capture_output=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        raise RuntimeError(
            'ffmpeg 未找到，请安装 ffmpeg 并加入 PATH。\n'
            '下载: https://ffmpeg.org/download.html'
        )


_check_ffmpeg()


# ----- helpers ---------------------------------------------------------------

def _probe(path: str) -> dict:
    cmd = [FFPROBE, '-v', 'error', '-select_streams', 'v:0',
           '-show_entries', 'stream=nb_frames,r_frame_rate,avg_frame_rate',
           '-of', 'json', path]
    r = subprocess.run(cmd, capture_output=True, text=True, check=True)
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
        r2 = subprocess.run(cmd2, capture_output=True, text=True)
        try:
            nf = int(r2.stdout.strip())
        except ValueError:
            nf = 0
    return {'fps': fps, 'nb_frames': nf}


def _stream_frames(in_path: str, width: int, height: int,
                   fps: float = 0, vframes: int = 0):
    """Yield (rgb_bytes) for each frame via ffmpeg PPM pipe."""
    cmd = [FFMPEG, '-y', '-i', in_path,
           '-vf', f'scale={width}:{height}:flags=lanczos',
           '-f', 'image2pipe', '-c:v', 'ppm']
    if fps > 0:
        cmd += ['-r', str(fps)]
    if vframes > 0:
        cmd += ['-vframes', str(vframes)]
    cmd.append('pipe:1')

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    try:
        while True:
            # read 3 header lines: P6\n W H\n 255\n
            hdr = b''
            nl = 0
            while nl < 3:
                ch = proc.stdout.read(1)
                if not ch:
                    return
                hdr += ch
                if ch == b'\n':
                    nl += 1
            # parse dims from 2nd line
            lines = hdr.strip().split(b'\n')
            if len(lines) < 3 or lines[0] != b'P6':
                break
            _, dims, _ = lines
            fw, fh = map(int, dims.split())
            pix = fw * fh * 3
            data = proc.stdout.read(pix)
            if len(data) != pix:
                break
            yield data, fw, fh
    finally:
        proc.kill()
        proc.wait()


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
    out = bytearray()
    for i in range(0, len(rgb_pixels), 3):
        out.extend(_rgb_to_rgb565(rgb_pixels[i], rgb_pixels[i + 1], rgb_pixels[i + 2],
                   endian, brightness))
    return bytes(out)


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
    except ValueError:
        return jsonify({'error': 'Invalid parameters'}), 400

    if not (1 <= width <= 1024) or not (1 <= height <= 1024):
        return jsonify({'error': 'Dimensions out of range (1-1024)'}), 400
    ext = Path(file.filename).suffix.lower()
    is_video = ext == '.mp4'
    if ext not in ALLOWED_EXT:
        return jsonify({'error': f'Unsupported file type: {ext}'}), 400

    body = file.read()

    with tempfile.NamedTemporaryFile(delete=False, suffix=ext) as fin:
        fin.write(body)
        in_path = fin.name

    try:
        endian = '<' if swap else '>'
        if is_video:
            result = _process_video(in_path, width, height, fps, endian, brightness)
        else:
            result = _process_image(in_path, width, height, endian, brightness)
    except subprocess.CalledProcessError as e:
        msg = e.stderr.decode('utf-8', errors='replace')[-300:] if e.stderr else str(e)
        return jsonify({'error': f'ffmpeg error: {msg}'}), 500
    except Exception as e:
        return jsonify({'error': str(e)}), 500
    finally:
        try:
            os.unlink(in_path)
        except OSError:
            pass

    result['original_name'] = file.filename
    result['hex'] = result.pop('preview_hex', '')
    result['endian'] = 'little' if swap else 'big'
    return jsonify(result)


def _process_image(in_path: str, width: int, height: int,
                   endian: str = '>', brightness: float = 100.0) -> dict:
    gen = _stream_frames(in_path, width, height, vframes=1)
    try:
        rgb, fw, fh = next(gen)
    except StopIteration:
        raise RuntimeError('ffmpeg produced no output')

    data = _convert_to_rgb565(rgb, fw, fh, endian, brightness)

    return {
        'type': 'image',
        'preview_hex': data.hex(),
        'width': fw,
        'height': fh,
        'frame_count': 1,
        'frame_size': len(data),
        'fps': 30,
    }


def _process_video(in_path: str, width: int, height: int,
                   output_fps: float = 30, endian: str = '>',
                   brightness: float = 100.0) -> dict:
    output_fps = max(1, min(60, output_fps))

    # stream frames → collect full RGB565 into temp file
    preview = bytearray()
    full = bytearray()
    count = 0

    for rgb, fw, fh in _stream_frames(in_path, width, height, fps=output_fps):
        frame = _convert_to_rgb565(rgb, fw, fh, endian, brightness)
        full.extend(frame)
        if count < PREVIEW_FRAMES:
            preview.extend(frame)
        count += 1

    if count == 0:
        raise RuntimeError('No frames extracted')

    # save full data to temp file
    name = Path(in_path).stem
    did = _save_temp(name, bytes(full), width, height, count, output_fps)

    return {
        'type': 'video',
        'preview_hex': bytes(preview).hex(),
        'download_id': did,
        'width': width,
        'height': height,
        'frame_count': count,
        'frame_size': width * height * 2,
        'fps': output_fps,
    }


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

    # GET with download_id (for processed videos)
    if download_id:
        ent = _DL_REG.get(download_id)
        if not ent:
            abort(404, description='Download not found or expired')
        ent['mtime'] = time.time()  # extend lifetime
        w, h = ent['width'], ent['height']
        fc = ent['frame_count']
        fs = ent['frame_size']
        name = ent['name']
        raw = Path(ent['path']).read_bytes()
        buf = io.BytesIO()
        if fc == 1:
            buf.write(raw)
            fname = f'{name}_{w}x{h}.bin'
        else:
            with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as zf:
                for i in range(fc):
                    zf.writestr(f'frame_{i:04d}.bin', raw[i * fs:(i + 1) * fs])
                zf.writestr('_info.txt',
                            f'width={w}\nheight={h}\nframes={fc}\nframe_size={fs}')
            fname = f'{name}_{w}x{h}.zip'
        buf.seek(0)
        return send_file(buf, as_attachment=True, download_name=fname)

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
    print(f'Server: http://{args.host}:{args.port}')
    app.run(host=args.host, port=args.port, debug=args.debug)
