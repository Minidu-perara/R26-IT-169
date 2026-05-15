import { useEffect, useState } from 'react'
import { io } from 'socket.io-client'
import {
  LineChart, Line, XAxis, YAxis, Tooltip, Legend, ResponsiveContainer, ReferenceLine
} from 'recharts'

const BACKEND = 'http://localhost:3001'
const socket  = io(BACKEND)

const STATE_COLOR = {
  ALERT:    'bg-red-600',
  ACTIVE:   'bg-orange-500',
  STABLE:   'bg-green-600',
  CONSERVE: 'bg-yellow-600',
}

const REASON_COLOR = {
  'urgency-fill':   'text-red-400',
  'urgency-gas135': 'text-red-400',
  'urgency-gas5':   'text-red-400',
  'urgency-rate':   'text-red-400',
  'timer':          'text-blue-400',
  'deadband':       'text-green-400',
  'baseline':       'text-gray-400',
}

// ── StatCard ──────────────────────────────────────────────────────────────
function StatCard({ label, value, unit = '' }) {
  return (
    <div className="bg-gray-800 rounded-xl p-4 flex flex-col gap-1">
      <span className="text-gray-400 text-xs uppercase tracking-wide">{label}</span>
      <span className="text-white text-2xl font-bold">
        {value}<span className="text-base font-normal text-gray-400 ml-1">{unit}</span>
      </span>
    </div>
  )
}

// ── TCR display ───────────────────────────────────────────────────────────
function TCRDisplay({ stats }) {
  const adaptive = stats.tx_count_adaptive
  const baseline = stats.tx_count_baseline
  const tcr      = baseline > 0 ? Math.round((1 - adaptive / baseline) * 100) : null
  const saved    = baseline > adaptive ? baseline - adaptive : 0
  const energy   = tcr !== null ? Math.round(tcr * 0.7) : null

  const tcrColor = tcr === null ? 'text-gray-500'
                 : tcr >= 60   ? 'text-green-400'
                 : tcr >= 30   ? 'text-yellow-400'
                 :                'text-red-400'

  return (
    <div className="bg-gray-800 rounded-xl p-5 mb-6">
      <div className="flex items-baseline gap-3 mb-3">
        <h2 className="text-lg font-semibold">Transmission Count Reduction</h2>
        <span className="text-gray-500 text-xs">target ≥ 60%</span>
      </div>
      <div className="flex flex-wrap gap-8 items-end">
        <div>
          <p className="text-gray-400 text-xs mb-1">TCR</p>
          <p className={`text-6xl font-black ${tcrColor}`}>
            {tcr !== null ? `${tcr}%` : '—'}
          </p>
        </div>
        <div className="flex flex-wrap gap-6">
          <div>
            <p className="text-gray-400 text-xs mb-1">ADAPTIVE TX</p>
            <p className="text-green-400 text-3xl font-bold">{adaptive}</p>
          </div>
          <div>
            <p className="text-gray-400 text-xs mb-1">BASELINE TX</p>
            <p className="text-red-400 text-3xl font-bold">{baseline}</p>
          </div>
          <div>
            <p className="text-gray-400 text-xs mb-1">TX Saved</p>
            <p className="text-yellow-300 text-3xl font-bold">{saved > 0 ? saved : '—'}</p>
          </div>
          <div>
            <p className="text-gray-400 text-xs mb-1">Est. Energy Saved</p>
            <p className="text-cyan-400 text-3xl font-bold">
              {energy !== null && energy > 0 ? `${energy}%` : '—'}
            </p>
          </div>
        </div>
      </div>
      <p className="text-gray-600 text-xs mt-3">
        Baseline = fixed 60 s interval | Adaptive = FSM-driven | Est. energy = TCR × 0.7
      </p>
    </div>
  )
}

