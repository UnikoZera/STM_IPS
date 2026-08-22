// videoproc.js — 视频/图片转码（服务端 ffmpeg）+ 结果预览与烧录回填
import {
  $, state, elFileInfo, elName, elBtnSend,
  C_LCD, hexToBytes, log, toast,
} from './core.js';
import { frame } from './protocol.js';
import { writeSer } from './serial.js';
import { updateNBC } from './transfer.js';

/* ===== Video Processor ===== */
const VP_SERVER = (window.location.protocol === 'file:')
  ? 'http://127.0.0.1:5000'
  : window.location.origin.replace(/:(\d+)/, function(m,p){ return ':'+(p==='80'||p==='443'?'5000':p); });

const VP = {
  file: null,
  fileBytes: null,
  result: null,
  jobId: null,
  abort: false,
  frames: [],          // 预览帧槽：完整帧数，但按需解码（未解码为 null）
  frameIndex: [],      // MJPEG 压缩流中每帧 [start,end) 偏移
  compressedBytes: null, // 完整烧录 payload（二进制，禁止用超大 hex）
  curFrame: 0,
  decodeGen: 0,        // 取消在途解码
};
const $vp = id => document.getElementById('vp'+id);
const isVPVideoName = name => /\.(mp4|webm|mkv|avi|mov|flv|wmv|gif)$/i.test(name);

export function onVPFileSelected(input){
  const f = input.files[0];
  if(!f) return;
  VP.file = f;
  VP.result = null;
  VP.jobId = null;
  VP._thumb = null;
  VP.frames = [];
  VP.frameIndex = [];
  VP.compressedBytes = null;
  VP.curFrame = 0;
  VP.decodeGen++;
  $vp('Result').style.display = 'none';
  $vp('Status').textContent = '';
  $vp('ProgWrap').style.display = 'none';
  $vp('ProgBar').style.width = '0';
  $vp('ProgPct').textContent = '';
  $vp('ProgInfo').textContent = '';

  const isVideo = isVPVideoName(f.name);
  const fpsRow = $vp('FpsRow');
  if(fpsRow) fpsRow.style.display = isVideo ? 'flex' : 'none';
  const sz = f.size < 1024 ? f.size+'B' : f.size < 1048576 ? (f.size/1024).toFixed(1)+'KB' : (f.size/1048576).toFixed(2)+'MB';
  const icon = isVideo ? '&#127909;' : '&#128247;';
  $vp('FileInfo').innerHTML = icon+' '+f.name+' ('+sz+')';
  $vp('Process').disabled = false;
  if(isVideo && f.size > 80*1024*1024){
    toast('源视频较大，转换会占用较多内存/磁盘；建议适当降低 FPS 或分辨率','warn');
  }
}

let vpAbortController = null;
let vpLastProgress = 0;

