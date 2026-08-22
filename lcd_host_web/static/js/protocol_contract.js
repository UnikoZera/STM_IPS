// Generated from protocol/contract.json. Keep this file browser-loadable.
export const PROTOCOL = Object.freeze({
  version: 1,
  hostHeader: Object.freeze([0xBB, 0x44]),
  hostHeaderSize: 9,
  hostMaxDataBytes: 1024,
  deviceHeader: Object.freeze([0xAA, 0x55]),
  deviceHeaderSize: 5,
  deviceMaxPayloadBytes: 32768,
  commands: Object.freeze({
    downloadLarge: 0x11,
    downloadSmall: 0x45,
    endDownload: 0x14,
    deleteFile: 0x19,
    queryFileList: 0x20,
    lcdStream: 0x10,
    sendBitmap: 0x21,
    abortDownload: 0x15,
    continue: 0xA1,
    error: 0xE0,
    lcdFrame: 0xA0,
  }),
  lcdStream: Object.freeze({ stop: 0x00, start: 0x01 }),
});
