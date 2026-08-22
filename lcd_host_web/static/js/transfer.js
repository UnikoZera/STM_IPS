// transfer.js — 文件列表、文件详情/删除、烧录发送（帧流控制）
import {
  $, state, elFileCnt, elFileList, elFLBody, elFileInfo, elName, elNameCnt,
  elBtnSend, elBtnCancel, elBtnRetry, elStatus, elProg, elPct, elBytes,
  C_L, C_S, C_END, C_DEL, C_QRY, C_LCD, C_ABORT,
  ST_INIT, ST_SENDING, ST_WAIT_ACK, ST_DONE, ST_ERR, SEND_TIMEOUT,
  escHtml, escAttr, log, toast,
} from './core.js';
import { frame, signalCont, waitCont, resetCont } from './protocol.js';
import { writeSer, setDeviceActivity } from './serial.js';
import { lcdStreamQuery } from './lcd.js';

/* ===== File List ===== */
export function parseList(payload){
  if(payload.length<2)return;
  const cnt=payload[0];
  const slotCount=payload[1];
  elFileCnt.textContent='共 '+cnt+' 个';

  let slots=[]; let idx=2;
  for(let i=0;i<slotCount;i++){
    if(idx>=payload.length)break;
    const rLen=payload[idx];if(idx+rLen>payload.length)break;
    const tag=payload[idx+1];
    if(tag===0xFF&&rLen===10){
      const startSector=new DataView(payload.buffer,payload.byteOffset+idx+2,4).getUint32(0,true);
      const sectorCount=new DataView(payload.buffer,payload.byteOffset+idx+6,4).getUint32(0,true);
      slots.push({startSector,sectorCount});
    }
    idx+=rLen;
  }

  let files=[];
  for(let i=0;idx<payload.length;i++){
    if(idx>=payload.length)break;
    const rLen=payload[idx];if(rLen<12||idx+rLen>payload.length)break;
    const tag=payload[idx+1],fi=payload[idx+2],nLen=payload[idx+3];
    if(nLen>16||idx+12+nLen>payload.length)break;
    if(tag===0xFF)break;
    const zn=(tag&0x80)?'large':'small',fType=tag&0x7F;
    const name=new TextDecoder('ascii').decode(payload.slice(idx+4,idx+4+nLen)).replace(/\0/g,'');
    const addr=new DataView(payload.buffer,payload.byteOffset+idx+4+nLen,4).getUint32(0,true);
    const size=new DataView(payload.buffer,payload.byteOffset+idx+8+nLen,4).getUint32(0,true);
    let sectorCount=0, imgW=0xFFFF, imgH=0xFFFF;
    if(zn==='large'&&idx+16+nLen<=payload.length){
      sectorCount=new DataView(payload.buffer,payload.byteOffset+idx+12+nLen,4).getUint32(0,true);
      // width/height at offset idx+16+nLen (if rLen >= 20+nLen)
      if(idx+20+nLen<=payload.length){
        imgW=new DataView(payload.buffer,payload.byteOffset+idx+16+nLen,2).getUint16(0,true);
        imgH=new DataView(payload.buffer,payload.byteOffset+idx+18+nLen,2).getUint16(0,true);
      }
    }else if(zn==='small'&&idx+16+nLen<=payload.length){
      // width/height at offset idx+12+nLen (if rLen >= 16+nLen)
      imgW=new DataView(payload.buffer,payload.byteOffset+idx+12+nLen,2).getUint16(0,true);
      imgH=new DataView(payload.buffer,payload.byteOffset+idx+14+nLen,2).getUint16(0,true);
    }
    files.push({zn,zName:zn==='large'?'大文件':'小文件',fType,fi,name,addr,size,sectorCount,imgW,imgH});
    idx+=rLen;
  }

  state.lastFileListData=files;

  /* 删除确认兜底：目标文件已从列表消失 → 删除实际完成（即使 0xA1 确认帧丢失） */
  if(state.delPending && state.delTarget){
    const stillThere=files.some(f=>f.fType===state.delTarget.ft && f.fi===state.delTarget.fi);
    if(!stillThere){
      if(state.delTimer){clearTimeout(state.delTimer);state.delTimer=null;}
      if(state.delPollTimer){clearInterval(state.delPollTimer);state.delPollTimer=null;}
      state.delPending=false;state.delTarget=null;
      toast('删除完成','success');
    }
  }


  let h='';
  files.forEach(function(f,order){
    const szBytes=f.size;
    const szStr=szBytes<1024?szBytes+' B':szBytes<1048576?(szBytes/1024).toFixed(1)+' KB':(szBytes/1048576).toFixed(2)+' MB';
    const tagCls=f.zn==='large'?'tag-L':'tag-S',zoneLabel=f.zName;
    const largeDetail=f.zn==='large'?'扇区 #'+f.addr+' ('+f.sectorCount+' sectors)':'地址:0x'+f.addr.toString(16).toUpperCase();
    const sizeIcon=f.zn==='large'?'\u{1F4E6}':'\u{1F4C4}';
    h+='<div class="file-item" style="animation-delay:'+(order*.04)+'s" data-action="openFileDetail" data-index="'+order+'">'
      +'<div class="file-item-name"><span class="file-icon">'+sizeIcon+'</span> ['+f.fi+'] '+escHtml(f.name||'(无名)')+'</div>'
      +'<div class="file-item-meta">'
      +'<span class="tag '+tagCls+'">#'+f.fi+' '+zoneLabel+'</span>'
      +'<span class="sep">|</span><span>'+szStr+'</span>'
      +'<span class="sep">|</span><span>'+largeDetail+'</span>'
      +'</div>'
      +'<div class="file-item-actions">'
      +'<button class="btn btn-danger" data-action="delFile" data-ft="'+f.fType+'" data-fi="'+f.fi+'" data-name="'+escAttr(f.name||'')+'">&#128465; 删除</button>'
      +'</div></div>';
  });
  if(!files.length)h='<div class="empty-state"><span class="icon">&#9744;</span>设备中没有文件</div>';
  elFileList.innerHTML=h;
  elFLBody.scrollTop=0;
}