export async function startVPProcess(){
  if(!VP.file){ toast('请选择文件','error'); return; }
  VP.abort = false;

  const w = parseInt($vp('Width').value) || 160;
  const h = parseInt($vp('Ht').value) || 80;
  const fps = Math.max(1, Math.min(60, parseFloat($vp('Fps').value) || 30));
  const isVideo = isVPVideoName(VP.file.name);

  if(w < 1 || w > 1024 || h < 1 || h > 1024){
    toast('尺寸范围: 1-1024','error'); return;
  }

  // 转换期间尽量关掉 LCD USB 推流，减少浏览器+串口额外负载
  let _lcdWasOn = false;
  try{
    if(typeof lcdStreamOn !== 'undefined' && lcdStreamOn){
      _lcdWasOn = true;
      if(typeof stopLcdStream === 'function') await stopLcdStream();
      else if(typeof writeSer === 'function') await writeSer(frame(C_LCD, new Uint8Array([0x00])));
    }
  }catch(e){}

  $vp('Process').style.display = 'none';
  $vp('Cancel').style.display = 'block';
  $vp('Status').innerHTML = '<span class="spinner"></span> 正在转换...';
  $vp('ProgWrap').style.display = 'block';
  $vp('Result').style.display = 'none';
  vpLastProgress = 0;
  updateVPProg(0);

  const form = new FormData();
  form.append('file', VP.file);
  form.append('width', w);
  form.append('height', h);
  form.append('fps', fps);
  form.append('swap', '1');
  form.append('brightness', parseInt($('vpBrightness').value) || 100);
  const codec = $('vpCodec').value;
  form.append('codec', codec);
  if (codec === 'mjpeg') {
    form.append('quality', parseInt($('vpMjpegQuality').value) || 60);
  }

  const SERVER = VP_SERVER;

  vpAbortController = new AbortController();

  try{
    const resp = await fetch(SERVER+'/convert/jobs', {
      method: 'POST',
      body: form,
      signal: vpAbortController.signal,
    });
    if(!resp.ok){
      const err = await resp.json().catch(()=>({error:'HTTP '+resp.status}));
      if(resp.status === 429){
        throw new Error(err.error || err.detail || '转换忙，请稍后再试');
      }
      throw new Error(err.error || err.detail || 'Server error');
    }

    const queued = await resp.json();
    VP.jobId = queued.job_id;
    let job;
    do {
      await new Promise(resolve => setTimeout(resolve, 250));
      const statusResp = await fetch(SERVER+'/convert/jobs/'+VP.jobId, { signal: vpAbortController.signal });
      if(!statusResp.ok) throw new Error('Job status HTTP '+statusResp.status);
      job = await statusResp.json();
      updateVPProg(job.progress || 0);
      const frameInfo = job.processed_frames
        ? (job.total_frames
          ? `（${job.processed_frames}/${job.total_frames} 帧）`
          : `（已处理 ${job.processed_frames} 帧）`)
        : '';
      $vp('ProgInfo').textContent = (job.detail || (job.status === 'running' ? '正在转码...' : job.status)) + frameInfo;
    } while(job.status === 'queued' || job.status === 'running' || job.status === 'cancelling');
    if(job.status !== 'completed') throw new Error(job.error || 'Conversion '+job.status);
    VP.result = job.result;
    VP.frames = [];
    VP.frameIndex = [];
    VP.curFrame = 0;
    VP.compressedBytes = null;
    VP.decodeGen++;

    // 优先 download_id 拉完整二进制；禁止依赖超大 compressed_hex
    if(VP.result.download_id){
      $vp('Status').innerHTML = '<span class="spinner"></span> 正在拉取完整结果...';
      $vp('ProgInfo').textContent = '(下载中)';
      const binResp = await fetch(SERVER+'/download/'+VP.result.download_id, {
        signal: vpAbortController.signal,
      });
      if(!binResp.ok) throw new Error('下载转换结果失败 HTTP '+binResp.status);
      VP.compressedBytes = new Uint8Array(await binResp.arrayBuffer());
      updateVPProg(98);
    } else if(VP.result.compressed_hex){
      VP.compressedBytes = hexToBytes(VP.result.compressed_hex);
    }

    // 缩略图（若服务端给了少量 preview_hex）
    if(VP.result.hex){
      try{
        const raw = hexToBytes(VP.result.hex);
        const fs = VP.result.frame_size || (VP.result.width*VP.result.height*2);
        if(fs && raw.length >= fs){
          // 仅作首帧 fallback，真正完整预览走按需解码
          VP._thumb = raw.subarray(0, fs);
        }
      }catch(e){}
    }

    $vp('Status').innerHTML = '<span class="spinner"></span> 正在建立帧索引...';
    $vp('ProgInfo').textContent = '(完整预览按需解码)';
    updateVPProg(99);

    await showVPResult();

    updateVPProg(100);
    $vp('Status').innerHTML = '&#10003; 转换完成: '+VP.result.frame_count+' 帧（完整）';
    toast('转换完成: '+VP.result.frame_count+' 帧','success');
  } catch(e){
    stopVpProgSim();
    if(e.name === 'AbortError'){
      $vp('Status').textContent = '已取消';
      toast('已取消','warn');
    } else {
      $vp('Status').innerHTML = '&#10007; 转换失败';
      log('转换错误: '+e.message,'error');
      toast('转换失败: '+e.message,'error');
    }
    $vp('Process').style.display = 'block';
    $vp('Cancel').style.display = 'none';
    $vp('ProgPct').textContent = '';
    $vp('ProgInfo').textContent = '';
  }
}

function updateVPProg(pct){
  const bar = $vp('ProgBar');
  const lbl = $vp('ProgPct');
  if(!bar) return;
  pct = Number.isFinite(Number(pct)) ? Math.max(0, Math.min(100, Number(pct))) : 0;
  vpLastProgress = Math.max(vpLastProgress, pct);
  pct = vpLastProgress;
  bar.style.width = pct+'%';
  if(lbl) lbl.textContent = pct.toFixed(1)+'%';
  // color: 0% red → 50% gold → 100% green
  const r = pct < 50 ? 255 : Math.round(255 * (1 - (pct-50)/50));
  const g = pct < 50 ? Math.round(171 * (pct/50)) : Math.round(171 * (1 - (pct-50)/50) + 230 * ((pct-50)/50));
  const b = pct < 50 ? 0 : Math.round(118 * ((pct-50)/50));
  bar.style.background = 'rgb('+r+','+g+','+b+')';
  bar.style.boxShadow = '0 0 10px rgba('+r+','+g+','+b+',.35)';
}

