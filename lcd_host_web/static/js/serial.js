// Serial compatibility facade. DeviceSession owns lifecycle and I/O.
import { state, WRITE_TIMEOUT, log, toast } from './core.js';
import { feedRx } from './protocol.js';
import { DeviceSession } from './session.js';

export const deviceSession = new DeviceSession({
  onData: feedRx,
  onStateChange: (next, error) => {
    if (error) log(`设备状态: ${next} (${error.message})`, 'error');
  },
});

export function setDeviceActivity(activity) {
  if (deviceSession.port && deviceSession.state !== activity) deviceSession.transition(activity);
}

export async function startRead(){
  state.readLoop=true;
  try { await deviceSession.startRead(); }
  catch(e){ if(state.readLoop) log('串口出错: '+e.message,'error'); }
}

export async function writeSer(data){
  try { return await deviceSession.write(data, WRITE_TIMEOUT); }
  catch(e){ log('写错误: '+e.message, 'error'); toast('USB写入失败','error'); return false; }
}
