export const COMMAND = Object.freeze({
  LARGE_FILE: 0x11,
  SMALL_FILE: 0x45,
  END: 0x14,
  ABORT: 0x15,
  DELETE: 0x19,
  LIST: 0x20,
  BITMAP: 0x21,
  LCD: 0x10,
  LCD_FRAME: 0xa0,
  CONTINUE: 0xa1,
  ERROR: 0xe0,
});

export const ERROR_MESSAGE = Object.freeze({
  1: 'CRC 错误', 2: '未知类型', 3: '大文件区满', 4: '小文件区满',
  5: '类型不匹配', 6: '大文件槽满', 7: '小文件槽满', 8: '索引无效',
  9: '未知命令', 10: 'DMA 失败', 11: 'Flash 写入失败', 12: '写入验证失败',
});

export const LCD_WIDTH = 160;
export const LCD_HEIGHT = 80;
export const LCD_FRAME_SIZE = LCD_WIDTH * LCD_HEIGHT * 2;

const CRC_TABLE = new Uint16Array(256);
for (let byte = 0; byte < 256; byte += 1) {
  let value = byte;
  for (let bit = 0; bit < 8; bit += 1) value = value & 1 ? (value >>> 1) ^ 0xa001 : value >>> 1;
  CRC_TABLE[byte] = value;
}

export function crc16Usb(data, offset = 0, length = data.length - offset) {
  let value = 0xffff;
  for (let index = 0; index < length; index += 1) value = (value >>> 8) ^ CRC_TABLE[(value & 0xff) ^ data[offset + index]];
  return value ^ 0xffff;
}

export function makeHostFrame(command, payload = new Uint8Array(), totalFileSize = 0) {
  const frame = new Uint8Array(11 + payload.length);
  const view = new DataView(frame.buffer);
  frame[0] = 0xbb;
  frame[1] = 0x44;
  frame[2] = command;
  view.setUint32(3, totalFileSize, true);
  view.setUint16(7, payload.length + 2, true);
  frame.set(payload, 9);
  view.setUint16(9 + payload.length, crc16Usb(frame, 0, 9 + payload.length), true);
  return frame;
}

export function createDeviceFrameParser(onFrame, onInvalidFrame) {
  let buffer = new Uint8Array();
  return (chunk) => {
    const next = new Uint8Array(buffer.length + chunk.length);
    next.set(buffer);
    next.set(chunk, buffer.length);
    buffer = next;
    while (buffer.length >= 5) {
      let header = -1;
      for (let index = 0; index <= buffer.length - 2; index += 1) {
        if (buffer[index] === 0xaa && buffer[index + 1] === 0x55) { header = index; break; }
      }
      if (header < 0) { buffer = buffer.length > 1 ? buffer.slice(-1) : buffer; return; }
      if (header > 0) buffer = buffer.slice(header);
      if (buffer.length < 5) return;
      const payloadLength = buffer[3] | (buffer[4] << 8);
      if (payloadLength > 32768) { onInvalidFrame?.(payloadLength); buffer = buffer.slice(1); continue; }
      const frameLength = 5 + payloadLength;
      if (buffer.length < frameLength) return;
      onFrame(buffer[2], buffer.slice(5, frameLength));
      buffer = buffer.slice(frameLength);
    }
  };
}

export function parseFileList(payload) {
  if (payload.length < 2) return [];
  const decoder = new TextDecoder('ascii');
  const slotCount = payload[1];
  let offset = 2;
  for (let index = 0; index < slotCount && offset < payload.length; index += 1) {
    const length = payload[offset];
    if (!length || offset + length > payload.length) return [];
    offset += length;
  }
  const files = [];
  while (offset < payload.length) {
    const length = payload[offset];
    if (length < 12 || offset + length > payload.length) break;
    const tag = payload[offset + 1];
    if (tag === 0xff) break;
    const fileIndex = payload[offset + 2];
    const nameLength = payload[offset + 3];
    if (nameLength > 16 || offset + 12 + nameLength > payload.length) break;
    const view = new DataView(payload.buffer, payload.byteOffset + offset);
    const zone = tag & 0x80 ? 'large' : 'small';
    files.push({
      zone,
      type: tag & 0x7f,
      index: fileIndex,
      name: decoder.decode(payload.slice(offset + 4, offset + 4 + nameLength)).replace(/\0/g, ''),
      address: view.getUint32(4 + nameLength, true),
      size: view.getUint32(8 + nameLength, true),
      sectors: zone === 'large' && offset + 16 + nameLength <= payload.length ? view.getUint32(12 + nameLength, true) : 0,
    });
    offset += length;
  }
  return files;
}

export function bytesToRgb565Canvas(canvas, data, width, height, littleEndian = false) {
  const context = canvas?.getContext('2d');
  if (!context || data.length < width * height * 2) return;
  const image = context.createImageData(width, height);
  for (let pixel = 0; pixel < width * height; pixel += 1) {
    const first = data[pixel * 2];
    const second = data[pixel * 2 + 1];
    const rgb565 = littleEndian ? (second << 8) | first : (first << 8) | second;
    image.data[pixel * 4] = ((rgb565 >> 11) & 0x1f) * 255 / 31;
    image.data[pixel * 4 + 1] = ((rgb565 >> 5) & 0x3f) * 255 / 63;
    image.data[pixel * 4 + 2] = (rgb565 & 0x1f) * 255 / 31;
    image.data[pixel * 4 + 3] = 255;
  }
  context.putImageData(image, 0, 0);
}

export function formatBytes(size) {
  if (size < 1024) return `${size} B`;
  if (size < 1024 * 1024) return `${(size / 1024).toFixed(1)} KB`;
  return `${(size / (1024 * 1024)).toFixed(2)} MB`;
}
