import { describe, expect, it } from 'vitest';
import { DeviceSession } from '../js/session.js';

function fakePort() {
  return {
    readable: undefined,
    writable: {
      getWriter() {
        return { write: async () => {}, releaseLock() {} };
      },
    },
    async open() {},
    async close() {},
  };
}

describe('DeviceSession', () => {
  it('enforces activity states around a connected device', async () => {
    const session = new DeviceSession();
    const port = fakePort();
    await session.connect({ requestPort: port as any });
    expect(session.state).toBe('Idle');
    session.transition('Uploading');
    expect(session.state).toBe('Uploading');
    expect(await session.write(new Uint8Array([1]))).toBe(true);
    session.transition('Idle');
    await session.disconnect();
    expect(session.state).toBe('Idle');
  });

  it('rejects invalid activity transitions', () => {
    const session = new DeviceSession();
    session.transition('Deleting');
    expect(() => session.transition('Streaming')).toThrow(/Invalid device state/);
  });
});