let vpProgTimer = null;

function startVpProgSim(){
  stopVpProgSim();
  let pct = 10;
  updateVPProg(pct);
  // 降低进度 UI 刷新频率，转换期间少占主线程
  vpProgTimer = setInterval(function(){
    pct += (90 - pct) * 0.04;
    if(pct >= 88) pct = 88;
    updateVPProg(pct);
  }, 800);
}

function stopVpProgSim(){
  if(vpProgTimer){ clearInterval(vpProgTimer); vpProgTimer = null; }
}

export function cancelVPProcess(){
  if(VP.jobId){
    fetch(VP_SERVER+'/convert/jobs/'+VP.jobId+'/cancel', {method:'POST'}).catch(()=>{});
  }
  if(vpAbortController){
    vpAbortController.abort();
    vpAbortController = null;
  }
  VP.abort = true;
}

async function showVPResult(){
  const r = VP.result;
  if(!r){ return; }

  const isVid = r.type === 'video';
  const totalBytes = r.frame_count * r.frame_size;
  const totalKB = (totalBytes / 1024).toFixed(1);
  let info = '分辨率: '+r.width+'x'+r.height+' | 帧数: '+r.frame_count+' | 原始: '+totalKB+' KB' + (r.endian ? ' | '+r.endian : '');
  if(isVid && r.fps) info += ' | FPS: '+Number(r.fps).toFixed(1);
  if(r.hwaccel){
    const avail = r.hwaccel_available || '';
    info += ' | 加速: '+r.hwaccel + (avail && avail!==r.hwaccel ? ' (可用:'+avail+')' : '');
  }
  $vp('ResultInfo').textContent = info;

  // 建立完整帧索引 + 槽位；不一次性解码全部帧（防止 OOM / 整机重启）
  if(getVPCompressedBytes()){
    buildVPFrameIndex();
    // 预热首帧，保证缩略图与弹窗可立即使用
    await ensureVPFrame(0);
  } else if(VP._thumb){
    VP.frames = [VP._thumb];
    VP.frameIndex = [[0, VP._thumb.length]];
  }

  const compBytes = VP.compressedBytes
    ? VP.compressedBytes.length
    : (r.compressed_bytes || (r.compressed_hex ? r.compressed_hex.length/2 : 0));
  if(compBytes > 0){
    const ratio = totalBytes > 0 ? (totalBytes / compBytes) : 0;
    const qs = r.quality || 'N/A';
    const codecLabel = r.codec === 'mjpeg' ? 'MJPEG压缩' : (r.codec === 'raw' ? '原图(RGB565)' : '未知编码');
    $vp('CompressStats').innerHTML =
      '<span style="color:var(--gold);">\u21BB '+codecLabel+'</span> ' +
      '<span style="color:var(--text2);">| 质量:'+qs+' |</span> ' +
      '<span style="color:var(--cyan);">压缩后:'+compBytes+' B</span> ' +
      '<span style="color:var(--text2);">|</span> ' +
      '<span style="color:var(--green);">比率:'+(ratio?ratio.toFixed(2):'?')+':1</span>' +
      ' <span style="color:var(--text2);">| 完整 '+r.frame_count+' 帧（按需预览）</span>';
    $vp('CompressStats').style.display = 'block';
  } else {
    $vp('CompressStats').innerHTML =
      '<span style="color:var(--text2);">\u25CB 无损: 1:1</span>';
    $vp('CompressStats').style.display = 'block';
  }

  $vp('Result').style.display = 'block';
  $vp('Process').style.display = 'block';
  $vp('Cancel').style.display = 'none';
  $vp('ProgWrap').style.display = 'none';

  $vp('Preview').width = r.width;
  $vp('Preview').height = r.height;

  const pv = $vp('Preview');
  pv.style.cursor = 'pointer';
  pv.title = '点击查看大图（完整帧，按需解码）';

  const first = await ensureVPFrame(0);
  if(first){
    renderRGB565toCanvas(pv, first, r.width, r.height, r.endian === 'little');
  }

  pv.onclick = openVPDetail;

  $vp('Download').disabled = false;
  const canSend = !!(getVPCompressedBytes() && getVPCompressedBytes().length) || !!r.download_id || !!r.compressed_hex || !!r.hex;
  $vp('Send').disabled = !canSend;
  if(!canSend){
    $vp('Send').title = '请先转换';
  } else {
    $vp('Send').title = '';
  }
}