// ── Battery slider ────────────────────────────────────────────────────────
function BatterySlider() {
  const [batt, setBatt] = useState(80)
  const [sent, setSent] = useState(false)

  function sendValue(value) {
    fetch(`${BACKEND}/api/battery`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ value }),
    })
    .then(() => { setSent(true); setTimeout(() => setSent(false), 2000) })
    .catch(() => {})
  }

  useEffect(() => { sendValue(80) }, [])

  function handleChange(e) {
    const value = Number(e.target.value)
    setBatt(value)
    sendValue(value)
  }

  const color = batt <= 5  ? 'text-red-500'
              : batt <= 30 ? 'text-yellow-400'
              :               'text-green-400'

  return (
    <div className="bg-gray-800 rounded-xl p-4 flex flex-col gap-2">
      <div className="flex items-center justify-between">
        <span className="text-gray-400 text-xs uppercase tracking-wide">Simulated Battery</span>
        <div className="flex items-center gap-3">
          {sent && <span className="text-green-400 text-xs animate-pulse">Sent to device</span>}
          <span className={`text-sm font-bold ${color}`}>{batt}%</span>
        </div>
      </div>
      <input
        type="range" min={0} max={100} value={batt}
        onChange={handleChange}
        className="w-full accent-yellow-400 cursor-pointer"
      />
      <div className="flex justify-between text-gray-500 text-xs">
        <span>0% — DEAD</span>
        <span>30% — CONSERVE</span>
        <span>100%</span>
      </div>
    </div>
  )
}

// ── Mode control ──────────────────────────────────────────────────────────
function ModeControl({ currentMode }) {
  function setMode(mode) {
    fetch(`${BACKEND}/api/mode/bin01`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mode }),
    }).catch(() => {})
  }

  function resetData() {
    fetch(`${BACKEND}/api/reset`, { method: 'POST' }).catch(() => {})
  }

  const base    = 'px-4 py-2 rounded-lg text-sm font-semibold transition-colors'
  const active  = (m) => currentMode === m

  return (
    <div className="bg-gray-800 rounded-xl p-4 flex flex-wrap items-center gap-3 mb-6">
      <span className="text-gray-400 text-xs uppercase tracking-wide">Mode Control</span>
      <button
        onClick={() => setMode('BASELINE')}
        className={`${base} ${active('BASELINE') ? 'bg-red-600 text-white' : 'bg-gray-700 text-gray-300 hover:bg-red-700 hover:text-white'}`}
      >
        Run BASELINE
      </button>
      <button
        onClick={() => setMode('ADAPTIVE')}
        className={`${base} ${active('ADAPTIVE') ? 'bg-green-600 text-white' : 'bg-gray-700 text-gray-300 hover:bg-green-700 hover:text-white'}`}
      >
        Run ADAPTIVE
      </button>
      <button
        onClick={resetData}
        className={`${base} bg-gray-700 text-gray-400 hover:bg-gray-600 hover:text-white`}
      >
        Reset Data
      </button>
      <span className="ml-auto text-xs text-gray-400">
        Active:{' '}
        <span className={`font-bold ${active('ADAPTIVE') ? 'text-green-400' : active('BASELINE') ? 'text-red-400' : 'text-gray-400'}`}>
          {currentMode ?? '—'}
        </span>
      </span>
    </div>
  )
}

// ── Risk / status panel ───────────────────────────────────────────────────
function RiskPanel() {
  return (
    <div className="grid grid-cols-1 sm:grid-cols-3 gap-4 mb-6">
      <div className="bg-gray-800 rounded-xl p-4">
        <p className="text-gray-400 text-xs uppercase tracking-wide mb-1">MQ Sensor Warmup</p>
        <p className="text-green-400 font-bold">Ready</p>
        <p className="text-gray-500 text-xs mt-1">2 min warmup on boot</p>
      </div>
      <div className="bg-gray-800 rounded-xl p-4">
        <p className="text-gray-400 text-xs uppercase tracking-wide mb-1">Gateway</p>
        <p className="text-blue-400 font-bold">Laptop (PP1 Proxy)</p>
        <p className="text-gray-500 text-xs mt-1">Mosquitto MQTT broker</p>
      </div>
      <div className="bg-gray-800 rounded-xl p-4">
        <p className="text-gray-400 text-xs uppercase tracking-wide mb-1">Edge Deployment</p>
        <p className="text-gray-400 font-bold">Raspberry Pi — PP2</p>
        <p className="text-gray-500 text-xs mt-1">Planned next milestone</p>
      </div>
    </div>
  )
}

