const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('stmHost', {
  media: {
    convert: (request) => ipcRenderer.invoke('media:convert', request),
    cancelAll: () => ipcRenderer.invoke('media:cancel-all'),
  },
  selectSerialPort: (portId) => ipcRenderer.invoke('serial:select', portId),
  cancelSerialSelection: () => ipcRenderer.invoke('serial:cancel-selection'),
  onSerialSelectionRequest(listener) {
    const handler = (_event, ports) => listener(ports);
    ipcRenderer.on('serial:selection-request', handler);
    return () => ipcRenderer.removeListener('serial:selection-request', handler);
  },
});
