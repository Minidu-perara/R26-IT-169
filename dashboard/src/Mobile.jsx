import {useEffect,useState} from 'react'
import {io} from 'socket.io-client'
import {BACKEND,api,REPORT_TYPES} from './backend'

// Phone view at /m: read-only bin status plus a problem report form.
// Deliberately no operator controls (mode, battery, calibration).

const PHYSICAL=['bin01','bin02']
const EVENT_TRIGGERS=['urgent-transition','state-transition','calibration']
const isEvent=row=>!!row && (row.state==='ALERT' || EVENT_TRIGGERS.includes(row.tx_trigger))
const STATE_HINT={STABLE:'Routine monitoring',ACTIVE:'Filling now',ALERT:'Needs attention',CONSERVE:'Saving power',DEAD:'Node shut down'}
const CAUSE_TEXT={'fill-alert':'Fill at or above 80%','fill-rate-alert':'Filling quickly','gas135-alert':'Gas level high',
  'gas5-alert':'Gas level high','fill-sensor-fault':'Fill sensor fault','low-battery':'Low battery (simulated)',
  'active-fill':'Filling','stable':'Stable'}
const finite=value=>value!==null && value!==undefined && Number.isFinite(Number(value))
// The event list can span days, so a row from before today also shows its date.
function clock(value) {
  if(!value) return ''
  const date=new Date(value)
  return date.toDateString()===new Date().toDateString() ?
    date.toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'}) :
    date.toLocaleString([], {month:'short',day:'numeric',hour:'2-digit',minute:'2-digit'})
}

function ago(seconds) {
  if(!finite(seconds)) return 'never'
  const s=Math.max(0,Math.round(seconds))
  if(s<60) return `${s} s ago`
  if(s<3600) return `${Math.floor(s/60)} min ago`
  if(s<86400) return `${Math.floor(s/3600)} h ${Math.floor(s%3600/60)} min ago`
  return `${Math.floor(s/86400)} d ago`
}

function eventTitle(row) {
  if(row.tx_trigger==='calibration') return 'Recalibrated empty'
  if(row.tx_trigger==='urgent-transition') return `Entered ${row.state}`
  if(row.tx_trigger==='state-transition') return `Changed to ${row.state}`
  return `${row.state} reading`
}

function BinCard({id,node,now,receivedAt}) {
  if(!node) return <article className="m-card tone-off">
    <div className="m-card-top"><h3>{id}</h3><span className="m-state">NOT CONNECTED</span></div>
    <p className="m-empty">No reading received from this bin yet.</p>
  </article>
  // age_s is relative to the server clock; advance it locally so a phone with a
  // wrong clock still shows the right age.
  const age=finite(node.age_s) ? node.age_s+(now-receivedAt)/1000 : null
  const fault=!!node.sensor_fault || node.status==='SENSOR_FAULT'
  const stale=node.status==='STALE'
  const fill=finite(node.fill_pct) ? Number(node.fill_pct) : null
  const tone=node.state==='ALERT' || node.status==='URGENT' ? 'alert' : fault || stale || node.state==='CONSERVE' || node.state==='DEAD' ? 'warn' : 'ok'
  return <article className={`m-card tone-${tone}`} aria-label={`${id}, ${node.state}`}>
    <div className="m-card-top"><h3>{id}</h3><span className="m-state">{node.state}</span></div>
    <p className="m-hint">{STATE_HINT[node.state]||'State reported by the bin'}</p>
    <div className="m-fill"><strong>{fill===null?'No reading':`${Math.round(fill)}%`}</strong>{fill!==null&&<span>full</span>}</div>
    <div className="m-bar" aria-hidden="true"><span style={{'--fill':`${Math.min(100,Math.max(0,fill??0))}%`}}/></div>
    <div className="m-meta">
      <span>Updated {ago(age)}</span>
      {stale&&<span className="m-flag is-warn">No recent update</span>}
      <span className={`m-flag ${fault?'is-warn':'is-ok'}`}>{fault?'Sensor fault':'Sensor OK'}</span>
    </div>
  </article>
}

