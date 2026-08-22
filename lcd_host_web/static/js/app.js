// app.js — 入口：编排串口连接/断开、响应分发、事件委托、初始化
import {
  $, state,
  R_CONT, R_ERR, C_QRY, C_LCD, C_BMP, R_LCD,
  ERRMSG, LCD_FRAME_SZ, ST_SENDING, ST_WAIT_ACK, ST_DONE,
  clearLcdCanvas, clearLog, log, toast,
} from './core.js';
import { setRespHandler, signalCont, resetCont } from './protocol.js';
import { startRead, deviceSession } from './serial.js';
import {
  initLcdContext, updateLcdUI, renderLcdFrame,
  lcdStreamEnable, lcdStreamDisable, lcdWatchCheck, stopLcdWatch,
} from './lcd.js';
import { queryBitmap, handleBitmapResp, closeBitmapModal } from './bitmap.js';
import {
  parseList, closeModal, openFileDetailByIndex, openDeleteModal,
  delFIClick, delFromModal, onFileSelected, userStartSend, cancelSend,
  retrySend, doAbort, queryFileList, setUI,
} from './transfer.js';
import {
  onVPFileSelected, startVPProcess, cancelVPProcess, downloadVPResult,
  sendVPToDevice, closeVPDetail, closeVPDetailOverlay, toggleVPPlay,
} from './videoproc.js';
import {
  initTheme, toggleTheme, initSplitters, initSettingsListeners,
  loadAllSettings, onVpCodecChange, saveAllSettings,
} from './ui.js';

/* ===== Serial (编排) ===== */
async function connectSerial(){
  try{
    const br=parseInt($('baudRate').value)||921600;
    state.port=await deviceSession.connect({baudRate:br});
    $('statusDot').className='status-dot on';$('statusText').textContent='在线';$('portInfo').textContent='@ '+br;
    $('statusBadge').classList.add('connected');
    $('btnConnect').style.display='none';$('btnDisconnect').style.display='block';
    log('串口连接 @ '+br+' bps','info');startRead();
    updateLcdUI(false);
    state.lcdLastFrameTime=0;state.lcdFrameCount=0;clearLcdCanvas();
    if(state.lcdWatchTimer){clearInterval(state.lcdWatchTimer);}
    state.lcdWatchTimer=setInterval(lcdWatchCheck,300);
    setTimeout(async function(){ await lcdStreamEnable(); },300);
    setTimeout(function(){queryFileList();},600);
    saveAllSettings();
  }catch(e){log('连接失败: '+e.message,'error');toast('连接失败','error');}
}

async function disconnectSerial(){
  state.sendState=ST_DONE;resetCont();
  if(state.delTimer){clearTimeout(state.delTimer);state.delTimer=null;}
  if(state.delPollTimer){clearInterval(state.delPollTimer);state.delPollTimer=null;}
  state.delPending=false;state.delTarget=null; /* 删除中断开串口：释放删除锁，避免重连后锁死 */
  state.readLoop=false;
  await deviceSession.disconnect();
  state.port=null;
  $('statusDot').className='status-dot off';$('statusText').textContent='脱机';$('portInfo').textContent='';
  $('statusBadge').classList.remove('connected');
  $('btnConnect').style.display='block';$('btnDisconnect').style.display='none';
  setUI('idle');log('串口断开','info');
  updateLcdUI(false);
  clearLcdCanvas();
  stopLcdWatch();
}

