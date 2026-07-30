const { spawn } = require('node:child_process');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

const IMAGE_EXTENSIONS = new Set(['.png', '.jpg', '.jpeg', '.bmp', '.tiff', '.webp']);
const VIDEO_EXTENSIONS = new Set(['.mp4', '.webm', '.mkv', '.avi', '.mov', '.flv', '.wmv', '.gif']);
const MAX_FRAME_COUNT = 0xffff;

function ffmpegPath(resourcesPath) {
  const executable = process.platform === 'win32' ? 'ffmpeg.exe' : 'ffmpeg';
  const bundled = path.join(resourcesPath, 'ffmpeg', executable);
  return process.env.STM_IPS_FFMPEG || bundled;
}

async function exists(target) {
  try { await fs.access(target); return true; } catch { return false; }
}

async function resolveFfmpeg(resourcesPath) {
  const preferred = ffmpegPath(resourcesPath);
  if (process.env.STM_IPS_FFMPEG || await exists(preferred)) return preferred;
  return process.platform === 'win32' ? 'ffmpeg.exe' : 'ffmpeg';
}

function validatedOptions(options) {
  const width = Math.max(1, Math.min(1024, Number.parseInt(options.width, 10) || 160));
  const height = Math.max(1, Math.min(1024, Number.parseInt(options.height, 10) || 80));
  const fps = Math.max(1, Math.min(60, Number(options.fps) || 30));
  const brightness = Math.max(10, Math.min(300, Number(options.brightness) || 100));
  const codec = options.codec === 'raw' ? 'raw' : 'mjpeg';
  const quality = Math.max(1, Math.min(100, Number.parseInt(options.quality, 10) || 70));
  return { width, height, fps, brightness, codec, quality };
}

function writeHeader(magic, frameCount, width, height) {
  const header = Buffer.alloc(14);
  header.write(magic, 0, 'ascii');
  header.writeUInt16LE(frameCount, 4);
  header.writeUInt16LE(width, 6);
  header.writeUInt16LE(height, 8);
  return header;
}

function append(chunkList, chunk) {
  if (chunk?.length) chunkList.push(Buffer.from(chunk));
}

function collectMjpegFrames(process, task) {
  return new Promise((resolve, reject) => {
    const frames = [];
    let buffered = Buffer.alloc(0);
    let stderr = '';
    process.stdout.on('data', (chunk) => {
      buffered = Buffer.concat([buffered, chunk]);
      while (true) {
        const start = buffered.indexOf(Buffer.from([0xff, 0xd8]));
        if (start < 0) { buffered = buffered.subarray(Math.max(0, buffered.length - 1)); break; }
        const end = buffered.indexOf(Buffer.from([0xff, 0xd9]), start + 2);
        if (end < 0) { if (start > 0) buffered = buffered.subarray(start); break; }
        frames.push(buffered.subarray(start, end + 2));
        if (frames.length > MAX_FRAME_COUNT) { task.cancelled = true; process.kill(); break; }
        buffered = buffered.subarray(end + 2);
      }
    });
    process.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
    process.once('error', reject);
    process.once('close', (code, signal) => {
      if (task.cancelled) return reject(new Error('转换已取消'));
      if (code !== 0) return reject(new Error(`FFmpeg MJPEG 转码失败：${stderr.trim() || signal || code}`));
      if (!frames.length) return reject(new Error('FFmpeg 未输出视频帧'));
      resolve(frames);
    });
  });
}

function collectRawFrames(process, width, height, brightness, task) {
  return new Promise((resolve, reject) => {
    const frames = [];
    const frameSize = width * height * 3;
    let buffered = Buffer.alloc(0);
    let stderr = '';
    process.stdout.on('data', (chunk) => {
      buffered = Buffer.concat([buffered, chunk]);
      while (buffered.length >= frameSize) {
        frames.push(rgb24ToRgb565(buffered.subarray(0, frameSize), brightness));
        if (frames.length > MAX_FRAME_COUNT) { task.cancelled = true; process.kill(); break; }
        buffered = buffered.subarray(frameSize);
      }
    });
    process.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
    process.once('error', reject);
    process.once('close', (code, signal) => {
      if (task.cancelled) return reject(new Error('转换已取消'));
      if (code !== 0) return reject(new Error(`FFmpeg RGB565 转码失败：${stderr.trim() || signal || code}`));
      if (!frames.length) return reject(new Error('FFmpeg 未输出视频帧'));
      resolve(frames);
    });
  });
}