// ── App ───────────────────────────────────────────────────────────────────
export default function App() {
  const [latest, setLatest]   = useState(null)
  const [history, setHistory] = useState([])
  const [stats, setStats]     = useState({ tx_count_adaptive: 0, tx_count_baseline: 0 })

  useEffect(() => {
    fetch(`${BACKEND}/api/readings?limit=120`)
      .then(r => r.json())
      .then(rows => setHistory(rows.map(toChartPoint)))
      .catch(() => {})

    fetch(`${BACKEND}/api/stats`)
      .then(r => r.json())
      .then(setStats)
      .catch(() => {})

    socket.on('reading', data => {
      setLatest(data)
      setHistory(prev => [...prev.slice(-119), toChartPoint(data)])
      setStats(prev => ({
        ...prev,
        [data.mode === 'ADAPTIVE' ? 'tx_count_adaptive' : 'tx_count_baseline']: data.tx_count,
      }))
    })
    socket.on('reset', () => {
      setHistory([])
      setStats({ tx_count_adaptive: 0, tx_count_baseline: 0 })
    })
    return () => { socket.off('reading'); socket.off('reset') }
  }, [])

  function toChartPoint(d) {
    return {
      t:       new Date(d.ts).toLocaleTimeString(),
      fill:    parseFloat(d.fill_pct),
      temp:    parseFloat(d.temp_c),
      batt:    parseFloat(d.batt_pct),
      gas_135: d.gas_135,
      gas_5:   d.gas_5,
      state:   d.state,
    }
  }

  const stateLabel  = latest?.state  ?? '—'
  const reasonLabel = latest?.reason ?? '—'
  const reasonColor = REASON_COLOR[reasonLabel] ?? 'text-gray-400'

  const tooltipStyle = { background: '#1f2937', border: 'none' }
  const xAxisProps   = { dataKey: 't', tick: { fill: '#9ca3af', fontSize: 10 }, interval: 'preserveStartEnd' }
  const yAxisProps   = { tick: { fill: '#9ca3af', fontSize: 10 } }

  return (
    <div className="min-h-screen bg-gray-900 text-white p-6 font-sans">
      <h1 className="text-2xl font-bold mb-1">Smart Bin — R26-IT-169</h1>
      <p className="text-gray-400 text-sm mb-6">
        Node: {latest?.node_id ?? 'waiting…'} &nbsp;|&nbsp;
        Mode: <span className="text-white">{latest?.mode ?? '—'}</span>
      </p>

      {/* TCR — top of page */}
      <TCRDisplay stats={stats} />

      {/* Mode control */}
      <ModeControl currentMode={latest?.mode} />

      {/* State badge + live reason */}
      <div className="flex flex-wrap items-center gap-4 mb-6">
        <span className={`px-4 py-1 rounded-full text-sm font-semibold ${STATE_COLOR[stateLabel] ?? 'bg-gray-600'}`}>
          {stateLabel}
        </span>
        {stateLabel === 'ALERT' && (
          <span className="text-red-400 font-bold animate-pulse">⚠ Bin full or gas detected</span>
        )}
        <div className="ml-auto flex flex-wrap gap-4 text-sm">
          <span className="text-gray-400">
            Last TX reason:{' '}
            <span className={`font-semibold ${reasonColor}`}>{reasonLabel}</span>
          </span>
          <span className="text-gray-400">
            dF/dt:{' '}
            <span className="text-white font-semibold">
              {latest ? parseFloat(latest.dfdt).toFixed(1) : '—'} %/hr
            </span>
          </span>
        </div>
      </div>

      {/* Battery slider */}
      <div className="mb-6">
        <BatterySlider />
      </div>

      {/* Stat cards */}
      <div className="grid grid-cols-2 sm:grid-cols-4 lg:grid-cols-7 gap-4 mb-8">
        <StatCard label="Fill"        value={latest ? parseFloat(latest.fill_pct).toFixed(1) : '—'} unit="%" />
        <StatCard label="Battery"     value={latest ? parseFloat(latest.batt_pct).toFixed(0)  : '—'} unit="%" />
        <StatCard label="Temp"        value={latest ? parseFloat(latest.temp_c).toFixed(1)    : '—'} unit="°C" />
        <StatCard label="Gas MQ135"   value={latest?.gas_135 ?? '—'} />
        <StatCard label="Gas MQ5"     value={latest?.gas_5   ?? '—'} />
        <StatCard label="dF/dt"       value={latest ? parseFloat(latest.dfdt).toFixed(1) : '—'} unit="%/hr" />
        <StatCard label="TX Interval" value={latest?.interval ?? '—'} unit="s" />
      </div>

      {/* Fill chart */}
      <div className="bg-gray-800 rounded-xl p-5 mb-6">
        <h2 className="text-lg font-semibold mb-4">Fill % over time</h2>
        <ResponsiveContainer width="100%" height={220}>
          <LineChart data={history}>
            <XAxis {...xAxisProps} />
            <YAxis {...yAxisProps} domain={[0, 100]} unit="%" />
            <Tooltip contentStyle={tooltipStyle} />
            <ReferenceLine y={80} stroke="#ef4444" strokeDasharray="4 2"
              label={{ value: 'ALERT 80%', fill: '#ef4444', fontSize: 10 }} />
            <ReferenceLine y={60} stroke="#f97316" strokeDasharray="4 2"
              label={{ value: 'ACTIVE 60%', fill: '#f97316', fontSize: 10 }} />
            <Line type="monotone" dataKey="fill" stroke="#60a5fa" dot={false} strokeWidth={2} name="Fill %" />
          </LineChart>
        </ResponsiveContainer>
      </div>

      {/* Temp + Battery chart */}
      <div className="bg-gray-800 rounded-xl p-5 mb-6">
        <h2 className="text-lg font-semibold mb-4">Battery & Temperature</h2>
        <ResponsiveContainer width="100%" height={180}>
          <LineChart data={history}>
            <XAxis {...xAxisProps} />
            <YAxis {...yAxisProps} />
            <Tooltip contentStyle={tooltipStyle} />
            <Legend />
            <Line type="monotone" dataKey="batt" stroke="#facc15" dot={false} strokeWidth={2} name="Battery %" />
            <Line type="monotone" dataKey="temp" stroke="#f87171" dot={false} strokeWidth={2} name="Temp °C" />
          </LineChart>
        </ResponsiveContainer>
      </div>

      {/* Gas chart */}
      <div className="bg-gray-800 rounded-xl p-5 mb-6">
        <h2 className="text-lg font-semibold mb-4">Gas Sensor Readings (ADC)</h2>
        <ResponsiveContainer width="100%" height={200}>
          <LineChart data={history}>
            <XAxis {...xAxisProps} />
            <YAxis {...yAxisProps} />
            <Tooltip contentStyle={tooltipStyle} />
            <Legend />
            <ReferenceLine y={2500} stroke="#ef4444" strokeDasharray="4 2"
              label={{ value: 'MQ135 alert', fill: '#ef4444', fontSize: 10 }} />
            <ReferenceLine y={3500} stroke="#f97316" strokeDasharray="4 2"
              label={{ value: 'MQ5 alert', fill: '#f97316', fontSize: 10 }} />
            <Line type="monotone" dataKey="gas_135" stroke="#a78bfa" dot={false} strokeWidth={2} name="MQ-135" />
            <Line type="monotone" dataKey="gas_5"   stroke="#34d399" dot={false} strokeWidth={2} name="MQ-5" />
          </LineChart>
        </ResponsiveContainer>
      </div>

      {/* Risk / status panel */}
      <RiskPanel />
    </div>
  )
}
