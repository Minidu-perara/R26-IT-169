import {useEffect, useMemo, useState} from 'react'
import {io} from 'socket.io-client'
import {LineChart,Line,XAxis,YAxis,Tooltip,ResponsiveContainer,ReferenceLine,Legend} from 'recharts'

// Served by the backend (http://192.168.1.3:3002): same origin. On the Vite dev
// server (port 5173) the backend is on the same host at :3002.
const BACKEND = import.meta.env.VITE_BACKEND_URL ||
  (window.location.port === '5173' ? `${window.location.protocol}//${window.location.hostname}:3002` : window.location.origin)

const number = (value,digits=1) => value === null || value === undefined || !Number.isFinite(Number(value)) ? '—' : Number(value).toFixed(digits)
const time = value => value ? new Date(value).toLocaleTimeString([], {hour:'2-digit',minute:'2-digit',second:'2-digit'}) : '—'
const clamp = value => Math.min(100,Math.max(0,Number(value)||0))
const signalPercent = value => value === null || value === undefined ? 0 : Math.min(100,Math.max(3,(Number(value)/4095)*100))

async function api(path,body) {
  const response = await fetch(BACKEND+path,body===undefined ? {} : {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
  const value = await response.json()
  if(!response.ok) throw Error(value.error || `HTTP ${response.status}`)
  return value
}

function StatusLight({on,label}) {
  return <span className={`status-light ${on?'is-on':'is-off'}`}><span className="status-dot"/>{label}</span>
}

function BinVisual({node,id,compact=false}) {
  const fill=node?.fill_pct
  const level=clamp(fill)
  const isLive=node && ['CURRENT','URGENT'].includes(node.status)
  const tone=node?.status==='URGENT' || node?.state==='ALERT' ? 'danger' : node?.status==='STALE' ? 'warn' : 'safe'
  return <div className={`bin-stage ${compact?'is-compact':''}`} aria-label={`${id}, ${number(fill)} percent full`}>
    <div className={`node-beacon ${isLive?'is-live':''}`}><span/><span/><span/></div>
    <div className="bin-shadow"/>
    <div className="bin-lid"><span className="lid-handle"/></div>
    <div className={`bin-shell tone-${tone}`}>
      <div className="bin-inner">
        <div className="bin-fill" style={{'--fill-level':`${level}%`}}>
          <div className="fill-surface"/>
          <div className="waste-fragments" aria-hidden="true">
            <i/><i/><i/><i/><i/><i/><i/><i/>
          </div>
        </div>
        <div className="level-grid" aria-hidden="true"><span>80</span><span>60</span><span>40</span><span>20</span></div>
      </div>
      <div className="bin-rib rib-one"/><div className="bin-rib rib-two"/>
      <div className="bin-badge"><span>{id}</span><strong>{number(fill,0)}%</strong></div>
    </div>
    <div className="bin-wheels"><i/><i/></div>
  </div>
}

function NodeCard({id,node,selected,onSelect}) {
  const missing=!node
  const live=node && ['CURRENT','URGENT'].includes(node.status)
  return <button className={`node-card ${selected?'is-selected':''} ${missing?'is-missing':''}`} onClick={onSelect} aria-pressed={selected}>
    <div className="node-card-copy">
      <div className="eyebrow-row"><span className="eyebrow">PHYSICAL SENSOR NODE</span><span className={`node-status status-${(node?.status||'offline').toLowerCase()}`}>{node?.status||'NOT CONNECTED'}</span></div>
      <h2>{id}</h2>
      <p className="node-role">{id==='bin01'?'ESP32 · fill + climate + MQ-135 + power':'ESP-12E · fill + climate + power'}</p>
      {node ? <div className="node-card-facts">
        <span><b>{node.state}</b> state</span><span><b>{number(node.age_s,0)}s</b> since receipt</span><span><b>{node.mode}</b> policy</span>
      </div> : <p className="empty-copy">Waiting for real telemetry. This is not simulated data.</p>}
      <div className="node-card-footer"><span className={`live-mark ${live?'is-live':''}`}/>{live?'Live telemetry':'Historical / offline'}<span className="select-prompt">{selected?'Inspecting node':'Inspect →'}</span></div>
    </div>
    <BinVisual node={node} id={id} compact/>
  </button>
}

function Instrument({kind,label,value,unit,percent,detail,digits=1}) {
  const available=value!==null && value!==undefined && Number.isFinite(Number(value))
  return <article className={`instrument instrument-${kind} ${available?'':'is-unavailable'}`}>
    <div className="instrument-top"><span className="instrument-label">{label}</span><span className="instrument-value">{number(value,digits)}<small>{available?unit:''}</small></span></div>
    <div className="instrument-track"><span style={{'--signal':`${available?clamp(percent):0}%`}}/></div>
    <p>{available?detail:'Sensor not installed or value unavailable'}</p>
  </article>
}

function SensorPanel({node}) {
  const temperature=node?.temp_c
  const humidity=node?.humidity
  const powerTone=node?.power_status==='MEASURED'?'ready':node?.power_status==='UNINSTRUMENTED'?'missing':'check'
  const powerTitle=node?.power_status==='MEASURED'?'POWER PATH MEASURED' :
    node?.power_status==='SESSION_UNQUALIFIED'?'MEASUREMENT NEEDS NEW FIRMWARE SESSION' :
    node?.power_status==='CHECK_POWER_PATH'?'CHECK INA219 POWER PATH' : 'POWER MEASUREMENT UNAVAILABLE'
  return <section className="sensor-section panel">
    <div className="section-heading"><div><span className="eyebrow">LIVE SENSOR ARRAY</span><h2>What the bin can sense</h2></div><span className="section-note">Raw readings · no fabricated values</span></div>
    <div className={`measurement-banner is-${powerTone}`}><b>{powerTitle}</b><span>{node?.power_reason||'Waiting for telemetry from the selected node.'}</span></div>
    <div className="instrument-grid">
      <Instrument kind="temperature" label="Temperature" value={temperature} unit="°C" percent={temperature==null?0:((temperature-10)/35)*100} detail="DHT22 ambient temperature"/>
      <Instrument kind="humidity" label="Humidity" value={humidity} unit="%" percent={humidity} detail="DHT22 relative humidity"/>
      <Instrument kind="gas" label="MQ-135" value={node?.gas_135} unit=" ADC" percent={signalPercent(node?.gas_135)} digits={0} detail="Raw exploratory channel · not ppm"/>
      <Instrument kind="voltage" label="Supply voltage" value={node?.bus_voltage_v} unit=" V" percent={(node?.bus_voltage_v/6)*100} digits={3} detail="INA219 measured node supply"/>
      <Instrument kind="current" label="Current" value={node?.current_ma} unit=" mA" percent={(node?.current_ma/500)*100} detail="INA219 high-side measurement"/>
      <Instrument kind="power" label="Power" value={node?.power_mw} unit=" mW" percent={(node?.power_mw/2500)*100} detail="Instantaneous measured power"/>
      <Instrument kind="energy" label="Energy since boot" value={node?.energy_mwh} unit=" mWh" percent={(node?.energy_mwh/100)*100} digits={3} detail="10 Hz node-integrated INA219 estimate"/>
    </div>
  </section>
}

function ResearchFlow({node,gateway}) {
  const interval=node?.policy_interval??node?.interval
  const steps=[
    {tag:'01',title:'Sense locally',copy:'Fill, climate and measured power'},
    {tag:'02',title:'Decide on device',copy:`${node?.state||'—'} · ${node?.state_cause||'waiting for classified cause'}`},
    {tag:'03',title:'Transmit selectively',copy:`${number(interval,0)} s policy interval`},
    {tag:'04',title:'Prioritise at edge',copy:`${gateway?.network?.status||'—'} network · freshness + urgency`},
  ]
  return <section className="research-flow panel">
    <div className="section-heading"><div><span className="eyebrow">THE RESEARCH FUNCTION</span><h2>Useful information, fewer unnecessary transmissions</h2></div><span className="research-question">Can the node report less while preserving urgent awareness?</span></div>
    <div className="flow-track">
      {steps.map((step,index)=><div className="flow-segment" key={step.tag}>
        <div className="flow-node"><span>{step.tag}</span><b>{step.title}</b><small>{step.copy}</small></div>
        {index<steps.length-1 && <div className="flow-link"><i/><i/><i/></div>}
      </div>)}
    </div>
    <div className="feedback-loop"><span>BOUNDED FEEDBACK</span><i/><p>Laptop may request closer monitoring for a limited time; the node keeps urgency and energy guards.</p></div>
  </section>
}

function NetworkPanel({gateway,history,node}) {
  const network=gateway?.network
  const status=network?.status||'UNKNOWN'
  const healthy=['GOOD','FAIR'].includes(status)
  const networkAction=network?.optional_requests_allowed===true?'OPTIONAL FEEDBACK ALLOWED':
    network?.optional_requests_allowed===false?'WITHHOLD OPTIONAL FEEDBACK':'MONITOR INITIALISING'
  const plot=history.map(row=>({...row,t:time(row.ts)}))
  return <section className="network-panel panel">
    <div className="section-heading"><div><span className="eyebrow">GATEWAY NETWORK INTELLIGENCE</span><h2>Local link health shapes optional traffic</h2></div><span className={`network-state is-${status.toLowerCase()}`}>{status}</span></div>
    <div className="network-layout">
      <div className="network-score" style={{'--network-score':`${clamp(network?.score)}%`}}><div><strong>{number(network?.score,0)}</strong><small>/ 100</small><span>rule score</span></div></div>
      <div className="network-metrics">
        <div><span>Gateway Wi-Fi</span><strong>{number(network?.signal_pct,0)} <small>%</small></strong><em>{network?.rssi_dbm==null?'RSSI unavailable':`${number(network.rssi_dbm,0)} dBm`}</em></div>
        <div><span>Router latency</span><strong>{number(network?.latency_ms,1)} <small>ms</small></strong><em>{network?.gateway_ip||'No route detected'}</em></div>
        <div><span>Packet loss</span><strong>{number(network?.packet_loss_pct,1)} <small>%</small></strong><em>3 local ICMP probes</em></div>
        <div><span>MQTT transport</span><strong>{gateway?.mqtt_connected?'LINKED':'OFFLINE'}</strong><em>Local broker TCP 1883</em></div>
        <div><span>Telemetry received</span><strong>{network?.telemetry_rx_60s??'—'}</strong><em>Database receipts in 60 s</em></div>
        <div><span>{node?.node_id||'Selected node'} activity</span><strong>{node?.delivery_observation?.receipts_5m??'—'}</strong><em>Receipts in the last 5 min</em></div>
      </div>
      <div className={`network-policy ${healthy?'is-allow':network?.optional_requests_allowed===false?'is-hold':'is-wait'}`}><span>NETWORK-AWARE GATEWAY ACTION</span><b>{networkAction}</b><p>{network?.policy_reason||'Waiting for the first gateway network probe.'}</p><small>Local ALERT decisions on each bin remain authoritative.</small></div>
    </div>
    <div className="network-meta"><span>SSID <b>{network?.ssid||'—'}</b></span><span>Gateway host <b>{network?.local_ip||'—'}</b></span><span>Adapter <b>{network?.interface_name||'—'}</b></span><span>Radio <b>{network?.radio_type||'—'}</b></span></div>
    <div className="chart-label"><b>Gateway network history</b><span>15-second samples stored in SQLite</span></div>
    <div className="chart-wrap network-chart"><ResponsiveContainer width="100%" height="100%"><LineChart data={plot} margin={{top:8,right:5,left:-8,bottom:0}}>
      <XAxis dataKey="t" tick={{fill:'#718096',fontSize:10}}/>
      <YAxis yAxisId="quality" domain={[0,100]} tick={{fill:'#718096',fontSize:10}} unit="%"/>
      <YAxis yAxisId="latency" orientation="right" tick={{fill:'#718096',fontSize:10}} unit=" ms"/>
      <Tooltip contentStyle={{background:'#111820',border:'1px solid #34414d',borderRadius:8}}/><Legend/>
      <Line yAxisId="quality" dataKey="score" name="Network score" stroke="#70f0c4" strokeWidth={2.2} dot={false} connectNulls={false} isAnimationActive={false}/>
      <Line yAxisId="quality" dataKey="signal_pct" name="Gateway Wi-Fi %" stroke="#64b5f6" strokeWidth={1.7} dot={false} connectNulls={false} isAnimationActive={false}/>
      <Line yAxisId="quality" dataKey="packet_loss_pct" name="Packet loss %" stroke="#ff735d" strokeWidth={1.5} dot={false} connectNulls={false} isAnimationActive={false}/>
      <Line yAxisId="latency" dataKey="latency_ms" name="Router latency ms" stroke="#f0b35a" strokeWidth={1.7} dot={false} connectNulls={false} isAnimationActive={false}/>
    </LineChart></ResponsiveContainer></div>
    <p className="network-boundary"><b>Measurement boundary:</b> these are laptop edge-gateway link measurements plus MQTT/database receipt activity. They do not measure the bins’ individual Wi-Fi RSSI. A Raspberry Pi can run the same gateway monitor; node RSSI would require an added firmware field.</p>
  </section>
}

function PolicyPanel({node,gateway,onDispatch,busy}) {
  const interval=node?.policy_interval??node?.interval
  return <section className="policy-panel panel">
    <div className="section-heading"><div><span className="eyebrow">EXPLAIN THIS TRANSMISSION</span><h2>{node?.node_id||'Selected node'} decision trace</h2></div><span className={`state-pill state-${(node?.state||'waiting').toLowerCase()}`}>{node?.state||'WAITING'}</span></div>
    <div className="policy-layout">
      <div className="decision-orbit">
        <div className="orbit-ring orbit-a"/><div className="orbit-ring orbit-b"/>
        <div className="decision-core"><small>NEXT REPORT</small><strong>{number(interval,0)}<em>s</em></strong><span>{node?.mode||'—'}</span></div>
      </div>
      <div className="decision-data">
        <div><span>Transmission trigger</span><strong>{node?.tx_trigger||node?.reason||'Waiting for telemetry'}</strong></div>
        <div><span>State cause</span><strong>{node?.state_cause||'Legacy telemetry'}</strong></div>
        <div><span>Fill velocity</span><strong>{number(node?.dfdt)} <small>%/hr</small></strong></div>
        <div><span>Dynamic deadband</span><strong>{number(node?.deadband)} <small>%</small></strong></div>
        <div><span>Policy battery input</span><strong>{number(node?.batt_pct,0)} <small>% simulated</small></strong></div>
      </div>
      <div className="gateway-decision">
        <span className="eyebrow">LAPTOP EDGE GATEWAY</span>
        <p>{gateway?.decision||'Waiting for gateway state…'}</p>
        <button disabled={busy||!gateway?.selected_node||!gateway?.mqtt_connected} onClick={onDispatch}>Request closer monitoring<span>60 s reports · 180 s lease</span></button>
        <small>Explicit, addressed and self-expiring. Disabled during node-policy experiments.</small>
      </div>
    </div>
  </section>
}

function Controls({node,onError}) {
  const [battery,setBattery]=useState(node?.batt_pct ?? 80)
  const [notice,setNotice]=useState('')
  const [busy,setBusy]=useState(false)
  async function send(path,body) {
    setBusy(true);setNotice('')
    try {await api(path,body);setNotice('Command published. Confirm it in a later device reading.')} catch(e) {onError(e.message)} finally {setBusy(false)}
  }
  if(!node) return <p className="empty-copy">Controls appear after this physical node sends telemetry.</p>
  return <section className="experiment-controls panel">
    <div className="section-heading"><div><span className="eyebrow">CONTROLLED INPUTS</span><h2>Experiment controls</h2></div><span className="section-note">Commands are node-addressed</span></div>
    <div className="control-grid">
      <div className="control-block"><span>Reporting mode</span><div className="segmented">{['ADAPTIVE','BASELINE'].map(mode=><button key={mode} className={node.mode===mode?'is-active':''} disabled={busy} onClick={()=>send(`/api/mode/${node.node_id}`,{mode})}>{mode}</button>)}</div><small>Last reported: <b>{node.mode}</b></small></div>
      <div className="control-block battery-control"><label htmlFor={`battery-${node.node_id}`}>Simulated battery input <b>{battery}%</b></label><input id={`battery-${node.node_id}`} type="range" min="0" max="100" value={battery} onChange={e=>setBattery(Number(e.target.value))}/><button disabled={busy} onClick={()=>send(`/api/battery/${node.node_id}`,{value:battery})}>Apply to {node.node_id}</button><small>Software test input; this is not measured battery charge.</small></div>
    </div>
    {notice&&<p role="status" className="inline-notice">{notice}</p>}
  </section>
}

function EvidenceLab({selected,rows,stats,run,setRun,filter,setFilter,onSetRun,gateway}) {
  const history=useMemo(()=>rows.map(r=>({...r,t:time(r.rx_ts)})),[rows])
  const powerHistory=useMemo(()=>history.filter(r=>r.power_mw!==null&&r.power_mw!==undefined&&r.energy_mwh!==null&&r.energy_mwh!==undefined),[history])
  const baselineEnergy=stats.energy_by_mode?.BASELINE
  const adaptiveEnergy=stats.energy_by_mode?.ADAPTIVE
  const energyCard=(label,info)=><div className={info?.qualified?'is-qualified':'is-unqualified'}><span>{label} energy</span><strong>{info?.qualified?`${number(info.delta_mwh,3)} mWh`:'NOT READY'}</strong><small>{info?.qualified?`${number(info.duration_s,0)} s · session ${info.power_session}`:`${info?.raw_delta_mwh==null?'No valid delta':number(info.raw_delta_mwh,3)+' mWh observed'} · ${info?.issue||'Select one uninterrupted run'}`}</small></div>
  return <section className="evidence-lab panel">
    <div className="section-heading"><div><span className="eyebrow">EVIDENCE LAB</span><h2>Received-message record · {selected}</h2></div><span className="section-note">Database receipts, separated by node and run</span></div>
    <div className="evidence-toolbar">
      <label>New-reading run label<input value={run} maxLength={120} onChange={e=>setRun(e.target.value)}/></label><button onClick={onSetRun}>Set run label</button>
      <label>Evidence filter<input placeholder="All labels" value={filter} onChange={e=>setFilter(e.target.value)}/></label>
    </div>
    <div className="evidence-stats">
      <div><span>BASELINE receipts</span><strong>{stats.tx_count_baseline??'—'}</strong></div><div><span>ADAPTIVE receipts</span><strong>{stats.tx_count_adaptive??'—'}</strong></div>
      {energyCard('BASELINE',baselineEnergy)}
      {energyCard('ADAPTIVE',adaptiveEnergy)}
    </div>
    <p className="evidence-warning"><b>LIVE-DEMO POWER MODE:</b> INA219 integration runs at 10 Hz while Wi-Fi stays connected and deep sleep is disabled. This validates measurement; it does not yet prove an energy saving. Compare only one boot session per run with matched duration, board, supply and sensor scenario.</p>
    <div className="chart-label"><b>Fill and policy input</b><span>What the rule-based node reacts to</span></div>
    <div className="chart-wrap"><ResponsiveContainer width="100%" height="100%"><LineChart data={history} margin={{top:8,right:8,left:-18,bottom:0}}>
      <XAxis dataKey="t" tick={{fill:'#718096',fontSize:10}}/><YAxis domain={[0,100]} tick={{fill:'#718096',fontSize:10}}/>
      <Tooltip contentStyle={{background:'#111820',border:'1px solid #34414d',borderRadius:8}}/><Legend/>
      <ReferenceLine y={80} label={{value:'ALERT 80%',fill:'#ff735d',fontSize:10}} stroke="#ff735d" strokeDasharray="5 5"/>
      <Line dataKey="fill_pct" name="Fill %" stroke="#42d3a5" strokeWidth={2.4} dot={false} connectNulls={false} isAnimationActive={false}/>
      <Line dataKey="batt_pct" name="Simulated battery %" stroke="#f0b35a" strokeWidth={1.6} dot={false} connectNulls={false} isAnimationActive={false}/>
    </LineChart></ResponsiveContainer></div>
    <div className="chart-label"><b>Measured electrical trace</b><span>Instantaneous power and energy accumulated within the current firmware session</span></div>
    <div className="chart-wrap power-chart"><ResponsiveContainer width="100%" height="100%"><LineChart data={powerHistory} margin={{top:8,right:5,left:-8,bottom:0}}>
      <XAxis dataKey="t" tick={{fill:'#718096',fontSize:10}}/>
      <YAxis yAxisId="power" tick={{fill:'#718096',fontSize:10}} unit=" mW"/>
      <YAxis yAxisId="energy" orientation="right" tick={{fill:'#718096',fontSize:10}} unit=" mWh"/>
      <Tooltip contentStyle={{background:'#111820',border:'1px solid #34414d',borderRadius:8}}/><Legend/>
      <Line yAxisId="power" dataKey="power_mw" name="Measured power mW" stroke="#64b5f6" strokeWidth={2.1} dot={false} connectNulls={false} isAnimationActive={false}/>
      <Line yAxisId="energy" dataKey="energy_mwh" name="Energy this boot mWh" stroke="#70f0c4" strokeWidth={2.1} dot={false} connectNulls={false} isAnimationActive={false}/>
    </LineChart></ResponsiveContainer></div>
    <details className="request-log"><summary>Gateway request and acknowledgement log <span>{gateway?.commands.length||0} records</span></summary>
      <div className="table-scroll"><table><thead><tr><th>Time / node</th><th>Request</th><th>Device status</th><th>Effective interval</th></tr></thead><tbody>
        {gateway?.commands.map(c=><tr key={c.request_id}><td>{time(c.issued_at)} · {c.node_id}</td><td><code>{c.request_id}</code><small>{c.interval_s}s / {c.ttl_s}s lease</small></td><td>{c.status}{c.lease_elapsed&&!['expired','cancelled'].includes(c.status)&&<small>Lease elapsed; expiry not confirmed</small>}</td><td>{number(c.ack?.effective_s,0)} s</td></tr>)}
      </tbody></table>{!gateway?.commands.length&&<p className="empty-copy">No requests yet. Publishing alone is not proof that a device applied a command.</p>}</div>
    </details>
  </section>
}

export default function App() {
  const [gateway,setGateway]=useState(null),[selected,setSelected]=useState('bin01')
  const [rows,setRows]=useState([]),[stats,setStats]=useState({}),[run,setRun]=useState('')
  const [filter,setFilter]=useState(''),[error,setError]=useState(''),[notice,setNotice]=useState('')
  const [connected,setConnected]=useState(false),[busy,setBusy]=useState(false)
  const [networkRows,setNetworkRows]=useState([])

  useEffect(()=>{
    let alive=true
    const refresh=()=>api('/api/gateway').then(v=>{if(alive)setGateway(v)}).catch(e=>{if(alive)setError(e.message)})
    refresh()
    api('/api/run').then(v=>{if(alive){setRun(v.label);setFilter(v.label)}}).catch(e=>{if(alive)setError(e.message)})
    api('/api/network?limit=180').then(v=>{if(alive)setNetworkRows(v)}).catch(e=>{if(alive)setError(e.message)})
    const socket=io(BACKEND)
    socket.on('connect',()=>{setConnected(true);refresh()})
    socket.on('disconnect',()=>setConnected(false))
    socket.on('gateway',v=>{if(alive)setGateway(v)})
    socket.on('network',v=>{if(alive)setNetworkRows(rows=>[...rows.filter(row=>row.id!==v.id),v].slice(-180))})
    const timer=setInterval(refresh,5000)
    return()=>{alive=false;clearInterval(timer);socket.disconnect()}
  },[])

  const node=gateway?.nodes.find(n=>n.node_id===selected)
  const latestId=node?.id
  useEffect(()=>{
    let alive=true
    const query=new URLSearchParams({node_id:selected,...(filter?{run_label:filter}:{})}).toString()
    setRows([]);setStats({})
    Promise.all([api(`/api/readings?limit=150&${query}`),api(`/api/stats?${query}`)]).then(([r,s])=>{if(alive){setRows(r);setStats(s)}}).catch(e=>{if(alive)setError(e.message)})
    return()=>{alive=false}
  },[selected,filter,latestId])

  async function dispatch() {
    setBusy(true);setNotice('');setError('')
    try {const result=await api('/api/gateway/dispatch',{});setNotice(`Requested ${result.node_id}; request ${result.request_id}. Awaiting its device acknowledgement.`);setGateway(await api('/api/gateway'))} catch(e) {setError(e.message)} finally {setBusy(false)}
  }
  async function setRunLabel() {
    try {await api('/api/run',{label:run});setFilter(run);setGateway(await api('/api/gateway'));setNotice('New readings use this label; previous evidence remains preserved.')} catch(e) {setError(e.message)}
  }

  const ids=[...new Set(['bin01','bin02',...(gateway?.nodes.map(n=>n.node_id)||[])])]
  const physicalCount=gateway?.nodes.filter(n=>['CURRENT','URGENT'].includes(n.status)).length||0
  const isolated=BACKEND.includes(':3101')||(gateway?.topic_root&&gateway.topic_root!=='bin')

  return <main className="app-shell">
    <div className="ambient-grid" aria-hidden="true"/>
    <div className="app-frame">
      {isolated&&<div className="test-banner">ISOLATED TEST ENVIRONMENT · synthetic protocol data only</div>}
      <header className="app-header">
        <div><span className="project-kicker">R26-IT-169 · EDGE INTELLIGENCE FOR SMART WASTE</span><h1>Waste signals,<br/><em>made useful.</em></h1><p>Adaptive sensing at the bin. Local decisions at the edge. Evidence preserved for evaluation.</p></div>
        <div className="system-card"><span className="system-label">SYSTEM PULSE</span><StatusLight on={connected} label={connected?'Dashboard live':'Dashboard offline'}/><StatusLight on={gateway?.mqtt_connected} label={gateway?.mqtt_connected?'Local MQTT linked':'Broker offline'}/><StatusLight on={['GOOD','FAIR'].includes(gateway?.network?.status)} label={`Gateway network ${gateway?.network?.status||'unknown'}`}/><div className="system-count"><strong>{physicalCount}</strong><span>physical nodes<br/>reporting now</span></div><small>Latest interface update · {time(gateway?.at)}</small></div>
      </header>

      {error&&<div role="alert" className="alert-banner"><span>{error}</span><button onClick={()=>setError('')}>Dismiss</button></div>}
      {notice&&<div role="status" className="notice-banner">{notice}</div>}

      <section className="fleet-heading"><div><span className="eyebrow">LIVE BIN FLEET</span><h2>Physical fill at a glance</h2></div><p>Select a bin to trace its sensors and reporting decision.</p></section>
      <section aria-label="Bin overview" className="node-grid">
        {ids.map(id=><NodeCard key={id} id={id} node={gateway?.nodes.find(n=>n.node_id===id)} selected={selected===id} onSelect={()=>setSelected(id)}/>) }
      </section>

      <ResearchFlow node={node} gateway={gateway}/>
      <NetworkPanel gateway={gateway} history={networkRows} node={node}/>

      <section className="selected-node-layout">
        <div className="selected-bin panel"><div className="section-heading"><div><span className="eyebrow">SELECTED PHYSICAL NODE</span><h2>{selected}</h2></div><span className={`node-status status-${(node?.status||'offline').toLowerCase()}`}>{node?.status||'NOT CONNECTED'}</span></div><BinVisual node={node} id={selected}/><p className="selected-caption">{node?.priority_reason||'Waiting for a real sensor packet.'} {node&&`Stale after ${number(node.stale_after_s,0)} s without a receipt.`}</p></div>
        <SensorPanel node={node}/>
      </section>

      <PolicyPanel node={node} gateway={gateway} onDispatch={dispatch} busy={busy}/>
      <Controls key={selected} node={node} onError={setError}/>
      <EvidenceLab selected={selected} rows={rows} stats={stats} run={run} setRun={setRun} filter={filter} setFilter={setFilter} onSetRun={setRunLabel} gateway={gateway}/>

      <footer><div><b>Local prototype boundary</b><p>Trusted local network · no authentication or encrypted transport claimed.</p></div><div><b>Evidence boundary</b><p>Gateway acknowledgements are excluded from sensor-message counts. Missing sensors stay missing.</p></div><div><b>Research boundary</b><p>INA219 readings become comparative evidence only after matched repeated trials; no battery-life claim is made.</p></div></footer>
    </div>
  </main>
}
