#!/usr/bin/env node

const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');

function projectElectron() {
  try {
    return require('electron');
  } catch (error) {
    return undefined;
  }
}

function systemElectron() {
  if (process.platform !== 'linux') return undefined;
  return ['/usr/lib/electron/electron', '/usr/bin/electron'].find((candidate) => fs.existsSync(candidate));
}

const executable = process.env.STM_IPS_ELECTRON || projectElectron() || systemElectron();
if (!executable) {
  console.error('Electron runtime is unavailable. Run `pnpm install` to install the project Electron runtime.');
  process.exit(1);
}

const childEnv = { ...process.env };
delete childEnv.ELECTRON_RUN_AS_NODE;

const child = spawn(executable, ['.', ...process.argv.slice(2)], {
  cwd: path.resolve(__dirname, '..'),
  env: childEnv,
  stdio: 'inherit',
  windowsHide: false,
});

child.once('error', (error) => {
  console.error(`Unable to start Electron: ${error.message}`);
  process.exitCode = 1;
});
child.once('exit', (code, signal) => {
  process.exitCode = code ?? (signal ? 1 : 0);
});