/* ===== VP 详情弹窗 ===== */
let vpDPlaying = false, vpDTimer = null, vpDragging = false, vpRafStart = 0, vpSliderDrag = false, vpFrameDurationMs = 0;
let vpSeekGen = 0;          // 只采纳最新一次 seek 的解码结果
let vpSeekRaf = 0;          // 拖动时 rAF 合并渲染
let vpSliderBound = false;  // 进度条事件只绑定一次

function vpDetailFrameCount(){
  const r = VP.result;
  if(!r) return 0;
  return Math.max(VP.frameIndex.length || 0, r.frame_count || 0, VP.frames.length || 0);
}

function vpClampFrame(idx, nf){
  idx = parseInt(idx, 10);
  if(!isFinite(idx) || isNaN(idx)) idx = 0;
  if(nf <= 0) return 0;
  if(idx < 0) return 0;
  if(idx >= nf) return nf - 1;
  return idx;
}

/** 按帧号刷新预览（带世代号，避免拖动时旧解码覆盖新位置） */
function seekVPFrame(idx, opts){
  opts = opts || {};
  const r = VP.result;
  if(!r) return;
  const nf = vpDetailFrameCount();
  if(nf <= 0) return;
  idx = vpClampFrame(idx, nf);

  const cv = $vp('DCanvas');
  const slider = $vp('DSlider');
  const finfo = $vp('DFrameInfo');
  VP.curFrame = idx;
  if(slider && !opts.skipSlider){
    slider.value = String(idx);
  }
  if(finfo) finfo.textContent = (idx + 1) + ' / ' + nf;

  const gen = ++vpSeekGen;
  ensureVPFrame(idx).then(function(fr){
    if(gen !== vpSeekGen) return;          // 过期 seek
    if(!fr) return;
    if(VP.curFrame !== idx) return;
    if(cv) renderRGB565toCanvas(cv, fr, r.width, r.height, r.endian === 'little');
  });

  // 拖动时预取邻近帧，减轻跟手卡顿
  if(opts.prefetch){
    ensureVPFrame((idx + 1) % nf);
    if(idx > 0) ensureVPFrame(idx - 1);
  }
}

function endVPSliderDrag(){
  if(!vpSliderDrag) return;
  vpSliderDrag = false;
  const slider = $vp('DSlider');
  const nf = vpDetailFrameCount();
  const idx = vpClampFrame(slider ? slider.value : VP.curFrame, nf);
  VP.curFrame = idx;
  // 播放中：把时间轴锚到拖拽落点，避免松开后跳回旧位置
  if(vpDPlaying && vpFrameDurationMs > 0){
    vpRafStart = performance.now() - idx * vpFrameDurationMs;
  }
  seekVPFrame(idx, {prefetch:true});
}

function bindVPSliderOnce(){
  if(vpSliderBound) return;
  vpSliderBound = true;
  const slider = $vp('DSlider');
  if(!slider) return;

  function onSeekStart(e){
    vpSliderDrag = true;
    // 捕获指针，即使拖出滑块也能收到 pointerup
    try{
      if(e && e.pointerId != null && slider.setPointerCapture){
        slider.setPointerCapture(e.pointerId);
      }
    }catch(_e){}
  }
  function onSeekInput(){
    const nf = vpDetailFrameCount();
    const idx = vpClampFrame(slider.value, nf);
    VP.curFrame = idx;
    const finfo = $vp('DFrameInfo');
    if(finfo) finfo.textContent = (idx + 1) + ' / ' + nf;
    // rAF 合并：拖动过程中每帧最多解码/绘制一次，跟手且不堆积
    if(vpSeekRaf) return;
    vpSeekRaf = requestAnimationFrame(function(){
      vpSeekRaf = 0;
      seekVPFrame(slider.value, {skipSlider:true, prefetch:true});
    });
  }
  function onSeekEnd(){
    endVPSliderDrag();
  }

  slider.addEventListener('pointerdown', onSeekStart);
  slider.addEventListener('mousedown', onSeekStart);
  slider.addEventListener('touchstart', onSeekStart, {passive:true});
  slider.addEventListener('input', onSeekInput);
  slider.addEventListener('change', onSeekEnd);
  slider.addEventListener('pointerup', onSeekEnd);
  slider.addEventListener('pointercancel', onSeekEnd);
  slider.addEventListener('mouseup', onSeekEnd);
  slider.addEventListener('touchend', onSeekEnd);
  // 防止在滑块外松开导致 vpSliderDrag 卡死
  window.addEventListener('pointerup', onSeekEnd);
  window.addEventListener('mouseup', onSeekEnd);
  window.addEventListener('touchend', onSeekEnd);
  window.addEventListener('blur', onSeekEnd);
}

