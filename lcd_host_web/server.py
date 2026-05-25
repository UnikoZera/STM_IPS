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
    _, bw, bh, npix, idx_bytes, rm, gm, bm = _BLK_CFG[quality]
    def _lerp(a, b, n, d): return (a*(d-n)+b*n)//d
    bxc = (w + bw - 1) // bw
    byc = (h + bh - 1) // bh
    out = bytearray()

    for by in range(byc):
        for bx in range(bxc):
            r5s, g6s, b5s = [], [], []
            for py in range(bh):
                for px in range(bw):
                    ix = bx * bw + px
                    iy = by * bh + py
                    if ix < w and iy < h:
                        off = (iy * w + ix) * 2
                        hi, lo = rgb565[off], rgb565[off + 1]
                        val = (hi << 8) | lo
                        r5s.append((val >> 11) & 0x1F)
                        g6s.append((val >> 5) & 0x3F)
                        b5s.append(val & 0x1F)
                    else:
                        r5s.append(0); g6s.append(0); b5s.append(0)

            bright = [r5s[i]*19595 + g6s[i]*38470 + b5s[i]*7471
                      for i in range(npix)]
            med = sorted(bright)[npix // 2]

            sums = [[0,0,0,0],[0,0,0,0]]
            for i in range(npix):
                g = 0 if bright[i] < med else 1
                sums[g][0] += 1
                sums[g][1] += r5s[i]; sums[g][2] += g6s[i]; sums[g][3] += b5s[i]

            def _avg(s, fb):
                return (s[1]//s[0], s[2]//s[0], s[3]//s[0]) if s[0] else fb
            fb = (sum(r5s)//npix, sum(g6s)//npix, sum(b5s)//npix)
            c0 = _avg(sums[0], fb); c1 = _avg(sums[1], fb)
            c0 = (c0[0]&rm, c0[1]&gm, c0[2]&bm)
            c1 = (c1[0]&rm, c1[1]&gm, c1[2]&bm)

            # 4-colour palette: c0, c1, and 2 interpolated
            c2 = (_lerp(c0[0],c1[0],1,3), _lerp(c0[1],c1[1],1,3), _lerp(c0[2],c1[2],1,3))
            c3 = (_lerp(c0[0],c1[0],2,3), _lerp(c0[1],c1[1],2,3), _lerp(c0[2],c1[2],2,3))
            pal = [c0, c1, c2, c3]

            c0v = (c0[0]<<11)|(c0[1]<<5)|c0[2]
            c1v = (c1[0]<<11)|(c1[1]<<5)|c1[2]
            out.append((c0v>>8)&0xFF); out.append(c0v&0xFF)
            out.append((c1v>>8)&0xFF); out.append(c1v&0xFF)

            indices = 0
            for i in range(npix):
                pr, pg, pb = r5s[i], g6s[i], b5s[i]
                best_d, best_i = 99999, 0
                for ci in range(4):
                    dr = pr-pal[ci][0]; dg = pg-pal[ci][1]; db = pb-pal[ci][2]
                    d = dr*dr + dg*dg + db*db
                    if d < best_d: best_d, best_i = d, ci
                indices = (indices << 2) | best_i
            for b in range(idx_bytes):
                out.append((indices >> (8*(idx_bytes-1-b))) & 0xFF)
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
        subprocess.run([FFMPEG, '-version'], capture_output=True, check=True)
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
        quality = max(7, min(11, int(request.form.get('quality', 11))))
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
            result = _process_video(in_path, width, height, fps, endian, brightness, quality)
        else:
            result = _process_image(in_path, width, height, endian, brightness, quality)
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
                   endian: str = '>', brightness: float = 100.0, quality: int = 8) -> dict:
    gen = _stream_frames(in_path, width, height, vframes=1)
    try:
        rgb, fw, fh = next(gen)
    except StopIteration:
        raise RuntimeError('ffmpeg produced no output')

    data = _convert_to_rgb565(rgb, fw, fh, endian, brightness)

    # compression always uses big-endian (MCU native)
    comp_data = _to_be(data) if endian == '<' else data

    result = {
        'type': 'image',
        'preview_hex': data.hex(),
        'width': fw,
        'height': fh,
        'frame_count': 1,
        'frame_size': len(data),
        'fps': 30,
        'quality': quality,
    }

    compressed = compress_frame(comp_data, fw, fh, quality)
    result['compressed_hex'] = compressed.hex()

    return result


def _process_video(in_path: str, width: int, height: int,
                   output_fps: float = 30, endian: str = '>',
                   brightness: float = 100.0, quality: int = 8) -> dict:
    output_fps = max(1, min(60, output_fps))

    # stream frames
    full_compressed = bytearray()
    preview_raw = bytearray()   # raw RGB565 for browser preview
    count = 0

    fs_raw = width * height * 2  # raw frame size
    for rgb, fw, fh in _stream_frames(in_path, width, height, fps=output_fps):
        frame = _convert_to_rgb565(rgb, fw, fh, endian, brightness)
        be_frame = _to_be(frame) if endian == '<' else frame
        full_compressed.extend(compress_frame(be_frame, width, height, quality))
        # keep raw data for preview (first N frames)
        if count < PREVIEW_FRAMES:
            preview_raw.extend(frame)
        count += 1

    if count == 0:
        raise RuntimeError('No frames extracted')

    frame_size_approx = len(full_compressed) // count if count else 0

    # save full compressed data to temp file
    name = Path(in_path).stem
    did = _save_temp(name, bytes(full_compressed), width, height, count, output_fps)
    if did in _DL_REG:
        _DL_REG[did]['frame_size'] = frame_size_approx

    result = {
        'type': 'video',
        'preview_hex': bytes(preview_raw).hex(),
        'download_id': did,
        'width': width,
        'height': height,
        'frame_count': count,
        'frame_size': fs_raw,
        'fps': output_fps,
        'quality': quality,
    }
    result['compressed_hex'] = bytes(full_compressed).hex()

    return result


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
        name = ent['name']
        raw = Path(ent['path']).read_bytes()
        buf = io.BytesIO()
        # compressed data: download as single blob (variable-length frames)
        buf.write(raw)
        info_txt = f'# STM IPS Compressed Video\n'
        info_txt += f'width={w}\nheight={h}\nframes={fc}\ncompressed_size={len(raw)}\n'
        buf.write(info_txt.encode())
        fname = f'{name}_{w}x{h}_qc.bin'
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