// bitmap.js — Flash 扇区位图（0x21）
import { $, state, C_BMP, log, toast } from './core.js';
import { frame } from './protocol.js';
import { writeSer } from './serial.js';

let bitmapRawData=null;

export async function queryBitmap(){
  if(!state.port){toast('请先连接串口','error');return;}
  const frm=frame(C_BMP,null,0);
  const ok=await writeSer(frm);
  if(ok){log('HOST -> 查询Flash位图','send');toast('正在获取位图...','info');}
}

export function handleBitmapResp(payload){
  if(!payload||payload.length<496){toast('位图数据无效','error');return;}
  bitmapRawData=new Uint8Array(payload);
  const total=3968;
  let used=0;
  for(let i=0;i<total;i++){
    const byteIdx=Math.floor(i/8),bitIdx=i%8;
    if(bitmapRawData[byteIdx]&(1<<bitIdx))used++;
  }
  const free=total-used;
  const pct=(used/total*100).toFixed(1);
  log('MCU -> Flash位图: '+used+'/'+total+' 扇区已用 ('+pct+'%)','recv');
  showBitmapModal(bitmapRawData,used,free,total);
}

function showBitmapModal(data,used,free,total){
  const canvas=$('bitmapCanvas');
  const info=$('bitmapInfo');
  if(!canvas)return;
  const pct2=(used/total*100).toFixed(1);
  info.innerHTML='大文件区: '+total+' 扇区 | <span style="color:#7c4dff;">已用 '+used+'</span> <span style="color:#00e676;opacity:0.7;">空闲 '+free+'</span> | 占用 '+pct2+'%';
  const COLS=64, ROWS=62, CELL=6, GAP=1, STEP=CELL+GAP;
  const W=COLS*STEP-GAP, H=ROWS*STEP-GAP;
  canvas.width=W; canvas.height=H;
  const ctx=canvas.getContext('2d');
  for(let row=0;row<ROWS;row++){
    for(let col=0;col<COLS;col++){
      const idx=row*COLS+col;
      const byteIdx=Math.floor(idx/8),bitIdx=idx%8;
      const used2=(data[byteIdx]&(1<<bitIdx))!==0;
      ctx.fillStyle=used2?'#7c4dff':'#00e676';
      ctx.globalAlpha=used2?0.7:0.4;
      ctx.fillRect(col*STEP,row*STEP,CELL,CELL);
    }
  }
  ctx.globalAlpha=1;
  canvas.onmousemove=function(e){
    const rect=canvas.getBoundingClientRect();
    const scaleX=canvas.width/rect.width, scaleY=canvas.height/rect.height;
    const mx=(e.clientX-rect.left)*scaleX, my=(e.clientY-rect.top)*scaleY;
    const col2=Math.floor(mx/STEP), row2=Math.floor(my/STEP);
    if(col2>=0&&col2<COLS&&row2>=0&&row2<ROWS){
      const idx2=row2*COLS+col2;
      const sectorNum=idx2+64;  // AREA_LARGE_START_SECTOR = 64
      const byteIdx2=Math.floor(idx2/8),bitIdx2=idx2%8;
      const used3=(data[byteIdx2]&(1<<bitIdx2))!==0;
      $('bitmapHoverInfo').textContent='扇区 #'+sectorNum+' ('+(used3?'已用':'空闲')+')';
    }
  };
  $('bitmapModal').style.display='flex';
}

export function closeBitmapModal(){
  const el=$('bitmapModal');
  el.classList.add('closing');
  setTimeout(function(){el.style.display='none';el.classList.remove('closing');},200);
}
