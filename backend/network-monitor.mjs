import os from 'node:os';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';

const execFileAsync = promisify(execFile);
const finite = value => typeof value === 'number' && Number.isFinite(value);
const clamp = (value,min,max) => Math.min(max,Math.max(min,value));

async function safeExec(file,args) {
  try {
    const {stdout=''} = await execFileAsync(file,args,{encoding:'utf8',timeout:6500,windowsHide:true,maxBuffer:256*1024});
    return String(stdout);
  } catch(error) {
    return String(error?.stdout || '');
  }
}

export function parseWindowsRoute(text) {
  const routes=[];
  for(const line of String(text).split(/\r?\n/)) {
    const parts=line.trim().split(/\s+/);
    if(parts.length>=5&&parts[0]==='0.0.0.0'&&parts[1]==='0.0.0.0'&&/^\d+(\.\d+){3}$/.test(parts[2])) {
      routes.push({gateway_ip:parts[2],local_ip:parts[3],metric:Number(parts[4])});
    }
  }
  return routes.sort((a,b)=>(a.metric||Infinity)-(b.metric||Infinity))[0] || {};
}

export function parseWindowsWlan(text) {
  const source=String(text);
  const field=name => source.match(new RegExp(`^\\s*${name}\\s*:\\s*(.+?)\\s*$`,'mi'))?.[1]?.trim() ?? null;
  const numeric=name => {
    const raw=field(name);
    if(raw===null) return null;
    const value=Number.parseFloat(raw);
    return Number.isFinite(value)?value:null;
  };
  return {
    interface_name:field('Name'), ssid:field('SSID'), radio_type:field('Radio type'),
    channel:numeric('Channel'), signal_pct:numeric('Signal'), rssi_dbm:numeric('Rssi'),
    rx_rate_mbps:numeric('Receive rate \\(Mbps\\)'), tx_rate_mbps:numeric('Transmit rate \\(Mbps\\)'),
  };
}

export function parsePing(text,requested=3) {
  const source=String(text);
  const times=[...source.matchAll(/time[=<]\s*(\d+(?:\.\d+)?)\s*ms/gi)].map(match=>Number(match[1]));
  const windows=source.match(/Sent\s*=\s*(\d+).*Received\s*=\s*(\d+).*Lost\s*=\s*(\d+)/is);
  const linux=source.match(/(\d+)\s+packets transmitted,\s*(\d+)\s+(?:packets )?received/is);
  const sent=Number(windows?.[1] ?? linux?.[1] ?? requested);
  const received=Number(windows?.[2] ?? linux?.[2] ?? times.length);
  const latency=times.length?times.reduce((sum,value)=>sum+value,0)/times.length:null;
  return {latency_ms:latency,packet_loss_pct:sent>0?clamp(((sent-received)/sent)*100,0,100):null};
}

export function evaluateNetworkSample(sample) {
  const measured=finite(sample.latency_ms)||finite(sample.packet_loss_pct)||finite(sample.signal_pct);
  if(!sample.gateway_ip&&!measured) return {...sample,score:null,status:'UNKNOWN',optional_requests_allowed:null,
    policy_reason:'Gateway link metrics are not available'};
  let score=100;
  const reasons=[];
  if(sample.mqtt_connected===false) {score-=45;reasons.push('MQTT broker disconnected');}
  if(finite(sample.packet_loss_pct)) {
    score-=Math.min(50,sample.packet_loss_pct*0.7);
    if(sample.packet_loss_pct>5) reasons.push(`${sample.packet_loss_pct.toFixed(0)}% router packet loss`);
  }
  if(finite(sample.latency_ms)) {
    if(sample.latency_ms>100) score-=30;
    else if(sample.latency_ms>50) score-=18;
    else if(sample.latency_ms>20) score-=8;
    if(sample.latency_ms>20) reasons.push(`${sample.latency_ms.toFixed(1)} ms router latency`);
  }
  if(finite(sample.signal_pct)) {
    if(sample.signal_pct<40) score-=30;
    else if(sample.signal_pct<60) score-=18;
    else if(sample.signal_pct<75) score-=7;
    if(sample.signal_pct<60) reasons.push(`${sample.signal_pct.toFixed(0)}% gateway Wi-Fi signal`);
  }
  score=Math.round(clamp(score,0,100));
  const status=score>=85?'GOOD':score>=65?'FAIR':score>=40?'DEGRADED':'POOR';
  const optional=status==='GOOD'||status==='FAIR';
  return {...sample,score,status,optional_requests_allowed:optional,
    policy_reason:optional?'Network permits optional closer-monitoring requests':
      `Optional closer-monitoring requests withheld: ${reasons.join('; ')||'gateway link degraded'}`};
}

function interfaceForIp(ip) {
  for(const [name,addresses] of Object.entries(os.networkInterfaces())) {
    if(addresses?.some(address=>address.family==='IPv4'&&address.address===ip)) return name;
  }
  return null;
}

export async function collectNetworkSample({mqttConnected=false,telemetryRx60s=0,now=Date.now()}={}) {
  const platform=os.platform();
  let route={},wifi={},ping={latency_ms:null,packet_loss_pct:null};
  if(platform==='win32') {
    route=parseWindowsRoute(await safeExec('route.exe',['print','-4']));
    wifi=parseWindowsWlan(await safeExec('netsh.exe',['wlan','show','interfaces']));
    if(route.gateway_ip) ping=parsePing(await safeExec('ping.exe',['-n','3','-w','1000',route.gateway_ip]),3);
  } else if(platform==='linux') {
    const routeText=await safeExec('ip',['route','show','default']);
    const match=routeText.match(/default\s+via\s+(\S+).*?(?:dev\s+(\S+))?(?:.*?src\s+(\S+))?/);
    route={gateway_ip:match?.[1]||null,interface_name:match?.[2]||null,local_ip:match?.[3]||null};
    const ssid=(await safeExec('iwgetid',['-r'])).trim();
    wifi={interface_name:route.interface_name||null,ssid:ssid||null,signal_pct:null,rssi_dbm:null,rx_rate_mbps:null,tx_rate_mbps:null,radio_type:null,channel:null};
    if(route.gateway_ip) ping=parsePing(await safeExec('ping',['-c','3','-W','1',route.gateway_ip]),3);
  }
  const base={ts:now,platform,interface_name:wifi.interface_name||route.interface_name||interfaceForIp(route.local_ip)||null,
    local_ip:route.local_ip||null,gateway_ip:route.gateway_ip||null,ssid:wifi.ssid||null,
    signal_pct:wifi.signal_pct??null,rssi_dbm:wifi.rssi_dbm??null,radio_type:wifi.radio_type||null,
    channel:wifi.channel??null,rx_rate_mbps:wifi.rx_rate_mbps??null,tx_rate_mbps:wifi.tx_rate_mbps??null,
    latency_ms:ping.latency_ms??null,packet_loss_pct:ping.packet_loss_pct??null,
    mqtt_connected:!!mqttConnected,telemetry_rx_60s:Number(telemetryRx60s)||0,
    scope:'gateway-link-and-local-delivery'};
  return evaluateNetworkSample(base);
}