function ReportForm({nodes}) {
  const [bin,setBin]=useState('')
  const [type,setType]=useState('overflow')
  const [note,setNote]=useState('')
  const [busy,setBusy]=useState(false)
  const [result,setResult]=useState(null)
  // Only bins that have sent data are valid report targets. Sorted by name, not
  // by the gateway's priority order, so the default choice does not move.
  const ids=nodes.map(n=>n.node_id).sort()
  const target=ids.includes(bin) ? bin : ids[0]||''
  async function submit(event) {
    event.preventDefault()
    if(!target||busy) return
    setBusy(true);setResult(null)
    try {
      await api('/api/reports',{node_id:target,type,...(note.trim()?{note:note.trim()}:{})})
      setResult({tone:'ok',text:`Report sent for ${target}. Thank you.`});setNote('')
    } catch(e) {
      setResult({tone:'error',text:e.status===429?'Too many reports in the last minute. Please try again shortly.':`Not sent: ${e.message}`})
    } finally {setBusy(false)}
  }
  return <form className="m-form" onSubmit={submit}>
    <label htmlFor="m-report-bin">Bin</label>
    <select id="m-report-bin" value={target} disabled={!ids.length} onChange={e=>setBin(e.target.value)}>
      {ids.length?ids.map(id=><option key={id} value={id}>{id}</option>):<option value="">No bins reporting yet</option>}
    </select>
    <fieldset className="m-types"><legend>Problem</legend>
      {REPORT_TYPES.map(t=><label key={t.value} className={type===t.value?'is-active':''}>
        <input type="radio" name="m-report-type" value={t.value} checked={type===t.value} onChange={()=>setType(t.value)}/>{t.label}
      </label>)}
    </fieldset>
    <label htmlFor="m-report-note">Note <small>(optional, {note.length}/280)</small></label>
    <textarea id="m-report-note" rows={3} maxLength={280} value={note} placeholder="What did you see?" onChange={e=>setNote(e.target.value)}/>
    <button className="m-submit" type="submit" disabled={busy||!target}>{busy?'Sending':'Send report'}</button>
    {result&&<p role="status" className={`m-result is-${result.tone}`}>{result.text}</p>}
  </form>
}

export default function Mobile() {
  const [gateway,setGateway]=useState(null),[receivedAt,setReceivedAt]=useState(Date.now())
  const [events,setEvents]=useState([]),[connected,setConnected]=useState(false)
  const [error,setError]=useState(''),[now,setNow]=useState(Date.now())

  useEffect(()=>{
    document.title='Smart Bin'
    let alive=true
    const take=v=>{if(alive){setGateway(v);setReceivedAt(Date.now());setError('')}}
    const refresh=()=>api('/api/gateway').then(take).catch(e=>{if(alive)setError(e.message)})
    // No event endpoint: scan recent receipts for alerts and state changes.
    const loadEvents=()=>api('/api/readings?limit=500').then(rows=>{if(alive)setEvents(rows.filter(isEvent).slice(-20).reverse())}).catch(e=>{if(alive)setError(e.message)})
    refresh();loadEvents()
    const socket=io(BACKEND)
    socket.on('connect',()=>{setConnected(true);refresh();loadEvents()})
    socket.on('disconnect',()=>setConnected(false))
    socket.on('gateway',take)
    socket.on('reading',row=>{if(alive&&isEvent(row))setEvents(list=>[row,...list.filter(r=>r.id!==row.id)].slice(0,20))})
    // Poll only while the socket is down; the backend pushes 'gateway' every 2 s.
    const poll=setInterval(()=>{if(!socket.connected)refresh()},5000)
    const tick=setInterval(()=>setNow(Date.now()),1000)
    return()=>{alive=false;clearInterval(poll);clearInterval(tick);socket.disconnect()}
  },[])

  const nodes=gateway?.nodes||[]
  const ids=[...new Set([...PHYSICAL,...nodes.map(n=>n.node_id)])]

  return <main className="m-shell">
    <div className="m-frame">
      <header className="m-header">
        <div><span className="eyebrow">R26-IT-169</span><h1>Smart Bin</h1><small>Live bin levels</small></div>
        <span className={`m-live ${connected?'is-on':''}`} role="status"><i/>{connected?'Live':'Offline'}</span>
      </header>
      {error&&<div role="alert" className="m-error">Cannot reach the bin server: {error}</div>}

      <section className="m-section" aria-labelledby="m-bins"><h2 id="m-bins">Bins</h2>
        <div className="m-cards">{ids.map(id=><BinCard key={id} id={id} node={nodes.find(n=>n.node_id===id)} now={now} receivedAt={receivedAt}/>)}</div>
      </section>

      <section className="m-section" aria-labelledby="m-events"><h2 id="m-events">Alerts and state changes</h2>
        {events.length?<ul className="m-events">{events.map((row,index)=><li key={row.id??index}>
          <time>{clock(row.rx_ts)}</time>
          <b>{row.node_id} · {eventTitle(row)}</b>
          <span className="m-event-fill">{finite(row.fill_pct)?`${Math.round(row.fill_pct)}%`:''}</span>
          <small>{row.tx_trigger==='calibration'?'':CAUSE_TEXT[row.state_cause]||row.state_cause||''}</small>
        </li>)}</ul>:<p className="m-empty">No alerts or state changes in recent readings.</p>}
      </section>

      <section className="m-section" aria-labelledby="m-report"><h2 id="m-report">Report a problem</h2>
        <ReportForm nodes={nodes}/>
      </section>
    </div>
  </main>
}
