const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('node:path');
const { convertMedia } = require('./media-service.cjs');

let mainWindow;
let serialSelection;
const mediaTasks = new Map();
function registerSerialSelection(session) {
  session.on('select-serial-port', (event, ports, _contents, callback) => {
    event.preventDefault();
    if (serialSelection) serialSelection.callback('');
    serialSelection = { callback, portIds: new Set(ports.map((port) => port.portId)) };
    mainWindow.webContents.send('serial:selection-request', ports);
  });
  session.setPermissionCheckHandler((_contents, permission) => permission === 'serial');
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1440,
    height: 900,
    minWidth: 1024,
    minHeight: 680,
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });
  registerSerialSelection(mainWindow.webContents.session);
  if (process.env.ELECTRON_RENDERER_URL) mainWindow.loadURL(process.env.ELECTRON_RENDERER_URL);
  else mainWindow.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
}

ipcMain.handle('media:convert', async (_event, request) => {
  const task = { cancelled: false, process: undefined };
  const taskId = crypto.randomUUID();
  mediaTasks.set(taskId, task);
  try {
    const result = await convertMedia(request, task, process.resourcesPath);
    return { ...result, bytes: result.bytes.buffer.slice(result.bytes.byteOffset, result.bytes.byteOffset + result.bytes.byteLength) };
  } finally {
    mediaTasks.delete(taskId);
  }
});
ipcMain.handle('media:cancel-all', () => {
  for (const task of mediaTasks.values()) {
    task.cancelled = true;
    task.process?.kill();
  }
});
ipcMain.handle('serial:select', (_event, portId) => {
  if (!serialSelection) return false;
  const selection = serialSelection;
  serialSelection = undefined;
  selection.callback(selection.portIds.has(portId) ? portId : '');
  return true;
});
ipcMain.handle('serial:cancel-selection', () => {
  if (!serialSelection) return;
  serialSelection.callback('');
  serialSelection = undefined;
});

app.whenReady().then(createWindow);
app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
app.on('window-all-closed', () => app.quit());

