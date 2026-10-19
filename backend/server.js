import express from 'express';
import {createServer} from 'node:http';
import {randomUUID} from 'node:crypto';
import {existsSync} from 'node:fs';
import {dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {Server as IO} from 'socket.io';
import mqtt from 'mqtt';
import cors from 'cors';
import Database from 'better-sqlite3';
import {gatewaySnapshot, normalizeReading, validNodeId, validLease} from './gateway.mjs';
import {collectNetworkSample} from './network-monitor.mjs';

const PORT = Number(process.env.PORT || 3002);
const MQTT_URL = process.env.MQTT_URL || 'mqtt://localhost:1883';
const TOPIC_ROOT = process.env.TOPIC_ROOT || 'bin';
if (!/^[A-Za-z0-9_-]+$/.test(TOPIC_ROOT)) throw Error('Unsafe topic root');
// Cross-origin callers only (the Vite dev server). The built dashboard is served
// by this server below, so it is same-origin and needs no entry here.
const DASHBOARD_ORIGIN = (process.env.DASHBOARD_ORIGIN || 'http://localhost:5173')
  .split(',').map(origin=>origin.trim()).filter(Boolean);
// 0.0.0.0 so the presentation laptop and phones on Smart-Network can reach it.
const BIND_HOST = process.env.BIND_HOST || '0.0.0.0';
const DASHBOARD_DIST = join(dirname(fileURLToPath(import.meta.url)), '..', 'dashboard', 'dist');
const db = new Database(process.env.DB_PATH || 'smart-bin-gateway.db');
db.pragma('journal_mode = WAL');
const fields = {
  node_id:'TEXT',ts:'INTEGER',fill_pct:'REAL',temp_c:'REAL',humidity:'REAL',gas_135:'INTEGER',gas_5:'INTEGER',
  batt_pct:'REAL',state:'TEXT',mode:'TEXT',tx_count:'INTEGER',interval:'INTEGER',deadband:'REAL',dfdt:'REAL',
  reason:'TEXT',tx_trigger:'TEXT',state_cause:'TEXT',tx_dropped:'INTEGER',sensor_fault:'INTEGER',rx_ts:'INTEGER',run_label:'TEXT',
  sensor_profile:'TEXT',power_session:'TEXT',power_profile:'TEXT',power_mode:'TEXT',policy_interval:'REAL',next_interval:'REAL',control_id:'TEXT',control_source:'TEXT',
  control_ttl:'REAL',control_version:'INTEGER',ina_present:'INTEGER',bus_voltage_v:'REAL',current_ma:'REAL',
  power_mw:'REAL',energy_mwh:'REAL',power_sample_ms:'INTEGER',
};
db.exec(`CREATE TABLE IF NOT EXISTS readings (id INTEGER PRIMARY KEY AUTOINCREMENT,
  ${Object.entries(fields).map(([key,type])=>`${key} ${type}`).join(',')})`);
const existing = new Set(db.prepare('PRAGMA table_info(readings)').all().map(c=>c.name));
for (const [key,type] of Object.entries(fields)) if (!existing.has(key)) db.exec(`ALTER TABLE readings ADD COLUMN ${key} ${type}`);
db.exec(`CREATE INDEX IF NOT EXISTS readings_node_rx ON readings(node_id,rx_ts);
  CREATE TABLE IF NOT EXISTS gateway_commands (
    request_id TEXT PRIMARY KEY, node_id TEXT, interval_s INTEGER, ttl_s INTEGER, issued_at INTEGER,
    status TEXT, ack_at INTEGER, ack_json TEXT);
  CREATE TABLE IF NOT EXISTS network_samples (
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, run_label TEXT,
    platform TEXT, interface_name TEXT, local_ip TEXT, gateway_ip TEXT, ssid TEXT,
    signal_pct REAL, rssi_dbm REAL, radio_type TEXT, channel REAL,
    rx_rate_mbps REAL, tx_rate_mbps REAL, latency_ms REAL, packet_loss_pct REAL,
    mqtt_connected INTEGER, telemetry_rx_60s INTEGER, score REAL, status TEXT,
    optional_requests_allowed INTEGER, policy_reason TEXT, scope TEXT);
  CREATE INDEX IF NOT EXISTS network_samples_ts ON network_samples(ts);
  CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT)`);
const insert = db.prepare(`INSERT INTO readings (${Object.keys(fields).join(',')}) VALUES (${Object.keys(fields).map(k=>'@'+k).join(',')})`);
const networkFields=['ts','run_label','platform','interface_name','local_ip','gateway_ip','ssid','signal_pct','rssi_dbm',
  'radio_type','channel','rx_rate_mbps','tx_rate_mbps','latency_ms','packet_loss_pct','mqtt_connected',
  'telemetry_rx_60s','score','status','optional_requests_allowed','policy_reason','scope'];
const insertNetwork=db.prepare(`INSERT INTO network_samples (${networkFields.join(',')}) VALUES (${networkFields.map(k=>'@'+k).join(',')})`);
let currentRunLabel = db.prepare("SELECT value FROM settings WHERE key='run_label'").get()?.value || 'unlabelled';
const latest = new Map(db.prepare(`SELECT r.* FROM readings r JOIN
  (SELECT node_id,MAX(id) id FROM readings GROUP BY node_id) n ON r.id=n.id`).all().map(row=>[row.node_id,row]));
const networkBooleans=row=>row?{...row,mqtt_connected:!!row.mqtt_connected,
  optional_requests_allowed:row.optional_requests_allowed===null?null:!!row.optional_requests_allowed}:null;
let latestNetwork=networkBooleans(db.prepare('SELECT * FROM network_samples ORDER BY id DESC LIMIT 1').get());
let retainedIgnored = 0, rejectedMessages = 0;
const app = express();
const httpServer = createServer(app);
const io = new IO(httpServer, {cors:{origin:DASHBOARD_ORIGIN}});
app.use(cors({origin:DASHBOARD_ORIGIN}));
app.use(express.json({limit:'16kb'}));
const mqttClient = mqtt.connect(MQTT_URL, {clientId:`pp2-gateway-${randomUUID().slice(0,8)}`, queueQoSZero:false});
const snapshot = () => {
  const now=Date.now();
  const state = gatewaySnapshot([...latest.values()]);
  const network=latestNetwork?{...latestNetwork,age_s:Math.max(0,(now-latestNetwork.ts)/1000)}:null;
  const networkBlocksOptional=network?.age_s<=45&&network.optional_requests_allowed===false;
  const recentDelivery=new Map(db.prepare('SELECT node_id,COUNT(*) receipts_5m FROM readings WHERE rx_ts>=? GROUP BY node_id')
    .all(now-300000).map(row=>[row.node_id,row]));
  const busy = new Set(db.prepare("SELECT node_id FROM gateway_commands WHERE status IN ('pending','accepted','duplicate') AND issued_at + ttl_s * 1000 + 10000 > ?").all(now).map(c=>c.node_id));
  for (const node of state.nodes) {
    node.request_outstanding = busy.has(node.node_id);
    node.feedback_eligible = node.feedback_eligible && !node.request_outstanding && !networkBlocksOptional;
    const delivery=recentDelivery.get(node.node_id);
    node.delivery_observation={receipts_5m:delivery?.receipts_5m||0,
      receipt_rate_min:(delivery?.receipts_5m||0)/5,
      source:'Gateway database receipts; this is application delivery activity, not node RSSI'};
  }
  const available = state.nodes.find(node=>node.feedback_eligible);
  state.selected_node = available?.node_id ?? null;
  state.decision = networkBlocksOptional ? `${network.policy_reason}. Local node urgency rules remain active.` :
    available ? `${available.node_id}: ${available.priority_reason}; eligible for a bounded 60 s request` :
    'No eligible node: waiting for fresh supported telemetry or an outstanding request to expire';
  return {...state, mqtt_connected:mqttClient.connected,
  network,
  run_label:currentRunLabel, topic_root:TOPIC_ROOT, retained_ignored:retainedIgnored, rejected_messages:rejectedMessages,
  commands:db.prepare('SELECT * FROM gateway_commands ORDER BY issued_at DESC LIMIT 20').all().map(c=>({
    ...c, status:c.status==='pending' && Date.now()-c.issued_at>10000 ? 'no-ack' : c.status,
    lease_elapsed:Date.now()>c.issued_at+c.ttl_s*1000,
    ack:c.ack_json ? JSON.parse(c.ack_json) : null, ack_json:undefined}))};
};

mqttClient.on('connect',()=>mqttClient.subscribe([`${TOPIC_ROOT}/+/data`,`${TOPIC_ROOT}/+/ack`]));
mqttClient.on('error',error=>console.error('[MQTT]',error.message));
mqttClient.on('message',(topic,payload,packet)=>{
  // Retained telemetry has no trustworthy receipt age. Re-inserting it on
  // reconnect would fabricate a fresh sample and inflate experiment counts.
  if (packet.retain) { retainedIgnored++; return; }
  try {
    if (payload.length>4096) throw Error('Payload too large');
    const [root,nodeId,kind,...extra] = topic.split('/');
    if (root!==TOPIC_ROOT || extra.length || !validNodeId(nodeId)) throw Error('Invalid topic');
    const value = JSON.parse(payload.toString());
    if (kind==='ack') {
      if (value.node_id!==nodeId || typeof value.request_id!=='string') throw Error('Invalid acknowledgment');
      const command = db.prepare('SELECT * FROM gateway_commands WHERE request_id=? AND node_id=?').get(value.request_id,nodeId);
      if (!command) throw Error('Unmatched acknowledgment');
      const statuses = ['accepted','duplicate','expired','cancelled','rejected-baseline','rejected-invalid','rejected-protected'];
      if (!statuses.includes(value.status)) throw Error('Unknown acknowledgment status');
      db.prepare('UPDATE gateway_commands SET status=?,ack_at=?,ack_json=? WHERE request_id=?').run(
        value.status,Date.now(),JSON.stringify(value),value.request_id);
      io.emit('gateway',snapshot());
      return;
    }
    if (kind!=='data') return;
    const row = normalizeReading(nodeId,value,Date.now(),currentRunLabel);
    row.id = Number(insert.run(row).lastInsertRowid);
    latest.set(nodeId,row);
    io.emit('reading',row);
    io.emit('gateway',snapshot());
    console.log(`[RX] ${nodeId} ${row.mode} ${row.state} fill=${row.fill_pct} run=${row.run_label}`);
  } catch(error) { rejectedMessages++; console.error('[MQTT rejected]',error.message); }
});

let networkProbeRunning=false;
async function refreshNetwork() {
  if(networkProbeRunning) return;
  networkProbeRunning=true;
  try {
    const telemetryRx60s=db.prepare('SELECT COUNT(*) n FROM readings WHERE rx_ts>=?').get(Date.now()-60000).n;
    const sample=await collectNetworkSample({mqttConnected:mqttClient.connected,telemetryRx60s});
    const stored={...sample,run_label:currentRunLabel,
      mqtt_connected:sample.mqtt_connected?1:0,
      optional_requests_allowed:sample.optional_requests_allowed===null?null:(sample.optional_requests_allowed?1:0)};
    stored.id=Number(insertNetwork.run(stored).lastInsertRowid);
    latestNetwork=networkBooleans(stored);
    io.emit('network',latestNetwork);
    io.emit('gateway',snapshot());
  } catch(error) { console.error('[NETWORK]',error.message); }
  finally { networkProbeRunning=false; }
}
void refreshNetwork();
const networkTimer=setInterval(refreshNetwork,15000);

const nodeGuard = (req,res,next)=>validNodeId(req.params.nodeId) ? next() : res.status(400).json({error:'Invalid node ID'});
const send = (topic,payload)=>new Promise((resolve,reject)=>{
  if (!mqttClient.connected) return reject(Error('Broker disconnected; command not queued'));
  mqttClient.publish(topic,payload,{qos:0,retain:false},error=>error?reject(error):resolve());
});
const fail = (res,error)=>res.status(503).json({error:error.message});
app.get('/api/health',(_,res)=>res.json({ok:true,mqtt_connected:mqttClient.connected,topic_root:TOPIC_ROOT,network:latestNetwork}));
app.get('/api/gateway',(_,res)=>res.json(snapshot()));
app.get('/api/network',(req,res)=>{
  const limit=Math.max(1,Math.min(1000,Number.parseInt(req.query.limit)||240));
  const where=[],args=[];
  if(req.query.run_label!==undefined){where.push('run_label=?');args.push(String(req.query.run_label));}
  const rows=db.prepare(`SELECT * FROM network_samples ${where.length?'WHERE '+where.join(' AND '):''} ORDER BY id DESC LIMIT ?`).all(...args,limit);
  res.json(rows.reverse().map(networkBooleans));
});
app.get('/api/readings',(req,res)=>{
  const limit = Math.max(1,Math.min(5000,Number.parseInt(req.query.limit)||200));
  const where=[], args=[];
  if (req.query.node_id) { if(!validNodeId(req.query.node_id)) return res.status(400).json({error:'Invalid node'}); where.push('node_id=?'); args.push(req.query.node_id); }
  if (req.query.run_label!==undefined) { where.push('run_label=?'); args.push(String(req.query.run_label)); }
  const rows=db.prepare(`SELECT * FROM readings ${where.length?'WHERE '+where.join(' AND '):''} ORDER BY id DESC LIMIT ?`).all(...args,limit);
  res.json(rows.reverse());
});
app.get('/api/stats',(req,res)=>{
  const where=[], args=[];
  if (req.query.node_id) { if(!validNodeId(req.query.node_id)) return res.status(400).json({error:'Invalid node'}); where.push('node_id=?'); args.push(req.query.node_id); }
  if(req.query.run_label!==undefined) {where.push('run_label=?');args.push(String(req.query.run_label));}
  const counts=db.prepare(`SELECT mode,COUNT(*) n FROM readings ${where.length?'WHERE '+where.join(' AND '):''} GROUP BY mode`).all(...args);
  const energyWhere=[...where,'energy_mwh IS NOT NULL'];
  const energyRows=db.prepare(`SELECT mode,COALESCE(power_session,'legacy-unknown') power_session,
    COUNT(energy_mwh) n_power,MIN(rx_ts) first_rx,MAX(rx_ts) last_rx,
    MIN(energy_mwh) first_energy_mwh,MAX(energy_mwh) last_energy_mwh
    FROM readings WHERE ${energyWhere.join(' AND ')} GROUP BY mode,COALESCE(power_session,'legacy-unknown')`).all(...args);
  const sessions=energyRows.map(r=>({mode:r.mode,power_session:r.power_session,
    samples:r.n_power,duration_s:r.first_rx===null||r.last_rx===null?null:(r.last_rx-r.first_rx)/1000,
    delta_mwh:r.n_power>=2&&r.last_energy_mwh>=r.first_energy_mwh?r.last_energy_mwh-r.first_energy_mwh:null,
    first_mwh:r.first_energy_mwh,last_mwh:r.last_energy_mwh,
  }));
  const energy_by_mode={};
  const mixedModes=counts.filter(r=>r.n>0).length>1;
  for(const mode of ['BASELINE','ADAPTIVE']) {
    const modeSessions=sessions.filter(s=>s.mode===mode);
    const only=modeSessions.length===1?modeSessions[0]:null;
    const qualified=!mixedModes&&!!only&&only.power_session!=='legacy-unknown'&&only.samples>=2&&only.delta_mwh!==null;
    const issue=mixedModes?'Run filter contains both modes; use one mode per run label' :
      modeSessions.length===0?'No INA219 samples for this filter' :
      modeSessions.length>1?'Run filter contains more than one boot session' :
      only.power_session==='legacy-unknown'?'Legacy firmware did not identify the boot session' :
      only.samples<2?'At least two readings are required' :
      only.delta_mwh===null?'Energy counter is not monotonic within this session' : null;
    energy_by_mode[mode]={qualified,delta_mwh:qualified?only.delta_mwh:null,
      raw_delta_mwh:only?.delta_mwh??null,samples:only?.samples??0,
      duration_s:only?.duration_s??null,session_count:modeSessions.length,
      power_session:only?.power_session??null,issue,sessions:modeSessions};
  }
  res.json({tx_count_adaptive:counts.find(r=>r.mode==='ADAPTIVE')?.n||0,
    tx_count_baseline:counts.find(r=>r.mode==='BASELINE')?.n||0,
    energy_by_mode,
    scope:{node_id:req.query.node_id||null,run_label:req.query.run_label??null},
    interpretation:'Qualified energy deltas require one identified boot session per node, mode and run label. Compare only matched durations, hardware, supply and sensor scenarios.'});
});
app.post('/api/mode/:nodeId',nodeGuard,async(req,res)=>{
  if(!['BASELINE','ADAPTIVE'].includes(req.body?.mode)) return res.status(400).json({error:'Invalid mode'});
  try {await send(`${TOPIC_ROOT}/${req.params.nodeId}/mode`,req.body.mode);res.json({ok:true,status:'published-not-device-confirmed'});} catch(e){fail(res,e);}
});
const battery = async(req,res)=>{
  if(typeof req.body?.value!=='number'||!Number.isFinite(req.body.value)||req.body.value<0||req.body.value>100) return res.status(400).json({error:'Battery must be a number from 0 to 100'});
  try {await send(`${TOPIC_ROOT}/${req.params.nodeId||'bin01'}/battery`,String(req.body.value));res.json({ok:true,status:'published-not-device-confirmed'});}catch(e){fail(res,e);}
};
app.post('/api/battery/:nodeId',nodeGuard,battery);
app.post('/api/battery',battery); // legacy tools remain compatible

async function issue(nodeId,body) {
  if(!validLease(body)) throw Error('Use interval 60–300 s and TTL 60–600 s, with TTL at least the interval');
  const node=snapshot().nodes.find(n=>n.node_id===nodeId);
  if(!node?.feedback_eligible) throw Error('Node is stale, protected, baseline, or lacks verified feedback support');
  const pending=db.prepare("SELECT * FROM gateway_commands WHERE node_id=? AND status IN ('pending','accepted','duplicate') ORDER BY issued_at DESC LIMIT 1").get(nodeId);
  if(pending && Date.now()<pending.issued_at+pending.ttl_s*1000+10000) throw Error('A request is already pending or within its lease; wait for expiry');
  if(!mqttClient.connected) throw Error('Broker disconnected');
  const request_id=randomUUID().replaceAll('-','').slice(0,16);
  const issued_at=Date.now();
  db.prepare('INSERT INTO gateway_commands(request_id,node_id,interval_s,ttl_s,issued_at,status) VALUES(?,?,?,?,?,?)')
    .run(request_id,nodeId,body.interval_s,body.ttl_s,issued_at,'pending');
  try {await send(`${TOPIC_ROOT}/${nodeId}/override`,JSON.stringify({request_id,interval_s:body.interval_s,ttl_s:body.ttl_s}));}
  catch(e){db.prepare("UPDATE gateway_commands SET status='publish-failed' WHERE request_id=?").run(request_id);throw e;}
  return {request_id,node_id:nodeId,issued_at,status:'published-awaiting-device-ack'};
}
app.post('/api/override/:nodeId',nodeGuard,async(req,res)=>{
  if(!validLease(req.body)) return res.status(400).json({error:'Invalid bounded lease'});
  try{res.json(await issue(req.params.nodeId,req.body));}catch(e){res.status(409).json({error:e.message});}
});
app.post('/api/gateway/dispatch',async(_,res)=>{
  const state=snapshot();
  if(!state.selected_node) return res.status(409).json({error:state.decision});
  try{res.json({...await issue(state.selected_node,{interval_s:60,ttl_s:180}),decision:state.decision});}
  catch(e){res.status(409).json({error:e.message});}
});
app.get('/api/run',(_,res)=>res.json({label:currentRunLabel}));
app.post('/api/run',(req,res)=>{
  if(typeof req.body?.label!=='string'||req.body.label.length>120) return res.status(400).json({error:'Label must be a string of at most 120 characters'});
  currentRunLabel=req.body.label;
  db.prepare("INSERT INTO settings(key,value) VALUES('run_label',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value").run(currentRunLabel);
  res.json({ok:true,label:currentRunLabel});
});
// Destructive reset is deliberately unavailable during research evidence collection.
app.post('/api/reset',(_,res)=>res.status(409).json({error:'Evidence preservation: start a new run label instead of deleting readings'}));
app.use('/api',(_,res)=>res.status(404).json({error:'Unknown API route'}));
// The built dashboard (npm run build in dashboard/), same origin as the API.
if (existsSync(join(DASHBOARD_DIST,'index.html'))) {
  app.use(express.static(DASHBOARD_DIST));
  app.get(/^\/(?!api\/|socket\.io\/).*/,(_,res)=>res.sendFile(join(DASHBOARD_DIST,'index.html')));
} else {
  console.warn(`[server] ${DASHBOARD_DIST} not built; run npm run build in dashboard/ to serve the UI here`);
}
const broadcast=setInterval(()=>io.emit('gateway',snapshot()),2000);
httpServer.listen(PORT,BIND_HOST,()=>console.log(`[server] http://${BIND_HOST}:${PORT}; MQTT=${TOPIC_ROOT}; dashboard ${existsSync(DASHBOARD_DIST)?'served at /':'not built'}; CORS ${DASHBOARD_ORIGIN.join(', ')}`));
for(const signal of ['SIGINT','SIGTERM']) process.on(signal,()=>{
  clearInterval(broadcast);clearInterval(networkTimer);mqttClient.end(true);io.close();httpServer.close();db.close();process.exit(0);
});