/* ===== RX 分发 ===== */
function handleResp(cmd,payload){
  if(cmd===R_CONT){
    if(state.delPending){ /* 删除完成确认：MCU 擦除完毕后回 0xA1 */
      log('MCU \u2192 删除完成 (0xA1)','recv');
      if(state.delTimer){clearTimeout(state.delTimer);state.delTimer=null;}
      if(state.delPollTimer){clearInterval(state.delPollTimer);state.delPollTimer=null;}
      state.delPending=false;state.delTarget=null;
      deviceSession.transition('Idle');
      toast('删除完成','success');
      queryFileList();
    }else if(!state.isTransferring){log('MCU \u2192 就绪 (0xA1)','recv');}
    else{signalCont();}
  }
  else if(cmd===R_ERR){
    const code=payload.length>0?payload[0]:0;
    log('MCU \u2192 错误 0x'+code.toString(16).toUpperCase().padStart(2,'0')+' '+ (ERRMSG[code]||'未知'),'error');
    if(state.sendState===ST_SENDING||state.sendState===ST_WAIT_ACK) doAbort(ERRMSG[code]||'0x'+code.toString(16));
    if(state.delPending){ /* 删除失败：释放删除锁，避免“删除操作进行中”永久卡死 */
      if(state.delTimer){clearTimeout(state.delTimer);state.delTimer=null;}
      if(state.delPollTimer){clearInterval(state.delPollTimer);state.delPollTimer=null;}
      state.delPending=false;state.delTarget=null;
      deviceSession.transition('Idle');
      toast('删除失败: '+(ERRMSG[code]||'0x'+code.toString(16)),'error');
    }
  }
  else if(cmd===C_QRY){log('MCU \u2192 文件列表 ('+payload[0]+' 个文件)','recv');parseList(payload);}
  else if(cmd===C_LCD){
    if(payload.length>=1){
      const en=payload[0];
      updateLcdUI(en);
      if(!en&&state.lcdAckResolve){state.lcdAckResolve();state.lcdAckResolve=null;}
      // 优先用主机意图确定日志文本：主机明确操作时按意图显示，
      // 避免 MCU 响应滞后/滞留帧导致的错误日志
      var logState = en ? '已开启' : '已关闭';
      if (state.lcdHostIntent === 'enable') logState = '已开启';
      else if (state.lcdHostIntent === 'disable') logState = '已关闭';
      log('MCU \u2192 LCD流 ' + logState, 'recv');
      state.lcdHostIntent = null; // 消费后清除
    }
  }
  else if(cmd===C_BMP){handleBitmapResp(payload);}
  else if(cmd===R_LCD){
    if(payload.length>=LCD_FRAME_SZ){renderLcdFrame(payload);state.lcdLastFrameTime=Date.now();}
    state.lcdFrameCount++;
    const stEl=$('lcdStreamStatus'),cv=$('lcdCanvas'),lc=$('lcdCard');
    if(stEl&&!stEl.textContent.includes('已开启')){
      stEl.textContent='\u25C9 已开启';stEl.style.color='var(--green)';
    }
    if(cv){
      if(!cv.classList.contains('streaming'))cv.classList.add('streaming');
    }
    if(lc&&!lc.classList.contains('streaming'))lc.classList.add('streaming');
  }
  else{let h='';for(let i=0;i<Math.min(payload.length,12);i++)h+=('0'+payload[i].toString(16).toUpperCase()).slice(-2)+' ';log('MCU \u2192 0x'+cmd.toString(16).toUpperCase().padStart(2,'0')+' '+h,'recv');}
}

/* ===== 事件委托 ===== */
const actions = {
  toggleTheme,
  connectSerial,
  disconnectSerial,
  onFileSelected: (el) => onFileSelected(el),
  userStartSend,
  cancelSend,
  retrySend,
  queryFileList,
  queryBitmap,
  openDeleteModal,
  onVpCodecChange,
  onVPFileSelected: (el) => onVPFileSelected(el),
  startVPProcess,
  cancelVPProcess,
  downloadVPResult,
  sendVPToDevice,
  clearLog,
  lcdStreamEnable,
  lcdStreamDisable,
  closeModal,
  closeBitmapModal,
  closeVPDetail,
  closeVPDetailOverlay,
  toggleVPPlay,
  openFileDetail: (el) => openFileDetailByIndex(parseInt(el.dataset.index, 10)),
  delFile: (el) => delFIClick(parseInt(el.dataset.ft, 10), parseInt(el.dataset.fi, 10), el.dataset.name || ''),
  delFromModal: (el) => delFromModal(parseInt(el.dataset.ft, 10), parseInt(el.dataset.fi, 10), el.dataset.name || ''),
};

function handleAction(e){
  const el = e.target.closest('[data-action]');
  if(!el) return;
  const fn = actions[el.dataset.action];
  if(!fn) return;
  // 弹窗遮罩：仅点击遮罩自身（而非内容区）才触发关闭
  if(el.classList.contains('modal-overlay') && e.target !== el) return;
  fn(el, e);
}

function initEvents(){
  document.addEventListener('click', handleAction);
  document.addEventListener('change', handleAction);

  $('fileDetailModal').addEventListener('keydown',function(e){if(e.key==='Escape')closeModal();});
  $('vpDetailModal').addEventListener('keydown',function(e){if(e.key==='Escape')closeVPDetail();});

  window.addEventListener('beforeunload',function(){
    state.sendState=ST_DONE;resetCont();state.readLoop=false;
    if(deviceSession.port){try{deviceSession.disconnect()}catch(e){}}
  });

  if(!('serial' in navigator)){
    $('btnConnect').disabled=true;
    $('btnConnect').textContent='需Chrome/Edge';
    toast('请用Chrome/Edge浏览器','error');
  }
}

/* ===== Init ===== */
function init(){
  initLcdContext();
  initTheme();
  initSplitters();
  initSettingsListeners();
  loadAllSettings();
  initEvents();
  setRespHandler(handleResp);
}

init();