/* ===== File Detail Modal ===== */
export function closeModal(){
  const el=$('fileDetailModal');
  el.classList.add('closing');
  setTimeout(function(){el.style.display='none';el.classList.remove('closing');},200);
}
export function openFileDetailByIndex(idx){
  if(state.lastFileListData&&state.lastFileListData[idx])openFileDetail(state.lastFileListData[idx]);
}
function openFileDetail(f){
  $('modalTitle').textContent='\u{1F4C4} 文件详情';
  const body=$('modalBody');
  const szBytes=f.size;
  const szFormatted=szBytes<1024?szBytes+' B':szBytes<1048576?(szBytes/1024).toFixed(1)+' KB<span class="fd-bytes">('+szBytes+' B)</span>':(szBytes/1048576).toFixed(2)+' MB<span class="fd-bytes">('+szBytes+' B)</span>';
  const zone=f.zn==='large'?'大文件区 (Large)':'小文件区 (Small)';
  const addrStr=f.zn==='large'?'扇区 #'+f.addr:'0x'+f.addr.toString(16).toUpperCase();
  body.innerHTML=''
    +'<div class="fd-row"><span class="fd-label">文件名</span><span class="fd-value">'+escHtml(f.name||'(无名)')+'</span></div>'
    +'<div class="fd-row"><span class="fd-label">文件索引</span><span class="fd-value">#'+f.fi+'</span></div>'
    +'<div class="fd-row"><span class="fd-label">存储区域</span><span class="fd-value">'+zone+'</span></div>'
    +'<div class="fd-row"><span class="fd-label">类型代码</span><span class="fd-value">0x'+f.fType.toString(16).toUpperCase().padStart(2,'0')+'</span></div>'
    +'<div class="fd-row"><span class="fd-label">文件大小</span><span class="fd-value">'+szFormatted+'</span></div>'
    +'<div class="fd-row"><span class="fd-label">地址/扇区</span><span class="fd-value">'+addrStr+'</span></div>'
    + (f.zn==='large'?'<div class="fd-row"><span class="fd-label">占用扇区</span><span class="fd-value">#'+f.addr+' ~ #'+(f.addr+f.sectorCount-1)+' ('+f.sectorCount+' sectors)</span></div>':'')
    + (f.imgW!==undefined&&f.imgW!==0xFFFF&&f.imgW>0?'<div class="fd-row"><span class="fd-label">图像尺寸</span><span class="fd-value">'+f.imgW+' \u00D7 '+f.imgH+' px</span></div>':'');
  $('fileDetailModal').style.display='flex';
}
export function openDeleteModal(){
  if(!state.lastFileListData||!state.lastFileListData.length){
    toast('请先刷新文件列表','warn');
    return;
  }
  $('modalTitle').textContent='\u{1F5D1} 删除文件';
  const body=$('modalBody');
  let h='<div style="font-size:11px;color:var(--text2);margin-bottom:10px;">点击文件进行删除（共 '+state.lastFileListData.length+' 个）</div>';
  state.lastFileListData.forEach(function(f){
    const sz=f.size<1024?f.size+'B':f.size<1048576?(f.size/1024).toFixed(1)+'KB':(f.size/1048576).toFixed(2)+'MB';
    h+='<div class="fd-row" style="cursor:pointer;" data-action="delFromModal" data-ft="'+f.fType+'" data-fi="'+f.fi+'" data-name="'+escAttr(f.name||'')+'">'
      +'<span class="fd-label" style="width:60px;">#'+f.fi+'</span>'
      +'<span class="fd-value" style="font-size:12px;">'+escHtml(f.name||'(无名)')+'</span>'
      +'<span class="fd-label" style="width:80px;text-align:right;">'+f.zName+'</span>'
      +'<span class="fd-label" style="width:80px;text-align:right;">'+sz+'</span>'
      +'</div>';
  });
  body.innerHTML=h;
  $('fileDetailModal').style.display='flex';
}
export function delFromModal(ft,fi,name){
  closeModal();
  setTimeout(function(){delFIClick(ft,fi,name);},250);
}

