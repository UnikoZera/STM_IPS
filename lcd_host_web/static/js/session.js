// DeviceSession owns the Web Serial lifecycle and exposes one explicit device state.
export const DEVICE_STATES = Object.freeze({
  IDLE: 'Idle',
  STREAMING: 'Streaming',
  UPLOADING: 'Uploading',
  DELETING: 'Deleting',
  ERROR: 'Error',
});

const VALID_TRANSITIONS = Object.freeze({
  Idle: new Set(['Idle', 'Streaming', 'Uploading', 'Deleting', 'Error']),
  Streaming: new Set(['Idle', 'Uploading', 'Deleting', 'Error']),
  Uploading: new Set(['Idle', 'Streaming', 'Error']),
  Deleting: new Set(['Idle', 'Error']),
  Error: new Set(['Idle', 'Error']),
});

export class DeviceSession {
  constructor({ onData = () => {}, onStateChange = () => {} } = {}) {
    this.port = null;
    this.reader = null;
    this.readLoop = false;
    this.state = DEVICE_STATES.IDLE;
    this.onData = onData;
    this.onStateChange = onStateChange;
  }

  transition(next, error = null) {
    if (!VALID_TRANSITIONS[this.state]?.has(next)) {
      throw new Error(`Invalid device state transition: ${this.state} -> ${next}`);
    }
    this.state = next;
    this.onStateChange(next, error);
  }

  async connect({ baudRate = 921600, requestPort = null } = {}) {
    if (this.port) throw new Error('Device is already connected');
    const port = requestPort || await navigator.serial.requestPort();
    try {
      await port.open({ baudRate, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' });
      this.port = port;
      this.transition(DEVICE_STATES.IDLE);
      return port;
    } catch (error) {
      this.transition(DEVICE_STATES.ERROR, error);
      throw error;
    }
  }

  async startRead() {
    if (!this.port?.readable || this.readLoop) return;
    this.readLoop = true;
    while (this.port?.readable && this.readLoop) {
      try {
        this.reader = this.port.readable.getReader();
        while (this.readLoop) {
          const { value, done } = await this.reader.read();
          if (done) break;
          if (value?.length) this.onData(value);
        }
      } catch (error) {
        if (this.readLoop) {
          this.transition(DEVICE_STATES.ERROR, error);
          throw error;
        }
      } finally {
        try { this.reader?.releaseLock(); } catch (_) {}
        this.reader = null;
      }
      if (this.readLoop) await new Promise(resolve => setTimeout(resolve, 50));
    }
  }

  async write(data, timeoutMs = 8000) {
    if (!this.port?.writable) return false;
    const writer = this.port.writable.getWriter();
    let timer;
    try {
      await Promise.race([
        writer.write(data),
        new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('Serial write timeout')), timeoutMs); }),
      ]);
      return true;
    } catch (error) {
      this.transition(DEVICE_STATES.ERROR, error);
      throw error;
    } finally {
      clearTimeout(timer);
      try { writer.releaseLock(); } catch (_) {}
    }
  }

  async disconnect() {
    this.readLoop = false;
    try { await this.reader?.cancel(); } catch (_) {}
    try { this.reader?.releaseLock(); } catch (_) {}
    this.reader = null;
    if (this.port) {
      try { await this.port.close(); } catch (_) {}
    }
    this.port = null;
    this.transition(DEVICE_STATES.IDLE);
  }
}
