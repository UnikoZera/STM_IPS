// lcd.js — LCD 帧渲染（160×80 RGB565）与 LCD 流控制
import {
  $, state, LCD_W, LCD_H, LCD_FRAME_SZ, C_LCD,
  clearLcdCanvas, log, toast,
} from './core.js';
import { frame } from './protocol.js';
import { writeSer, setDeviceActivity } from './serial.js';

/* pre-init canvas context early */
export function initLcdContext(){
  state.lcdCanvas=$('lcdCanvas');
  if(state.lcdCanvas){
    state.lcdCtx=state.lcdCanvas.getContext('2d',{willReadFrequently:false});
    state.lcdCtx.imageSmoothingEnabled=false;
    clearLcdCanvas();
  }
}

/* ===== LCD Frame Render ===== */
let lcdBackCanvas=null,lcdBackCtx=null,lcdPendingFrame=null,lcdFrameScheduled=false;
export function renderLcdFrame(payload){
  lcdPendingFrame=payload;
  if(!lcdFrameScheduled){
    lcdFrameScheduled=true;
    requestAnimationFrame(commitLcdFrame);
  }
}
function commitLcdFrame(){
  lcdFrameScheduled=false;
  if(!state.lcdCtx)return;
  if(!lcdBackCanvas){lcdBackCanvas=document.createElement('canvas');lcdBackCanvas.width=LCD_W;lcdBackCanvas.height=LCD_H;lcdBackCtx=lcdBackCanvas.getContext('2d');}
  const p=lcdPendingFrame;if(!p)return;lcdPendingFrame=null;
  const img=lcdBackCtx.createImageData(LCD_W,LCD_H);
  const d=img.data;
  for(let i=0;i<LCD_W*LCD_H;i++){
    const hi=i*2,lo=hi+1;
    const c=(p[hi]<<8)|p[lo];
    d[i*4]=((c>>11)&0x1F)*255/31;
    d[i*4+1]=((c>>5)&0x3F)*255/63;
    d[i*4+2]=(c&0x1F)*255/31;
    d[i*4+3]=255;
  }
  lcdBackCtx.putImageData(img,0,0);
  state.lcdCtx.drawImage(lcdBackCanvas,0,0);
}

/* ===== LCD Stream Control ===== */
function hideSendWarn(){
  const el=$('lcdSendWarn');
  if(!el)return;
  clearTimeout(el._timer);
  el.classList.add('hide-warn');
  setTimeout(function(){el.style.display='none';},350);
}

export function updateLcdUI(enabled){
  const stEl=$('lcdStreamStatus');
  const cv=$('lcdCanvas'),lc=$('lcdCard');
  if(enabled){
    stEl.textContent='\u25C9 已开启';stEl.style.color='var(--green)';
    if(cv)cv.classList.add('streaming');
    if(lc)lc.classList.add('streaming');
  }else{
    stEl.textContent='\u25CB 已关闭';stEl.style.color='var(--red)';
    if(cv){cv.classList.remove('streaming');clearLcdCanvas();}
    if(lc)lc.classList.remove('streaming');
    hideSendWarn();
  }
}

export async function lcdStreamEnable(){
  if(!state.port){toast('未连接','error');return;}
  state.lcdHostIntent = 'enable';
  const ok=await writeSer(frame(C_LCD,new Uint8Array([0x01])));
  if(ok) setDeviceActivity('Streaming');
  log('HOST \u2192 开启LCD流','send');
}

export async function lcdStreamDisable(){
  if(!state.port){toast('未连接','error');return;}
  state.lcdHostIntent = 'disable';
  const ok=await writeSer(frame(C_LCD,new Uint8Array([0x00])));
  if(ok) setDeviceActivity('Idle');
  log('HOST \u2192 关闭LCD流','send');
}

export async function lcdStreamQuery(){
  if(!state.port){toast('未连接','error');return;}
  if(state.lcdQueryTid){clearTimeout(state.lcdQueryTid);state.lcdQueryTid=null;}
  state.lcdHostIntent = null; // 查询不改变意图
  await writeSer(frame(C_LCD,new Uint8Array(0)));
  log('HOST \u2192 查询LCD流状态','send');
}

/* ===== LCD Stream Watch ===== */
export function lcdWatchCheck(){
  const stEl=$('lcdStreamStatus');
  if(!stEl)return;
  if(state.lcdLastFrameTime&&Date.now()-state.lcdLastFrameTime>300){
    if(stEl.textContent.includes('已开启')){
      stEl.textContent='\u25CB 已关闭';stEl.style.color='var(--red)';
      const cv=$('lcdCanvas'),lc=$('lcdCard');
      if(cv){cv.classList.remove('streaming');clearLcdCanvas();}
      if(lc)lc.classList.remove('streaming');
    }
  }
}
export function stopLcdWatch(){
  if(state.lcdWatchTimer){clearInterval(state.lcdWatchTimer);state.lcdWatchTimer=null;}
  state.lcdLastFrameTime=0;
  const cv=$('lcdCanvas'),lc=$('lcdCard');
  if(cv){cv.classList.remove('streaming');clearLcdCanvas();}
  if(lc)lc.classList.remove('streaming');
}