function rgb24ToRgb565(rgb, brightness) {
  const output = Buffer.allocUnsafe((rgb.length / 3) * 2);
  const scale = brightness / 100;
  for (let source = 0, target = 0; source < rgb.length; source += 3, target += 2) {
    const red = Math.min(255, Math.round(rgb[source] * scale));
    const green = Math.min(255, Math.round(rgb[source + 1] * scale));
    const blue = Math.min(255, Math.round(rgb[source + 2] * scale));
    const packed = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
    output[target] = packed >> 8;
    output[target + 1] = packed & 0xff;
  }
  return output;
}

function startFfmpeg(executable, inputPath, options, isImage) {
  const vf = `${isImage ? '' : `fps=${options.fps},`}scale=${options.width}:${options.height}:flags=lanczos`;
  const args = ['-hide_banner', '-loglevel', 'error', '-nostdin', '-y', '-i', inputPath, '-an', '-sn', '-dn', '-vf', vf];
  if (options.codec === 'mjpeg') {
    const quantizer = String(Math.max(2, Math.min(31, Math.round(32 - options.quality / 100 * 30))));
    args.push('-q:v', quantizer, '-pix_fmt', 'yuvj420p', '-f', 'image2pipe', '-c:v', 'mjpeg');
  } else {
    args.push('-f', 'rawvideo', '-pix_fmt', 'rgb24');
  }
  if (isImage) args.push('-frames:v', '1');
  args.push('pipe:1');
  return spawn(executable, args, { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
}

async function convertMedia({ name, bytes, options }, task, resourcesPath) {
  const extension = path.extname(name || '').toLowerCase();
  const isImage = IMAGE_EXTENSIONS.has(extension);
  if (!isImage && !VIDEO_EXTENSIONS.has(extension)) throw new Error(`不支持的媒体格式：${extension || '未知'}`);
  if (!(bytes instanceof Uint8Array) && !Buffer.isBuffer(bytes) && !(bytes instanceof ArrayBuffer)) throw new Error('媒体数据无效');
  const config = validatedOptions(options || {});
  const input = Buffer.from(bytes);
  const temporaryDirectory = await fs.mkdtemp(path.join(os.tmpdir(), 'stm-ips-media-'));
  const inputPath = path.join(temporaryDirectory, `input${extension}`);
  try {
    await fs.writeFile(inputPath, input);
    const executable = await resolveFfmpeg(resourcesPath);
    const process = startFfmpeg(executable, inputPath, config, isImage);
    task.process = process;
    const frames = config.codec === 'mjpeg'
      ? await collectMjpegFrames(process, task)
      : await collectRawFrames(process, config.width, config.height, config.brightness, task);
    if (frames.length > MAX_FRAME_COUNT) throw new Error('容器最多支持 65535 帧；请降低 FPS 或缩短视频');
    const payloadParts = [writeHeader(config.codec === 'mjpeg' ? 'MJPG' : 'RAW5', frames.length, config.width, config.height)];
    if (config.codec === 'mjpeg') for (const frame of frames) { const size = Buffer.allocUnsafe(4); size.writeUInt32LE(frame.length); payloadParts.push(size, frame); }
    else for (const frame of frames) append(payloadParts, frame);
    const payload = Buffer.concat(payloadParts);
    return {
      type: isImage ? 'image' : 'video', width: config.width, height: config.height,
      frame_count: frames.length, frame_size: config.width * config.height * 2,
      fps: isImage ? 30 : config.fps, quality: config.codec === 'mjpeg' ? config.quality : 0,
      codec: config.codec, endian: 'big', hwaccel: 'cpu', compressed_bytes: payload.length,
      bytes: payload,
    };
  } finally {
    task.process = undefined;
    await fs.rm(temporaryDirectory, { recursive: true, force: true });
  }
}

module.exports = { convertMedia };
