<script>
  import { onMount, tick } from 'svelte';
  import { COMMAND, ERROR_MESSAGE, LCD_FRAME_SIZE, LCD_HEIGHT, LCD_WIDTH, bytesToRgb565Canvas, formatBytes, makeHostFrame, parseFileList } from './lib/protocol.js';
  import { SerialClient } from './lib/serial.js';
  import { convertMedia, decodePreviewFrame } from './lib/media.js';

  const SEND_TIMEOUT = 15000;
  let serial;
  let connected = false;
  let baudRate = 921600;
  let logs = [];
  let files = [];
  let selectedFile;
  let uploadName = '';
  let fileType = COMMAND.LARGE_FILE;
  let chunkSize = 512;
  let sending = false;
  let sendProgress = 0;
  let sendDetail = '';
  let continuation;
  let transferFailure;
  let lcdEnabled = false;
  let lcdCanvas;
  let lcdLastFrameAt = 0;
  let previewCanvas;
  let mediaFile;
  let mediaResult;
  let mediaBusy = false;
  let mediaProgress = 0;
  let mediaStatus = '';
  let mediaWidth = 160;
  let mediaHeight = 80;
  let mediaFps = 30;
  let mediaBrightness = 100;
  let mediaCodec = 'mjpeg';
  let mediaQuality = 70;
  let bitmap;
  let bitmapModal = false;
  let serialChoices = [];
  let serialChoiceModal = false;
  let theme = localStorage.getItem('stm_ips_theme') || 'dark';

  $: document.documentElement.dataset.theme = theme;
  $: fileNameBytes = new TextEncoder().encode(uploadName).length;
  $: mediaIsVideo = mediaFile && /\.(mp4|webm|mkv|avi|mov|flv|wmv|gif)$/i.test(mediaFile.name);

  function log(message, level = 'info') {
    logs = [...logs.slice(-299), { message, level, time: new Date().toLocaleTimeString() }];
  }

  function resetContinuation(error) {
    if (!continuation) return;
    const { reject, resolve } = continuation;
    continuation = undefined;
    if (error) reject(error);
    else resolve();
  }

  function waitContinuation() {
    return new Promise((resolve, reject) => {
      const done = () => {
        clearTimeout(timer);
        continuation = undefined;
        resolve();
      };
      const timer = setTimeout(() => {
        if (continuation?.resolve === done) continuation = undefined;
        reject(new Error('等待 MCU 就绪超时'));
      }, SEND_TIMEOUT);
      continuation = {
        resolve: done,
        reject: (error) => { clearTimeout(timer); continuation = undefined; reject(error); },
      };
    });
  }

  function handleFrame(command, payload) {
    if (command === COMMAND.CONTINUE) {
      if (sending) continuation?.resolve();
      else log('MCU → 就绪 (0xA1)', 'receive');
      return;
    }
    if (command === COMMAND.ERROR) {
      const code = payload[0] || 0;
      const message = ERROR_MESSAGE[code] || `未知错误 0x${code.toString(16)}`;
      log(`MCU → 错误：${message}`, 'error');
      if (sending) resetContinuation(new Error(message));
      return;
    }
    if (command === COMMAND.LIST) {
      files = parseFileList(payload);
      log(`MCU → 文件列表：${files.length} 个文件`, 'receive');
      return;
    }
    if (command === COMMAND.LCD) {
      lcdEnabled = Boolean(payload[0]);
      log(`MCU → LCD 流${lcdEnabled ? '已开启' : '已关闭'}`, 'receive');
      return;
    }
    if (command === COMMAND.BITMAP) {
      bitmap = payload.slice();
      bitmapModal = true;
      log('MCU → 已接收 Flash 位图', 'receive');
      tick().then(drawBitmap);
      return;
    }
    if (command === COMMAND.LCD_FRAME) {
      if (payload.length >= LCD_FRAME_SIZE) {
        bytesToRgb565Canvas(lcdCanvas, payload, LCD_WIDTH, LCD_HEIGHT);
        lcdLastFrameAt = Date.now();
        lcdEnabled = true;
      }
      return;
    }
    log(`MCU → 命令 0x${command.toString(16).padStart(2, '0').toUpperCase()} (${payload.length} B)`, 'receive');
  }

  async function connect() {
    try {
      await serial.connect(Number(baudRate));
      connected = true;
      log(`串口已连接 @ ${baudRate} bps`);
      await setLcdStream(true);
      await queryFiles();
    } catch (error) {
      log(`串口连接失败：${error.message}`, 'error');
    }
  }

  async function disconnect() {
    resetContinuation();
    await serial.disconnect();
    connected = false;
    lcdEnabled = false;
    log('串口已断开');
  }

  async function write(command, payload = new Uint8Array(), totalSize = 0) {
    if (!serial.connected) throw new Error('未连接串口');
    const ok = await serial.write(makeHostFrame(command, payload, totalSize));
    if (!ok) throw new Error('串口写入失败');
  }

  async function queryFiles() {
    try {
      await write(COMMAND.LIST);
      log('Host → 查询文件列表', 'send');
    } catch (error) { log(error.message, 'error'); }
  }

  async function deleteFile(file) {
    if (!confirm(`删除 ${file.zone === 'large' ? '大' : '小'}文件 #${file.index}：${file.name}？`)) return;
    try {
      await write(COMMAND.DELETE, new Uint8Array([file.type, file.index]));
      log(`Host → 删除 ${file.name}`, 'send');
      setTimeout(queryFiles, 500);
    } catch (error) { log(error.message, 'error'); }
  }

  async function setLcdStream(enabled) {
    try {
      await write(COMMAND.LCD, new Uint8Array([enabled ? 1 : 0]));
      log(`Host → ${enabled ? '开启' : '关闭'} LCD 流`, 'send');
    } catch (error) { log(error.message, 'error'); }
  }

  function selectUpload(event) {
    selectedFile = event.currentTarget.files?.[0];
    if (selectedFile) uploadName = selectedFile.name.replace(/\.[^.]+$/, '').slice(0, 16);
  }

  async function sendSelectedFile() {
    if (!selectedFile || !connected || sending) return;
    if (!uploadName || fileNameBytes > 16) { log('设备文件名必须是 1–16 字节', 'error'); return; }
    if (!confirm(`发送 ${selectedFile.name} 到设备，名称：${uploadName}？`)) return;
    sending = true;
    sendProgress = 0;
    const wasStreaming = lcdEnabled;
    try {
      if (wasStreaming) await setLcdStream(false);
      const bytes = new Uint8Array(await selectedFile.arrayBuffer());
      for (let offset = 0; offset < bytes.length; offset += chunkSize) {
        const block = bytes.slice(offset, Math.min(offset + chunkSize, bytes.length));
        await write(fileType, block, offset === 0 ? bytes.length : 0);
        sendDetail = `${formatBytes(offset + block.length)} / ${formatBytes(bytes.length)}`;
        sendProgress = Math.round((offset + block.length) / bytes.length * 100);
        await waitContinuation();
      }
      const namePayload = new Uint8Array(16);
      namePayload.set(new TextEncoder().encode(uploadName));
      await write(COMMAND.END, namePayload);
      await waitContinuation();
      log(`发送完成：${uploadName}`, 'success');
      await queryFiles();
    } catch (error) {
      log(`传输失败：${error.message}`, 'error');
      try { await write(COMMAND.ABORT); } catch { /* connection already failed */ }
    } finally {
      sending = false;
      sendDetail = '';
      if (wasStreaming && connected) await setLcdStream(true);
    }
  }

  async function cancelTransfer() {
    resetContinuation();
    try { await write(COMMAND.ABORT); } catch { /* connection already failed */ }
    sending = false;
    sendProgress = 0;
    log('Host → 已中止传输', 'warning');
  }

  function selectMedia(event) {
    mediaFile = event.currentTarget.files?.[0];
    mediaResult = undefined;
    mediaStatus = '';
  }

  async function convert() {
    if (!mediaFile || mediaBusy) return;
    mediaBusy = true;
    mediaProgress = 15;
    mediaStatus = '正在转换…';
    try {
      const result = await convertMedia(mediaFile, {
        width: mediaWidth, height: mediaHeight, fps: mediaFps, brightness: mediaBrightness,
        codec: mediaCodec, quality: mediaQuality, swap: 1,
      });
      mediaProgress = 85;
      mediaResult = result;
      await tick();
      const preview = await decodePreviewFrame(result);
      if (preview) bytesToRgb565Canvas(previewCanvas, preview, result.width, result.height);
      mediaProgress = 100;
      mediaStatus = `转换完成：${result.frame_count} 帧，${formatBytes(result.bytes?.length || 0)}`;
      log(mediaStatus, 'success');
    } catch (error) {
      mediaStatus = `转换失败：${error.message}`;
      log(mediaStatus, 'error');
    } finally {
      mediaBusy = false;
    }
  }

  function useMediaForTransfer() {
    if (!mediaResult?.bytes) return;
    selectedFile = new File([mediaResult.bytes], `${mediaFile?.name.replace(/\.[^.]+$/, '') || 'media'}.bin`, { type: 'application/octet-stream' });
    uploadName = mediaResult.type === 'video' ? 'vp_vid' : 'vp_img';
    fileType = mediaResult.type === 'video' || selectedFile.size > 6 * 1024 ? COMMAND.LARGE_FILE : COMMAND.SMALL_FILE;
    log('转换结果已填入烧录区，请确认名称后发送', 'success');
  }

  async function queryBitmap() {
    try {
      await write(COMMAND.BITMAP);
      log('Host → 查询 Flash 位图', 'send');
    } catch (error) { log(error.message, 'error'); }
  }

  function drawBitmap() {
    const canvas = document.querySelector('#bitmap-canvas');
    if (!canvas || !bitmap) return;
    const columns = 64, rows = 62, cell = 7;
    canvas.width = columns * cell;
    canvas.height = rows * cell;
    const context = canvas.getContext('2d');
    for (let index = 0; index < columns * rows; index += 1) {
      const used = Boolean(bitmap[Math.floor(index / 8)] & (1 << (index % 8)));
      context.fillStyle = used ? '#8b5cf6' : '#34d399';
      context.fillRect((index % columns) * cell, Math.floor(index / columns) * cell, cell - 1, cell - 1);
    }
  }

  function selectElectronPort(port) {
    window.stmHost.selectSerialPort(port.portId);
    serialChoiceModal = false;
  }

  function toggleTheme() {
    theme = theme === 'dark' ? 'light' : 'dark';
    localStorage.setItem('stm_ips_theme', theme);
  }

  onMount(() => {
    serial = new SerialClient({ onFrame: handleFrame, onLog: log });
    const unsubscribe = window.stmHost?.onSerialSelectionRequest((ports) => {
      serialChoices = ports;
      serialChoiceModal = true;
    });
    const watchdog = setInterval(() => {
      if (lcdEnabled && lcdLastFrameAt && Date.now() - lcdLastFrameAt > 1200) lcdEnabled = false;
    }, 400);
    return () => { unsubscribe?.(); clearInterval(watchdog); serial?.disconnect(); };
  });
