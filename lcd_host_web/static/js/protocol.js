// protocol.js — BB44 帧构建、AA55 接收解析、continuation 握手
import { H0, H1, HS, UH0, UH1, UHS, crc16, state, log } from './core.js';

export function frame(cmd,data,totalFileSize) {
  const dL=data?data.length:0, pL=dL+2, tfs=totalFileSize||0, total=HS+dL+2;
  const f=new Uint8Array(total);
  f[0]=H0;f[1]=H1;f[2]=cmd;
  f[3]=tfs&0xFF;f[4]=(tfs>>>8)&0xFF;f[5]=(tfs>>>16)&0xFF;f[6]=(tfs>>>24)&0xFF;
  f[7]=pL&0xFF;f[8]=(pL>>>8)&0xFF;
  if(data&&dL)f.set(data,HS);
  /* CRC16 只覆盖 payload 数据（不含帧头）：
   * 完整 1022B 数据包的 CRC 即存储块 CRC（1022 数据 + 2 CRC），MCU 验证通过后整包直接烧录。
   * 空 payload 的 CRC 为 0x0000（crc16(空)=0xFFFF^0xFFFF），与 MCU crc16_usb_calc 一致。 */
  const crc=(data&&dL)?crc16(data,0,dL):0x0000;
  f[HS+dL]=crc&0xFF;f[HS+dL+1]=(crc>>>8)&0xFF;
  return f;
}

/* ===== RX ===== */
let respHandler = null;
export function setRespHandler(fn) { respHandler = fn; }

export function feedRx(data) {
  let c=new Uint8Array(state.rxBuf.length+data.length);c.set(state.rxBuf);c.set(data,state.rxBuf.length);state.rxBuf=c;
  while(state.rxBuf.length>=UHS){
    let hi=-1;
    for(let i=0;i<=state.rxBuf.length-2;i++){if(state.rxBuf[i]===UH0&&state.rxBuf[i+1]===UH1){hi=i;break;}}
    if(hi<0){state.rxBuf=state.rxBuf.length>1?state.rxBuf.slice(state.rxBuf.length-1):state.rxBuf;return;}
    if(hi>0)state.rxBuf=state.rxBuf.slice(hi);
    if(state.rxBuf.length<UHS)return;
    const cmd=state.rxBuf[2],pLen=state.rxBuf[3]|(state.rxBuf[4]<<8),fLen=UHS+pLen;
    if(pLen>32768){state.rxBuf=state.rxBuf.slice(1);log('无效帧长度 '+pLen,'error');continue;}
    if(state.rxBuf.length<fLen)return;
    if(respHandler)respHandler(cmd,state.rxBuf.slice(UHS,fLen));
    state.rxBuf=state.rxBuf.slice(fLen);
  }
}

/* ===== Continuation ===== */
export function signalCont(){state.contPending++;if(state.contResolve){const r=state.contResolve;state.contResolve=null;state.contPending--;r();}}
export function waitCont(){
  if(state.contPending>0){state.contPending--;return Promise.resolve();}
  return new Promise(function(r){state.contResolve=r;});
}
export function resetCont(){
  if(state.contResolve){const r=state.contResolve;state.contResolve=null;state.contPending=0;r();}else{state.contPending=0;}
  if(state.writeTimer){clearTimeout(state.writeTimer);state.writeTimer=null;}
  if(state.sendTimer){clearTimeout(state.sendTimer);state.sendTimer=null;}
}
