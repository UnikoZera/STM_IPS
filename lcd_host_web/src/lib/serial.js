import { createDeviceFrameParser } from './protocol.js';

export class SerialClient {
  constructor({ onFrame, onLog }) {
    this.port = null;
    this.reader = null;
    this.reading = false;
    this.onLog = onLog;
    this.parse = createDeviceFrameParser(onFrame, (length) => onLog(`无效设备帧长度 ${length}`, 'error'));
  }

  get connected() { return Boolean(this.port?.writable); }

  async connect(baudRate) {
    if (!('serial' in navigator)) throw new Error('此运行环境不支持 Web Serial');
    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' });
    this.reading = true;
    void this.readLoop();
  }

  async disconnect() {
    this.reading = false;
    if (this.reader) {
      try { await this.reader.cancel(); } catch { /* already closed */ }
      try { this.reader.releaseLock(); } catch { /* already released */ }
      this.reader = null;
    }
    if (this.port) {
      try { await this.port.close(); } catch { /* already closed */ }
      this.port = null;
    }
  }

  async readLoop() {
    while (this.port?.readable && this.reading) {
      try {
        this.reader = this.port.readable.getReader();
        while (this.reading) {
          const { value, done } = await this.reader.read();
          if (done) break;
          if (value?.length) this.parse(value);
        }
      } catch (error) {
        if (this.reading) this.onLog(`串口读取失败：${error.message}`, 'error');
      } finally {
        try { this.reader?.releaseLock(); } catch { /* already released */ }
        this.reader = null;
      }
    }
  }

  async write(data) {
    if (!this.port?.writable) return false;
    const writer = this.port.writable.getWriter();
    try {
      await writer.write(data);
      return true;
    } catch (error) {
      this.onLog(`串口写入失败：${error.message}`, 'error');
      return false;
    } finally {
      writer.releaseLock();
    }
  }
}