/* ===== Delete ===== */
export async function delFIClick(ft,fi,name){
  if(!state.port){toast('未连接串口','error');return;}
  if(state.delPending){toast('删除操作进行中','warn');return;}
  const typeName=ft===C_L?'大文件':'小文件';
  if(!confirm('确认删除 '+typeName+' #'+fi+' ('+name+')？\n\n注意：删除后W25Q扇区将被擦除并回收空间。'))return;
  state.delPending=true;
  setDeviceActivity('Deleting');
  state.delTarget={ft,fi};
  const d=new Uint8Array([ft&0xFF,fi&0xFF]);
  const ok=await writeSer(frame(C_DEL,d));
  if(ok){
    log('HOST \u2192 删除 '+typeName+' #'+fi+' ('+name+')','send');
    toast('正在删除: '+name,'warn'); /* 等 MCU 擦除完成回 0xA1 再刷新列表 */
    /* 超时兜底：大文件擦除可达数分钟（扇区数×150ms），超时仅解锁，不误报完成 */
    state.delTimer=setTimeout(function(){
      if(state.delPending){state.delPending=false;state.delTarget=null;state.delTimer=null;setDeviceActivity('Idle');
        if(state.delPollTimer){clearInterval(state.delPollTimer);state.delPollTimer=null;}
        toast('删除超时（MCU 未确认，可能仍在擦除）','error');}
    },600000);
    /* 轮询确认：每 2s 查一次列表，0xA1 确认帧丢失时靠文件消失兜底解锁 */
    state.delPollTimer=setInterval(function(){
      if(!state.delPending){if(state.delPollTimer){clearInterval(state.delPollTimer);state.delPollTimer=null;}return;}
      writeSer(frame(C_QRY,new Uint8Array(0)));
    },2000);
  }else{
    toast('删除命令发送失败','error');
    state.delPending=false;state.delTarget=null;
    setDeviceActivity('Idle');
  }
}

/* ===== Send ===== */
export function onFileSelected(input){
  const f=input.files[0];if(!f)return;
  state.selFile=f;
  const sz=f.size<1024?f.size+'B':f.size<1048576?(f.size/1024).toFixed(1)+'KB':(f.size/1048576).toFixed(2)+'MB';
  elFileInfo.textContent='\u2714 '+f.name+' ('+sz+')';
  elName.value=f.name.replace(/\.[^.]+$/,'').slice(0,15);updateNBC();
  elBtnSend.disabled=false;
}
export function updateNBC(){
  const b=new TextEncoder().encode(elName.value).length;
  elNameCnt.textContent=b+' / 15';elNameCnt.style.color=b>15?'var(--red)':'var(--text2)';
}

export async function userStartSend(){
  if(!state.selFile){toast('请选择文件','error');return;}
  /* LCD 帧流开启时不再弹提示拦截：doSend 会自动接管关闭帧流（烧录完成自动恢复） */
  const cmd=parseInt($('fileType').value);
  let fn=elName.value.trim();if(!fn)fn=state.selFile.name.replace(/\.[^.]+$/,'').slice(0,15);
  if(new TextEncoder().encode(fn).length>15){toast('文件名编码超15字节','error');return;}
  const sz=state.selFile.size<1024?state.selFile.size+'B':(state.selFile.size/1024).toFixed(1)+'KB';
  const cn=cmd===C_L?'大文件':'小文件';
  if(!confirm('发送确认:\n  本地: '+state.selFile.name+'\n  设备名: '+fn+'\n  类型: '+cn+' ('+sz+')?'))return;
  await doSend(state.selFile,cmd,fn);
}

