export async function convertMedia(file, options) {
  if (!window.stmHost?.media) throw new Error('媒体转码只能在 STM IPS Electron 应用中使用');
  const bytes = await file.arrayBuffer();
  const result = await window.stmHost.media.convert({
    name: file.name,
    bytes,
    options,
  });
  return { ...result, bytes: new Uint8Array(result.bytes) };
}

export function indexFrames(result) {
  const data = result.bytes;
  if (!data?.length) return [];
  if (result.codec === 'mjpeg') {
    const frames = [];
    for (let offset = 14; offset + 4 <= data.length;) {
      const size = new DataView(data.buffer, data.byteOffset + offset, 4).getUint32(0, true);
      offset += 4;
      if (!size || offset + size > data.length) break;
      frames.push([offset, offset + size]);
      offset += size;
    }
    return frames;
  }
  const hasHeader = data.length >= 14 && String.fromCharCode(...data.slice(0, 4)) === 'RAW5';
  const start = hasHeader ? 14 : 0;
  const width = hasHeader ? data[6] | (data[7] << 8) : result.width;
  const height = hasHeader ? data[8] | (data[9] << 8) : result.height;
  const frameSize = width * height * 2;
  const frames = [];
  for (let offset = start; offset + frameSize <= data.length; offset += frameSize) frames.push([offset, offset + frameSize]);
  return frames;
}

export async function decodePreviewFrame(result, index = 0) {
  const frames = indexFrames(result);
  const range = frames[index];
  if (!range) return null;
  if (result.codec !== 'mjpeg') return result.bytes.slice(range[0], range[1]);
  const bitmap = await createImageBitmap(new Blob([result.bytes.slice(range[0], range[1])], { type: 'image/jpeg' }));
  const canvas = document.createElement('canvas');
  canvas.width = result.width;
  canvas.height = result.height;
  const context = canvas.getContext('2d', { willReadFrequently: true });
  context.drawImage(bitmap, 0, 0);
  bitmap.close();
  const rgba = context.getImageData(0, 0, result.width, result.height).data;
  const rgb565 = new Uint8Array(result.width * result.height * 2);
  for (let pixel = 0; pixel < result.width * result.height; pixel += 1) {
    const value = ((rgba[pixel * 4] >> 3) << 11) | ((rgba[pixel * 4 + 1] >> 2) << 5) | (rgba[pixel * 4 + 2] >> 3);
    rgb565[pixel * 2] = value >> 8;
    rgb565[pixel * 2 + 1] = value & 0xff;
  }
  return rgb565;
}