async function openVPDetail(){
  VP.curFrame = 0;
  vpSliderDrag = false;
  vpSeekGen++;
  const r = VP.result;
  if(!r) return;
  // 保证索引与至少首帧就绪（完整帧数，按需解码）
  if(getVPCompressedBytes() && !VP.frameIndex.length) buildVPFrameIndex();
  const nf = vpDetailFrameCount();
  if(nf <= 0) return;
  const first = await ensureVPFrame(0);
  if(!first) return;

  const isVid = r.type === 'video' && nf > 1;
  const cv = $vp('DCanvas');
  cv.width = r.width;
  cv.height = r.height;

  renderRGB565toCanvas(cv, first, r.width, r.height, r.endian === 'little');
  cv.style.aspectRatio = r.width+'/'+r.height;

  const ctrls = $vp('DVideoCtrls');
  const slider = $vp('DSlider');
  const finfo = $vp('DFrameInfo');
  const pbtn = $vp('DPlayBtn');

  // show compressed size info in modal
  var compInfo = $vp('DCompInfo');
  if (compInfo) {
    var rawBytes = r.frame_count * r.frame_size;
    var compBytes = (VP.compressedBytes && VP.compressedBytes.length)
      ? VP.compressedBytes.length
      : (r.compressed_bytes || (r.compressed_hex ? r.compressed_hex.length/2 : 0));
    if (compBytes > 0) {
      var ratio = rawBytes > 0 ? (rawBytes / compBytes).toFixed(2) : '?';
      compInfo.innerHTML = '原始:'+rawBytes+'B | 压缩后:'+compBytes+'B | 比率:'+ratio+':1 | 完整'+nf+'帧';
    } else {
      compInfo.innerHTML = '原始:'+rawBytes+'B | 未压缩 (无损)';
    }
    compInfo.style.display = 'block';
  }

  if(isVid){
    ctrls.style.display = 'block';
    slider.max = nf - 1;
    slider.value = 0;
    finfo.textContent = '1 / '+nf;
    bindVPSliderOnce();
    pbtn.disabled = false;
    vpDPlaying = false;
    pbtn.innerHTML = '&#9654; 播放';
    if(vpDTimer){ cancelAnimationFrame(vpDTimer); vpDTimer = null; }
  } else {
    ctrls.style.display = 'none';
  }

  const totalBytes = nf * r.frame_size;
  const totalStr = totalBytes+' B' + (totalBytes >= 1024 ? ' ('+(totalBytes/1024).toFixed(1)+' KB)' : '');
  const endianStr = r.endian ? ' | '+r.endian : '';
  $vp('DTitle').textContent = isVid
    ? '&#128249; '+r.width+'x'+r.height+' | '+nf+' 帧 | '+totalStr + (r.fps?' | '+r.fps.toFixed(1)+' FPS':'')+endianStr
    : '&#128247; '+r.width+'x'+r.height+' | '+totalStr+endianStr;

  if(isVid && r.fps){
    $vp('DFpsInfo').textContent = r.fps.toFixed(1)+' FPS | '+totalStr;
  } else {
    $vp('DFpsInfo').textContent = totalStr;
  }

  $('vpDetailModal').style.display = 'flex';
  initVPDrag();
}

function initVPDrag(){
  const rz = $vp('DResizer');
  if(!rz) return;
  // 每次打开重置默认宽度（约 900 弹窗内 78%），仍可拖拽放大
  const parentW = (rz.parentElement && rz.parentElement.clientWidth) || 860;
  const defW = Math.max(240, Math.min(700, Math.floor(parentW * 0.78)));
  rz.style.width = defW + 'px';
  if(rz._dragInit) return;
  rz._dragInit = true;
  let dragging = false, sx = 0, sw = 0;
  function onStart(e){
    dragging = true;
    vpDragging = true;
    sx = e.clientX;
    sw = rz.getBoundingClientRect().width;
    document.body.style.cursor = 'ew-resize';
    document.body.style.userSelect = 'none';
  }
  function onMove(e){
    if(!dragging) return;
    const dx = e.clientX - sx;
    const maxW = Math.max(defW, (rz.parentElement && rz.parentElement.clientWidth) || 900);
    const nw = Math.max(80, Math.min(maxW, sw + dx));
    rz.style.width = nw + 'px';
  }
  function onEnd(){
    if(!dragging) return;
    dragging = false;
    document.body.style.cursor = '';
    document.body.style.userSelect = '';
    setTimeout(function(){ vpDragging = false; }, 50);
  }
  rz.addEventListener('mousedown', function(e){
    if(e.target === rz || e.target.id === 'vpDGrip') onStart(e);
  });
  document.addEventListener('mousemove', onMove);
  document.addEventListener('mouseup', onEnd);
}