export async function doSend(file,cmd,fn){
  if(!state.port||!state.port.writable){toast('未连接串口','error');return;}
  if(state.delPending){toast('删除进行中，请稍候','warn');return;}
  if(state.sendState!==ST_INIT&&state.sendState!==ST_DONE&&state.sendState!==ST_ERR){toast('正在发送中','error');return;}
  state.sendState=ST_INIT;
  resetCont();
  if(state.lcdQueryTid){clearTimeout(state.lcdQueryTid);state.lcdQueryTid=null;}
  state.fileData=null;
  state.fileSource=file;state.fileSize=file.size;state.fileOff=0;state.fileCmd=cmd;state.fileName=fn;
  state.sendState=ST_SENDING;
  setDeviceActivity('Uploading');
  state.lcdStreamWasOn=($('lcdStreamStatus').textContent||'').indexOf('已开启')>=0;
  if(state.lcdStreamWasOn){
    const ackPromise=new Promise(function(r){state.lcdAckResolve=r;});
    const ok=await writeSer(frame(C_LCD,new Uint8Array([0x00])));
    if(ok){log('HOST \u2192 临时关闭LCD流以进行烧录','send');}
    await Promise.race([ackPromise,new Promise(function(r){setTimeout(r,1000);})]);
    state.lcdAckResolve=null;
    state.rxBuf=new Uint8Array(0);
  }
  state.isTransferring=true;state.contPending=0;
  const totalStr=state.fileSize<1024?state.fileSize+' B':(state.fileSize/1024).toFixed(1)+' KB';
  log('\u2500\u2500 开始发送: '+fn+' ('+totalStr+') \u2500\u2500','info');
  setUI('sending');updateProg(0,0,state.fileSize);
  try{await sendLoop();}
  catch(e){log('发送异常: '+e.message,'error');await doFinish(false);}
}

async function sendLoop(){
  while(state.sendState===ST_SENDING){
    if(state.fileOff>=state.fileSize){await sendEndFrame();return;}
    const len=Math.min(state.fileSize-state.fileOff,1022); /* 固定 1022B 数据 + 2B CRC16 一包（与 MCU CRC 块对齐） */
    const chunk=new Uint8Array(await state.fileSource.slice(state.fileOff,state.fileOff+len).arrayBuffer());
    const totalSize=(state.fileOff===0)?state.fileSize:0;
    const frm=frame(state.fileCmd,chunk,totalSize);
    const ok=await writeSer(frm);
    if(!ok){doAbort('写入失败');return;}
    state.fileOff+=len;updateProg(state.fileOff/state.fileSize*100,state.fileOff,state.fileSize);
    state.sendState=ST_WAIT_ACK;elStatus.innerHTML='<span class="spinner"></span> 等待MCU就绪...';
    state.sendTimer=setTimeout(function(){if(state.sendState===ST_WAIT_ACK)doAbort('等待MCU回应超时');},SEND_TIMEOUT);
    await waitCont();
    if(state.sendTimer){clearTimeout(state.sendTimer);state.sendTimer=null;}
    if(state.sendState!==ST_WAIT_ACK)return;
    state.sendState=ST_SENDING;elStatus.textContent='';
  }
}

async function sendEndFrame(){
  const enc=new TextEncoder(),nb=enc.encode(state.fileName),pn=new Uint8Array(16);
  pn.set(nb.slice(0,15));
  const frm=frame(C_END,pn);
  const ok=await writeSer(frm);
  if(ok){log('HOST \u2192 结束帧 文件名='+state.fileName,'send');}
  state.sendState=ST_WAIT_ACK;elStatus.innerHTML='<span class="spinner"></span> 等待确认...';
  state.sendTimer=setTimeout(function(){if(state.sendState===ST_WAIT_ACK)signalCont();},3000);
  await waitCont();
  if(state.sendTimer){clearTimeout(state.sendTimer);state.sendTimer=null;}
  if(state.sendState!==ST_WAIT_ACK){return;} // MCU 返回了错误，doAbort 已处理清理
  await doFinish(true);
}

