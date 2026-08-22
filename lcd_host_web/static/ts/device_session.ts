export type DeviceState = 'Idle' | 'Streaming' | 'Uploading' | 'Deleting' | 'Error';

export interface SessionPort {
  open(options: Record<string, unknown>): Promise<void>;
  close(): Promise<void>;
  readable?: ReadableStream<Uint8Array>;
  writable?: WritableStream<Uint8Array>;
}

// Typed seam for gradual migration; runtime remains on session.js until the
// Vite build is the default launcher path.
export interface DeviceSessionContract {
  readonly state: DeviceState;
  readonly port: SessionPort | null;
  connect(options?: { baudRate?: number; requestPort?: SessionPort }): Promise<SessionPort>;
  disconnect(): Promise<void>;
  write(data: Uint8Array, timeoutMs?: number): Promise<boolean>;
}
