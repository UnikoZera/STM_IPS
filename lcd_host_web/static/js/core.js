// core.js — 基础工具、协议常量、DOM 引用、跨模块共享状态、日志/提示
// 注意：本模块不依赖任何业务模块，仅提供地基能力。
// 共享可变状态统一放在 `state` 对象内（ES module 的 import 绑定只读，需通过对象属性跨模块读写）。

// ============= CRC16 USB (MODBUS) =============
const CRC16_TABLE = new Uint16Array(256);
for (let i=0;i<256;i++) { let c=i; for (let j=0;j<8;j++) c=(c&1)?((c>>>1)^0xA001):(c>>>1); CRC16_TABLE[i]=c; }
export function crc16(data,off,len) { let c=0xFFFF; for (let i=0;i<len;i++) c=(c>>>8)^CRC16_TABLE[(c&0xFF)^data[off+i]]; return c^0xFFFF; }

// ============= Constants =============
export const H0=0xBB,H1=0x44,HS=9;
export const C_L=0x11,C_S=0x45,C_END=0x14,C_DEL=0x19,C_QRY=0x20,C_LCD=0x10,C_BMP=0x21,C_ABORT=0x15;
export const LCD_W=160,LCD_H=80,LCD_TAIL=4,LCD_FRAME_SZ=LCD_W*LCD_H*2;
export const R_LCD=0xA0;
export const UH0=0xAA,UH1=0x55,UHS=5;
export const R_CONT=0xA1,R_ERR=0xE0;
export const ERRMSG={1:'CRC错误',2:'未知类型',3:'大文件区满',4:'小文件区满',5:'类型不匹配',6:'大文件槽满',7:'小文件槽满',8:'索引无效',9:'未知命令',10:'DMA失败',11:'Flash写入失败',12:'写入验证失败'};
export const WRITE_TIMEOUT=8000, SEND_TIMEOUT=15000;
export const ST_INIT=0, ST_SENDING=1, ST_WAIT_ACK=2, ST_DONE=3, ST_ERR=4;
export const SET_KEY='stm_ips_host_cfg';

// ============= DOM refs =============
export const $=id=>document.getElementById(id);
export const elLog=$('log'), elProg=$('progressBar'), elPct=$('progressPct'), elBytes=$('progressBytes');
export const elStatus=$('sendStatus'), elBtnSend=$('btnStartSend'), elBtnCancel=$('btnCancelSend'), elBtnRetry=$('btnRetrySend');
export const elFileInfo=$('fileInfo'), elFileList=$('fileList'), elToast=$('toastContainer');
export const elName=$('fileNameInput'), elNameCnt=$('nameByteCount');
export const elLogBody=$('logPanelBody'), elFLBody=$('fileListPanelBody');
export const elFileCnt=$('fileCountBadge');

// ============= 共享可变状态 =============
export const state = {
  port:null, reader:null, readLoop:false,
  fileData:null, fileOff:0, fileCmd:0, fileName:'',
  selFile:null, sendState:ST_INIT,
  contResolve:null, contPending:0,
  writeTimer:null, sendTimer:null,
  lastFileListData:null,
  lcdFrameCount:0, lcdCanvas:null, lcdCtx:null,
  lcdStreamWasOn:false,
  lcdHostIntent:null, // 'enable' | 'disable' | null — 最后一次主机的操作意图
  isTransferring:false,
  lcdLastFrameTime:0, lcdWatchTimer:null,
  lcdQueryTid:null,
  lcdAckResolve:null,
  rxBuf:new Uint8Array(0),
  delPending:false,
  delTimer:null,       // 删除确认超时兜底
  delPollTimer:null,   // 删除确认轮询：0xA1 丢失时靠文件从列表消失兜底
  delTarget:null,      // 当前删除目标 {ft,fi}
};

// ============= 工具 =============
export function escHtml(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML;}
export function escAttr(s){return s.replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/'/g,'&#39;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
export function hexToBytes(hex){
  const b = new Uint8Array(hex.length/2);
  for(let i=0; i<hex.length; i+=2) b[i/2] = parseInt(hex.substr(i,2), 16);
  return b;
}

export function clearLcdCanvas(){
  if(!state.lcdCtx)return;
  state.lcdCtx.fillStyle='#0a0f18';
  state.lcdCtx.fillRect(0,0,160,80);
}

// ============= Log / Toast =============
export function log(msg,cls){
  const t=new Date().toLocaleTimeString();
  const cm={send:'log-send',recv:'log-recv',error:'log-error',info:'log-info',warn:'log-warn'};
  const div=document.createElement('div');div.className='log-line';
  div.innerHTML='<span class="log-time">['+t+']</span> <span class="'+(cm[cls]||'')+'">'+escHtml(msg)+'</span>';
  elLog.appendChild(div);
  requestAnimationFrame(function(){elLogBody.scrollTo({top:elLogBody.scrollHeight,behavior:'smooth'});});
}

export function clearLog(){elLog.innerHTML='';log('日志已清除','info');}

export function toast(msg,type){
  const d=document.createElement('div');d.className='toast '+(type||'');
  const m=document.createElement('span');m.className='toast-msg';m.textContent=msg;
  const x=document.createElement('span');x.className='toast-close';x.textContent='\u2715';
  x.title='右键点击或点击此处关闭';
  d.appendChild(m);d.appendChild(x);
  let dismissTimer=setTimeout(function(){removeToast(d,dismissTimer);},4000);
  function dismiss(){removeToast(d,dismissTimer);}
  x.addEventListener('click',dismiss);
  d.addEventListener('contextmenu',function(e){e.preventDefault();dismiss();});
  elToast.appendChild(d);
}
export function removeToast(d,timer){
  if(timer)clearTimeout(timer);
  d.classList.add('hiding');
  setTimeout(function(){if(d.parentNode)d.parentNode.removeChild(d);},350);
}