export async function doFinish(ok){
  if(state.sendState===ST_DONE)return;
  state.sendState=ST_DONE;
  state.isTransferring=false;
  if(ok){
    const totalStr=state.fileSize?state.fileSize+' B':'';
    log('\u2500\u2500 发送完成: '+state.fileName+' ('+totalStr+') \u2500\u2500','info');
    updateProg(100,state.fileSize,state.fileSize||1);
    toast('发送完成: '+state.fileName,'success');
    setTimeout(function(){updateProg(0,0,0);},1200);
  }else{
    log('\u2500\u2500 发送中止 \u2500\u2500','error');
    updateProg(0,0,0);
  }
  if(state.lcdStreamWasOn){
    await writeSer(frame(C_LCD,new Uint8Array([0x01])));
    setDeviceActivity('Streaming');
    log('HOST \u2192 恢复LCD流','send');
  }
  if(!state.lcdStreamWasOn && ok) setDeviceActivity('Idle');
  state.fileData=null;state.fileSource=null;state.fileSize=0;state.fileOff=0;state.fileName='';
  resetCont();setUI('idle');
  if(ok){state.lcdQueryTid=setTimeout(function(){lcdStreamQuery();state.lcdQueryTid=null;},400);setTimeout(function(){queryFileList();},800);}
  else{setUI('error');setDeviceActivity('Error');}
}

export function doAbort(reason){
  log('中止: '+reason,'error');toast('传输中止: '+reason,'error');
  state.isTransferring=false;
  // 通知MCU中止下载，触发扇区回滚
  writeSer(frame(C_ABORT,new Uint8Array(0)));
  if(state.lcdStreamWasOn){
    writeSer(frame(C_LCD,new Uint8Array([0x01])));
    setDeviceActivity('Streaming');
  }
  resetCont();state.sendState=ST_ERR;
  state.fileData=null;state.fileSource=null;state.fileSize=0;state.fileOff=0;state.fileName='';
  updateProg(0,0,0);setUI('error');
}

export function cancelSend(){
  if(state.sendState===ST_INIT||state.sendState===ST_DONE||state.sendState===ST_ERR)return;
  log('用户取消','warn');doAbort('用户取消');
}

export async function retrySend(){
  const f=state.selFile;let fn=elName.value.trim();
  if(!f){toast('请重新选择文件','error');return;}
  if(!fn)fn=f.name.replace(/\.[^.]+$/,'').slice(0,16);
  const cmd=parseInt($('fileType').value);
  state.sendState=ST_INIT;elBtnRetry.style.display='none';elBtnSend.style.display='block';
  await doSend(f,cmd,fn);
}

export function setUI(st){
  if(st==='sending'){elBtnSend.style.display='none';elBtnCancel.style.display='block';elBtnRetry.style.display='none';}
  else if(st==='error'){elBtnSend.style.display='none';elBtnCancel.style.display='none';elBtnRetry.style.display='block';elStatus.textContent='';}
  else{elBtnSend.style.display='block';elBtnCancel.style.display='none';elBtnRetry.style.display='none';elBtnSend.disabled=true;elStatus.textContent='';elFileInfo.textContent='未选择文件';}
}

function updateProg(pct,done,total){
  elProg.style.width=pct+'%';
  const r=Math.round(255*(1-pct/100)),g=Math.round(23*(1-pct/100)+230*(pct/100)),b=Math.round(68*(1-pct/100)+118*(pct/100));
  const c='rgb('+r+','+g+','+b+')';
  elProg.style.background=c;elProg.style.boxShadow='0 0 10px rgba('+r+','+g+','+b+',.35)';
  if(state.sendState===ST_SENDING||state.sendState===ST_WAIT_ACK)elProg.classList.add('animating');
  else elProg.classList.remove('animating');
  elPct.textContent=pct>0?pct.toFixed(1)+'%':'';
  if(total>0){
    const ds=done<1024?done+' B':done<1048576?(done/1024).toFixed(1)+' KB':(done/1048576).toFixed(2)+' MB';
    const ts=total<1024?total+' B':total<1048576?(total/1024).toFixed(1)+' KB':(total/1048576).toFixed(2)+' MB';
    elBytes.textContent=ds+' / '+ts;
  }else elBytes.textContent='';
}

/* ===== Commands ===== */
export async function queryFileList(){
  if(!state.port){toast('未连接','error');return;}
  const ok=await writeSer(frame(C_QRY,new Uint8Array(0)));
  if(ok)log('HOST \u2192 查询文件列表','send');
  else toast('查询命令发送失败','error');
}
