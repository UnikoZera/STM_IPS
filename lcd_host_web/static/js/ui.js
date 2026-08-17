// ui.js — 设置持久化、主题切换、面板分割条拖拽
import { $, SET_KEY } from './core.js';
import { updateNBC } from './transfer.js';

export function saveAllSettings(){
  const s={
    baudRate:$('baudRate').value,
    fileType:$('fileType').value,
    fileName:$('fileNameInput').value,
    leftW:$('leftPanel').style.width||'',
    rightW:$('rightPanel').style.width||'',
    vpWidth:$('vpWidth').value,
    vpHt:$('vpHt').value,
    vpFps:$('vpFps').value,
    vpCodec:$('vpCodec').value,
    vpMjpegQuality:$('vpMjpegQuality').value,
    vpBrightness:$('vpBrightness').value
  };
  try{localStorage.setItem(SET_KEY,JSON.stringify(s));}catch(e){}
}

export function loadAllSettings(){
  try{
    const raw=localStorage.getItem(SET_KEY);
    if(!raw)return;
    const s=JSON.parse(raw);
    if(s.baudRate)$('baudRate').value=s.baudRate;
    if(s.fileType)$('fileType').value=s.fileType;
    if(s.fileName)$('fileNameInput').value=s.fileName;
    if(s.leftW){const w=Math.max(260,Math.min(520,parseInt(s.leftW)||340));$('leftPanel').style.width=w+'px';}
    if(s.rightW){const w=Math.max(260,Math.min(520,parseInt(s.rightW)||340));$('rightPanel').style.width=w+'px';}
    if(s.vpWidth)$('vpWidth').value=s.vpWidth;
    if(s.vpHt)$('vpHt').value=s.vpHt;
    if(s.vpFps)$('vpFps').value=s.vpFps;
    // 恢复上次选择的编码器（旧版可能存了已废弃的编码，统一回退到 mjpeg）
    if(s.vpCodec){
      const codec = (s.vpCodec === 'raw') ? 'raw' : 'mjpeg';
      $('vpCodec').value=codec;
      onVpCodecChange();
    }
    if(s.vpMjpegQuality){
      $('vpMjpegQuality').value=s.vpMjpegQuality;
      $('vpMjpegQualityLabel').textContent=s.vpMjpegQuality;
    }
    if(s.vpBrightness){
      $('vpBrightness').value=s.vpBrightness;
      const lbl=$('vpBrightnessLabel');
      if(lbl) lbl.textContent=s.vpBrightness+'%';
    }
    updateNBC();
  }catch(e){}
}

export function initSettingsListeners(){
  ['baudRate','fileType','vpWidth','vpHt','vpFps','vpCodec','vpMjpegQuality','vpBrightness'].forEach(function(id){
    const el=document.getElementById(id);
    if(el) el.addEventListener('change',saveAllSettings);
  });
  $('vpBrightness').addEventListener('input',function(){
    const lbl=$('vpBrightnessLabel');
    if(lbl) lbl.textContent=this.value+'%';
    saveAllSettings();
  });
  $('vpMjpegQuality').addEventListener('input',function(){
    const lbl=$('vpMjpegQualityLabel');
    if(lbl) lbl.textContent=this.value;
    saveAllSettings();
  });
  $('fileNameInput').addEventListener('input',function(){
    updateNBC();saveAllSettings();
  });
}

export function onVpCodecChange(){
  const v=$('vpCodec').value;
  $('vpMjpegQualityGroup').style.display=(v==='mjpeg')?'flex':'none';
  saveAllSettings();
}

/* ===== Panel Splitter Drag ===== */
export function initSplitters(){
  function makeSplitter(splitterId, panelId, side){
    const sp=document.getElementById(splitterId);
    const pn=document.getElementById(panelId);
    if(!sp||!pn)return;
    let sx=0, sw=0, dragging=false;
    function onDown(e){
      if(e.button!=null && e.button!==0)return;
      e.preventDefault();
      e.stopPropagation();
      const pt=e.touches?e.touches[0]:e;
      sx=pt.clientX;
      sw=pn.getBoundingClientRect().width;
      dragging=true;
      sp.classList.add('active');
      document.body.classList.add('is-resizing');
      document.addEventListener('mousemove',onMove);
      document.addEventListener('mouseup',onUp);
      document.addEventListener('touchmove',onMove,{passive:false});
      document.addEventListener('touchend',onUp);
    }
    function onMove(e){
      if(!dragging)return;
      if(e.cancelable)e.preventDefault();
      const pt=e.touches?e.touches[0]:e;
      let dx=pt.clientX-sx;
      let nw=side==='left'?sw+dx:sw-dx;
      nw=Math.max(260,Math.min(560,nw));
      pn.style.width=nw+'px';
      pn.style.flex='0 0 auto';
    }
    function onUp(){
      if(!dragging)return;
      dragging=false;
      sp.classList.remove('active');
      document.body.classList.remove('is-resizing');
      document.removeEventListener('mousemove',onMove);
      document.removeEventListener('mouseup',onUp);
      document.removeEventListener('touchmove',onMove);
      document.removeEventListener('touchend',onUp);
      saveAllSettings();
    }
    sp.addEventListener('mousedown',onDown);
    sp.addEventListener('touchstart',onDown,{passive:false});
  }
  makeSplitter('splitterL','leftPanel','left');
  makeSplitter('splitterR','rightPanel','right');
}

/* ===== Theme toggle ===== */
export function toggleTheme(){
  const html = document.documentElement;
  html.classList.add('theme-animating');
  html.classList.toggle('light');
  const isLight = html.classList.contains('light');
  $('themeBtn').innerHTML = isLight ? '&#9788;' : '&#9790;';
  $('themeBtn').title = isLight ? '切换深色模式' : '切换浅色模式';
  try{ localStorage.setItem('stm_ips_theme', isLight ? 'light' : 'dark'); }catch(e){}
  window.clearTimeout(toggleTheme._tid);
  toggleTheme._tid = window.setTimeout(function(){
    html.classList.remove('theme-animating');
  }, 480);
}

export function initTheme(){
  const saved = (typeof localStorage!=='undefined') ? localStorage.getItem('stm_ips_theme') : null;
  if(saved === 'light'){
    document.documentElement.classList.add('light');
    $('themeBtn').innerHTML = '&#9788;';
    $('themeBtn').title = '切换深色模式';
  }
}