export function closeVPDetail(){
  if(vpDTimer){ cancelAnimationFrame(vpDTimer); vpDTimer = null; }
  if(vpSeekRaf){ cancelAnimationFrame(vpSeekRaf); vpSeekRaf = 0; }
  vpDPlaying = false;
  vpSliderDrag = false;
  vpSeekGen++;
  const el = $('vpDetailModal');
  el.classList.add('closing');
  setTimeout(function(){ el.style.display = 'none'; el.classList.remove('closing'); }, 200);
}

// 点击弹窗遮罩时关闭；拖拽缩放刚结束的鼠标抬起不应误触关闭
export function closeVPDetailOverlay(){
  if(vpDragging) return;
  closeVPDetail();
}

export function toggleVPPlay(){
  const r = VP.result;
  if(!r) return;
  const cv = $vp('DCanvas');
  const slider = $vp('DSlider');
  const btn = $vp('DPlayBtn');
  const finfo = $vp('DFrameInfo');
  const nf = vpDetailFrameCount();
  if(nf <= 0) return;

  // 播放/暂停前先结束可能卡住的拖拽态
  if(vpSliderDrag) endVPSliderDrag();

  if(vpDPlaying){
    if(vpDTimer){ cancelAnimationFrame(vpDTimer); vpDTimer = null; }
    vpDPlaying = false;
    btn.innerHTML = '&#9654; 播放';
    return;
  }

  // 确保帧索引可用（跳转后偶发未建索引）
  if(getVPCompressedBytes() && !VP.frameIndex.length){
    buildVPFrameIndex();
  }

  vpDPlaying = true;
  btn.innerHTML = '&#9646; 暂停';
  // 用总帧数计时，保证完整预览节奏与烧录帧数一致
  vpFrameDurationMs = 0;
  if (r.original_duration && r.original_duration > 0 && nf > 1) {
    vpFrameDurationMs = (r.original_duration * 1000) / nf;
  } else {
    var fpsVal = (r.fps && nf > 1) ? r.fps : 30;
    vpFrameDurationMs = 1000 / fpsVal;
  }
  var frameDuration = vpFrameDurationMs;
  // 从当前位置帧开始播放，不归零
  var startFrame = vpClampFrame(VP.curFrame, nf);
  VP.curFrame = startFrame;
  if(slider) slider.value = String(startFrame);
  if(finfo) finfo.textContent = (startFrame + 1) + ' / ' + nf;
  vpRafStart = performance.now() - startFrame * frameDuration;
  var lastFrame = -1;
  // 立即渲染当前帧（按需解码）
  ensureVPFrame(startFrame).then(function(fr){
    if(fr && vpDPlaying) renderRGB565toCanvas(cv, fr, r.width, r.height, r.endian === 'little');
  });

  function playLoop(now){
    if(!vpDPlaying){ return; }
    // 拖动中：完全交给滑块逻辑，禁止时间轴改写 curFrame
    if(vpSliderDrag){
      vpDTimer = requestAnimationFrame(playLoop);
      return;
    }
    var elapsed = now - vpRafStart;
    if(elapsed < 0) elapsed = 0;
    var frameIndex = Math.floor(elapsed / frameDuration) % nf;
    if(frameIndex < 0) frameIndex = 0;
    if(frameIndex !== lastFrame){
      lastFrame = frameIndex;
      VP.curFrame = frameIndex;
      if(slider) slider.value = String(frameIndex);
      if(finfo) finfo.textContent = (frameIndex+1)+' / '+nf;
      ensureVPFrame(frameIndex).then(function(fr){
        if(fr && vpDPlaying && VP.curFrame === frameIndex && !vpSliderDrag){
          renderRGB565toCanvas(cv, fr, r.width, r.height, r.endian === 'little');
        }
      });
      // 轻量预取后续几帧，减少卡顿；不预取全部，避免 OOM
      for(var p=1;p<=3;p++){
        ensureVPFrame((frameIndex+p)%nf);
      }
    }
    vpDTimer = requestAnimationFrame(playLoop);
  }

  vpDTimer = requestAnimationFrame(playLoop);
}

function renderRGB565toCanvas(canvas, data, w, h, littleEndian){
  const ctx = canvas.getContext('2d');
  if(!ctx) return;
  const img = ctx.createImageData(w, h);
  const d = img.data;
  for(let i=0; i<w*h; i++){
    const b0 = data[i*2], b1 = data[i*2+1];
    // littleEndian=true: b1=high, b0=low; false: b0=high, b1=low
    const c = littleEndian ? (b1<<8) | b0 : (b0<<8) | b1;
    d[i*4]   = ((c>>11)&0x1F)*255/31;
    d[i*4+1] = ((c>>5)&0x3F)*255/63;
    d[i*4+2] = (c&0x1F)*255/31;
    d[i*4+3] = 255;
  }
  ctx.putImageData(img, 0, 0);
}

