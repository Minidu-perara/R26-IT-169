import express from 'express';
import { createServer } from 'http';
import { Server as IO } from 'socket.io';
import mqtt from 'mqtt';
import Database from 'better-sqlite3';

const PORT = 3001;
const MQTT_URL = 'mqtt://localhost:1883';

// ── DB ────────────────────────────────────────────────────────────────────
const db = new Database('smart-bin.db');
db.exec(`
  CREATE TABLE IF NOT EXISTS readings (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id   TEXT,
    ts        INTEGER,
    fill_pct  REAL,
    temp_c    REAL,
    humidity  REAL,
    gas_135   INTEGER,
    gas_5     INTEGER,
    batt_pct  REAL,
    state     TEXT,
    mode      TEXT,
    tx_count  INTEGER,
    interval  INTEGER,
    deadband  REAL,
    dfdt      REAL,
    reason    TEXT
  )
`);
const insert = db.prepare(`
  INSERT INTO readings
    (node_id,ts,fill_pct,temp_c,humidity,gas_135,gas_5,batt_pct,state,mode,tx_count,interval,deadband,dfdt,reason)
  VALUES
    (@node_id,@ts,@fill_pct,@temp_c,@humidity,@gas_135,@gas_5,@batt_pct,@state,@mode,@tx_count,@interval,@deadband,@dfdt,@reason)
`);

// ── Express + Socket.io ───────────────────────────────────────────────────
const app = express();
const httpServer = createServer(app);
const io = new IO(httpServer, { cors: { origin: 'http://localhost:5173' } });

app.use(express.json());

app.get('/api/readings', (req, res) => {
  const limit = parseInt(req.query.limit) || 200;
  const rows = db.prepare('SELECT * FROM readings ORDER BY id DESC LIMIT ?').all(limit);
  res.json(rows.reverse());
});

app.get('/api/stats', (req, res) => {
  const adaptive = db.prepare(
    `SELECT MAX(tx_count) AS tx_count FROM readings WHERE mode = 'ADAPTIVE'`
  ).get();
  const baseline = db.prepare(
    `SELECT MAX(tx_count) AS tx_count FROM readings WHERE mode = 'BASELINE'`
  ).get();
  res.json({
    tx_count_adaptive: adaptive?.tx_count ?? 0,
    tx_count_baseline: baseline?.tx_count ?? 0,
  });
});

// ── MQTT ──────────────────────────────────────────────────────────────────
const mqttClient = mqtt.connect(MQTT_URL);

mqttClient.on('connect', () => {
  console.log('[MQTT] connected to broker');
  mqttClient.subscribe('bin/+/data');
});

mqttClient.on('message', (topic, payload) => {
  try {
    const e = JSON.parse(payload.toString());
    const row = {
      node_id:  e.node_id,
      ts:       e.ts,
      fill_pct: e.fill_pct,
      temp_c:   e.temp_c,
      humidity: e.humidity,
      gas_135:  e.gas_135,
      gas_5:    e.gas_5,
      batt_pct: e.batt_pct,
      state:    e.state,
      mode:     e.mode,
      tx_count: e.tx_count,
      interval: e.interval,
      deadband: e.deadband,
      dfdt:     e.dfdt,
      reason:   e.reason,
    };
    insert.run(row);
    io.emit('reading', row);
    console.log(`[RX] ${topic} fill=${row.fill_pct}% state=${row.state} reason=${row.reason}`);
  } catch (e) {
    console.error('[MQTT] parse error', e.message);
  }
});

// ── Mode / override relay to device ───────────────────────────────────────
app.post('/api/mode/:nodeId', (req, res) => {
  const { nodeId } = req.params;
  const { mode } = req.body;
  mqttClient.publish(`bin/${nodeId}/mode`, mode);
  res.json({ ok: true });
});

app.post('/api/battery', (req, res) => {
  const { value } = req.body;
  console.log('[API] battery set to', value);
  mqttClient.publish('bin/bin01/battery', String(value));
  res.json({ ok: true });
});

app.post('/api/reset', (req, res) => {
  db.prepare('DELETE FROM readings').run();
  io.emit('reset');
  console.log('[API] readings reset');
  res.json({ ok: true });
});

httpServer.listen(PORT, () => console.log(`[server] http://localhost:${PORT}`));
