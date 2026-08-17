// serial.js — 串口底层读写（连接/断开的编排逻辑见 app.js）
import { state, WRITE_TIMEOUT, log, toast } from './core.js';
import { feedRx } from './protocol.js';

export async function startRead(){
  state.readLoop=true;
  while(state.port&&state.port.readable&&state.readLoop){
    try{
      state.reader=state.port.readable.getReader();
      while(true){const{value,done}=await state.reader.read();if(done)break;if(value&&value.length)feedRx(value);}
    }catch(e){if(state.readLoop)log('读错误: '+e.message,'error');}
    finally{if(state.reader){try{state.reader.releaseLock()}catch(e){}state.reader=null;}}
    if(!state.readLoop)break;
    await new Promise(r=>setTimeout(r,200));
  }
}

export async function writeSer(data){
  if(!state.port||!state.port.writable)return false;
  let w=null;
  try{
    w=state.port.writable.getWriter();
    await Promise.race([
      w.write(data),
      new Promise(function(_,reject){state.writeTimer=setTimeout(function(){reject(new Error('写超时'));},WRITE_TIMEOUT);})
    ]);
    return true;
  }catch(e){
    log('写错误: '+e.message,(e.message==='写超时'?'warn':'error'));
    if(e.message==='写超时')toast('USB写入超时','error');
    return false;
  }finally{
    if(state.writeTimer){clearTimeout(state.writeTimer);state.writeTimer=null;}
    if(w){try{w.releaseLock()}catch(e){}}
  }
}