function getVPCompressedBytes(){
  if(VP.compressedBytes && VP.compressedBytes.length) return VP.compressedBytes;
  var r = VP.result;
  if(r && r.compressed_hex){
    VP.compressedBytes = hexToBytes(r.compressed_hex);
    return VP.compressedBytes;
  }
  return null;
}

/** 扫描完整压缩流，建立每帧 [start,end) 索引；不解码像素（O(frames) 轻量） */
function buildVPFrameIndex(){
  var r = VP.result;
  var raw = getVPCompressedBytes();
  VP.frameIndex = [];
  VP.frames = [];
  if(!r || !raw || !raw.length) return 0;
  var w = r.width, h = r.height;
  var pos = 0;
  if(r.codec === 'mjpeg'){
    if(raw.length < 14) return 0;
    // 优先使用文件头 frame_count；与 r.frame_count 取一致
    var hdrCount = raw[4] | (raw[5]<<8);
    var expect = r.frame_count || hdrCount || 0;
    pos = 14;
    for(var i=0; i<expect && pos+4 <= raw.length; i++){
      var frameSize = (raw[pos] | (raw[pos+1]<<8) | (raw[pos+2]<<16) | (raw[pos+3]<<24)) >>> 0;
      pos += 4;
      if(frameSize === 0 || pos + frameSize > raw.length) break;
      VP.frameIndex.push([pos, pos + frameSize]);
      VP.frames.push(null);
      pos += frameSize;
    }
  } else {
    // RAW5 容器：14B 头 + 固定 width*height*2 的 BE RGB565 帧
    // 兼容旧无头 raw：无 magic 时从 offset 0 起按固定帧大小切
    var hasRawHdr = (raw.length >= 14 &&
      raw[0] === 0x52 && raw[1] === 0x41 && raw[2] === 0x57 && raw[3] === 0x35);
    if(hasRawHdr){
      var hdrCountR = raw[4] | (raw[5]<<8);
      var hdrW = raw[6] | (raw[7]<<8);
      var hdrH = raw[8] | (raw[9]<<8);
      if(hdrW > 0) w = hdrW;
      if(hdrH > 0) h = hdrH;
      r.width = w;
      r.height = h;
      pos = 14;
      var expectR = r.frame_count || hdrCountR || 0;
      var frameBR = w * h * 2;
      if(frameBR <= 0) return 0;
      for(var j=0; j<expectR && pos + frameBR <= raw.length; j++){
        VP.frameIndex.push([pos, pos + frameBR]);
        VP.frames.push(null);
        pos += frameBR;
      }
    } else {
      var frameB = w * h * 2;
      if(frameB <= 0) return 0;
      var expectB = r.frame_count || Math.floor(raw.length / frameB);
      for(var k=0; k<expectB && pos + frameB <= raw.length; k++){
        VP.frameIndex.push([pos, pos + frameB]);
        VP.frames.push(null);
        pos += frameB;
      }
    }
  }
  // 同步真实帧数（完整，不截断）
  if(VP.frameIndex.length > 0){
    r.frame_count = VP.frameIndex.length;
  }
  if (!r.original_duration || r.original_duration <= 0) {
    var fpsEst = (r.fps && r.fps > 0) ? r.fps : 30;
    r.original_duration = (r.frame_count || 0) / fpsEst;
  }
  return VP.frameIndex.length;
}

/** 按需解码第 i 帧；完整帧可访问，但内存只缓存已访问帧 */
async function ensureVPFrame(i){
  var r = VP.result;
  if(!r) return null;
  if(!VP.frameIndex.length){
    if(getVPCompressedBytes()) buildVPFrameIndex();
  }
  if(i < 0 || i >= VP.frameIndex.length) return null;
  if(VP.frames[i]) return VP.frames[i];

  var raw = getVPCompressedBytes();
  if(!raw) return null;
  var range = VP.frameIndex[i];
  var w = r.width, h = r.height;
  var gen = VP.decodeGen;

  try{
    if(r.codec === 'mjpeg'){
      var jpegBytes = raw.subarray(range[0], range[1]);
      var blob = new Blob([jpegBytes], {type:'image/jpeg'});
      var bitmap = await createImageBitmap(blob);
      if(gen !== VP.decodeGen){ try{bitmap.close();}catch(e){} return null; }
      var c = document.createElement('canvas');
      c.width = w; c.height = h;
      var ctx = c.getContext('2d', {willReadFrequently:true});
      ctx.drawImage(bitmap, 0, 0, w, h);
      var imgData = ctx.getImageData(0, 0, w, h).data;
      var rgba = new Uint8Array(w * h * 2);
      for (var px = 0; px < w * h; px++) {
        var r2 = imgData[px*4], g = imgData[px*4+1], b = imgData[px*4+2];
        var val = ((r2>>3)<<11) | ((g>>2)<<5) | (b>>3);
        rgba[px*2] = (val>>8)&0xFF;
        rgba[px*2+1] = val&0xFF;
      }
      bitmap.close();
      VP.frames[i] = rgba;
    } else {
      // raw/RAW5 帧：切片后已是 big-endian RGB565（与 MCU 烧录一致）
      var frame = raw.subarray(range[0], range[1]).slice();
      VP.frames[i] = frame;
    }
  }catch(e){
    console.warn('解码帧失败 ['+i+']:', e && e.message ? e.message : e);
    return null;
  }
  return VP.frames[i] || null;
}