</script>

<svelte:head><title>STM IPS Host</title></svelte:head>

<header>
  <div><strong>STM IPS Host</strong><span class="subtitle">Electron · Svelte · 本地 FFmpeg 转码</span></div>
  <div class:connected class="connection"><span></span>{connected ? `在线 @ ${baudRate}` : '脱机'}</div>
  <button class="icon" onclick={toggleTheme} title="切换主题">{theme === 'dark' ? '☀' : '☾'}</button>
</header>

<main>
  <aside class="controls">
    <section>
      <h2>串口连接</h2>
      <label>波特率<select bind:value={baudRate}><option>115200</option><option>460800</option><option>921600</option><option>1000000</option><option>1500000</option><option>2000000</option></select></label>
      {#if connected}<button class="danger" onclick={disconnect}>断开连接</button>{:else}<button class="primary" onclick={connect}>连接 STM32</button>{/if}
    </section>

    <section>
      <h2>文件烧录</h2>
      <label>存储类型<select bind:value={fileType}><option value={COMMAND.LARGE_FILE}>大文件 · 0x11</option><option value={COMMAND.SMALL_FILE}>小文件 · 0x45</option></select></label>
      <label>数据块<input type="number" bind:value={chunkSize} min="1" max="2048" step="16" /></label>
      <label>设备文件名 <input bind:value={uploadName} maxlength="16" class:invalid={fileNameBytes > 16} /><small>{fileNameBytes} / 16 字节</small></label>
      <label class="file-input">选择文件<input type="file" onchange={selectUpload} /></label>
      {#if selectedFile}<p class="hint">{selectedFile.name} · {formatBytes(selectedFile.size)}</p>{/if}
      {#if sending}
        <progress value={sendProgress} max="100"></progress><p class="hint">{sendProgress}% {sendDetail}</p><button class="danger" onclick={cancelTransfer}>中止传输</button>
      {:else}<button class="primary" disabled={!connected || !selectedFile || fileNameBytes > 16} onclick={sendSelectedFile}>发送到设备</button>{/if}
    </section>

    <section>
      <h2>设备管理</h2>
      <div class="row"><button onclick={queryFiles} disabled={!connected}>刷新文件</button><button onclick={queryBitmap} disabled={!connected}>Flash Map</button></div>
    </section>

    <section>
      <h2>媒体转码</h2>
      <label class="file-input">图片或视频<input type="file" accept="image/*,.mp4,.webm,.mkv,.avi,.mov,.flv,.wmv" onchange={selectMedia} /></label>
      {#if mediaFile}<p class="hint">{mediaFile.name} · {formatBytes(mediaFile.size)}</p>{/if}
      <div class="grid"><label>宽<input type="number" bind:value={mediaWidth} min="1" max="1024" /></label><label>高<input type="number" bind:value={mediaHeight} min="1" max="1024" /></label></div>
      {#if mediaBusy}<progress value={mediaProgress} max="100"></progress><button class="danger" onclick={() => window.stmHost?.media.cancelAll()}>取消转换</button>{:else}<button class="primary" disabled={!mediaFile} onclick={convert}>开始转换</button>{/if}
      <label>亮度<input type="range" bind:value={mediaBrightness} min="10" max="300" /><small>{mediaBrightness}%</small></label>
      <label>编码<select bind:value={mediaCodec}><option value="mjpeg">MJPEG 压缩</option><option value="raw">RGB565 原图</option></select></label>
      {#if mediaCodec === 'mjpeg'}<label>JPEG 质量<input type="range" bind:value={mediaQuality} min="1" max="100" /><small>{mediaQuality}</small></label>{/if}
      {#if mediaBusy}<progress value={mediaProgress} max="100"></progress><button class="danger" onclick={() => window.stmHost?.media.cancelAll()}>取消转换</button>{:else}<button class="primary" disabled={!mediaFile} onclick={convert}>开始转换</button>{/if}
      {#if mediaStatus}<p class="hint">{mediaStatus}</p>{/if}
      {#if mediaResult}<canvas bind:this={previewCanvas} width={mediaResult.width} height={mediaResult.height} class="preview"></canvas><button onclick={useMediaForTransfer}>使用此结果烧录</button>{/if}
    </section>
  </aside>

  <section class="log-panel"><h2>通信日志 <button class="text" onclick={() => logs = []}>清除</button></h2><div class="logs">{#each logs as entry}<p class={entry.level}><time>{entry.time}</time> {entry.message}</p>{/each}</div></section>

  <aside class="device">
    <section><h2>LCD 屏幕流 <span class:active={lcdEnabled} class="stream-state">{lcdEnabled ? '● 已开启' : '○ 已关闭'}</span></h2><div class="row"><button class="primary" disabled={!connected} onclick={() => setLcdStream(true)}>开启</button><button class="danger" disabled={!connected} onclick={() => setLcdStream(false)}>关闭</button></div><div class="lcd"><canvas bind:this={lcdCanvas} width="160" height="80"></canvas></div></section>
    <section><h2>设备文件 <span class="count">{files.length}</span></h2>{#if files.length === 0}<p class="hint">连接设备后刷新列表</p>{:else}<div class="files">{#each files as file}<article><div><strong>[{file.index}] {file.name || '无名文件'}</strong><small>{file.zone === 'large' ? `大文件 · 扇区 #${file.address} · ${file.sectors} sectors` : `小文件 · 0x${file.address.toString(16)}`}</small><small>{formatBytes(file.size)}</small></div><button class="danger" onclick={() => deleteFile(file)}>删除</button></article>{/each}</div>{/if}</section>
  </aside>
</main>

{#if serialChoiceModal}
  <div class="modal"><div class="dialog"><h2>选择串口</h2>{#if serialChoices.length}{#each serialChoices as port}<button class="port" onclick={() => selectElectronPort(port)}>{port.displayName || port.portName || port.portId}<small>{port.portName || port.portId}</small></button>{/each}{:else}<p>未发现可用串口。</p>{/if}<button onclick={() => { window.stmHost.cancelSerialSelection(); serialChoiceModal = false; }}>取消</button></div></div>
{/if}

{#if bitmapModal}
  <div class="modal" role="presentation" onclick={() => bitmapModal = false}><div class="dialog bitmap" role="dialog" tabindex="-1" onkeydown={(event) => event.stopPropagation()} onclick={(event) => event.stopPropagation()}><h2>Flash 大文件区位图</h2><canvas id="bitmap-canvas"></canvas><p class="hint">绿色为空闲，紫色为已占用；区域从 Flash 扇区 #64 开始。</p><button onclick={() => bitmapModal = false}>关闭</button></div></div>
{/if}