// 兼容旧调用名：不再全量解码
async function rebuildMJPEGFrames(){
  buildVPFrameIndex();
  await ensureVPFrame(0);
}
function rebuildVPFramesFromCompressed(){
  buildVPFrameIndex();
}

export async function downloadVPResult(){
  const r = VP.result;
  if(!r || (!r.hex && !r.download_id)){ toast('没有数据','error'); return; }

  try{
    // use download_id when available (full video file on server)
    if(r.download_id){
      const url = VP_SERVER + '/download/' + r.download_id;
      const a = document.createElement('a');
      a.href = url;
      a.download = '';
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      toast('正在下载完整文件...','success');
      return;
    }

    // legacy POST download for single images
    const resp = await fetch(VP_SERVER+'/download', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({
        hex: r.hex,
        width: r.width,
        height: r.height,
        frame_count: r.frame_count,
        type: r.type,
        name: (VP.file ? VP.file.name.replace(/\.[^.]+$/,'') : 'output') + '_st7735',
      }),
    });
    if(!resp.ok) throw new Error('Download failed');
    const blob = await resp.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    const ext = r.frame_count > 1 ? '.zip' : '.bin';
    const fname = (VP.file ? VP.file.name.replace(/\.[^.]+$/,'') : 'output') + '_' + r.width + 'x' + r.height + ext;
    a.download = fname;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    toast('下载完成: '+fname,'success');
  } catch(e){
    toast('下载失败: '+e.message,'error');
  }
}

export async function sendVPToDevice(){
  if(!state.port){ toast('请先连接串口','error'); return; }
  const r = VP.result;
  if(!r){ toast('没有数据','error'); return; }

  let raw = getVPCompressedBytes();
  // 若内存中尚无完整 payload，再按 download_id 拉一次（完整，不截断）
  if((!raw || !raw.length) && r.download_id){
    try{
      toast('正在拉取完整烧录数据...','info');
      const resp = await fetch(VP_SERVER+'/download/'+r.download_id);
      if(!resp.ok) throw new Error('HTTP '+resp.status);
      raw = new Uint8Array(await resp.arrayBuffer());
      VP.compressedBytes = raw;
    }catch(e){
      toast('拉取数据失败: '+e.message,'error');
      return;
    }
  }
  if(!raw || !raw.length){
    if(r.hex) raw = hexToBytes(r.hex);
  }
  if(!raw || !raw.length){ toast('没有可发送的数据','error'); return; }

  const blob = new Blob([raw]);
  const base = VP.file ? VP.file.name.replace(/\.[^.]+$/,'') : 'st7735';
  const fakeFile = new File([blob], base + '.bin', {type:'application/octet-stream'});

  state.selFile = fakeFile;
  const sz = fakeFile.size < 1024 ? fakeFile.size+'B' : (fakeFile.size/1024).toFixed(1)+'KB';
  elFileInfo.textContent = '\u2714 [VP] '+fakeFile.name+' ('+sz+')';
  try{
    // 视频/大文件默认大文件区
    if(r.type === 'video' || fakeFile.size > 6*1024){
      $('fileType').value = '0x11';
      if(typeof updateSmallFileTypeHint === 'function') updateSmallFileTypeHint({toast:false});
    }
  }catch(e){}
  elName.value = (r.type === 'video' ? 'vp_vid' : 'vp_img'); updateNBC();
  elBtnSend.disabled = false;

  toast('已填入完整数据（'+sz+'，'+ (r.frame_count||'?') +'帧），请确认类型后发送','success');

  // scroll to send settings
  const sendCard = $('fileType').closest('.card');
  if(sendCard){
    sendCard.scrollIntoView({behavior:'smooth',block:'center'});
    setTimeout(function(){
      $('fileType').focus();
    }, 500);
  }
}
