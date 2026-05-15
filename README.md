# R26-IT-169 — Smart Bin Node (PP1)

> **Edge-based energy-efficient IoT smart waste management system**
> Solo project — Perera G.M.T (IT21196638)

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [System Architecture](#2-system-architecture)
3. [Repository Structure](#3-repository-structure)
4. [Hardware](#4-hardware)
5. [Firmware](#5-firmware)
   - [FSM Design](#51-fsm-design)
   - [AIMD Interval Adaptation](#52-aimd-interval-adaptation)
   - [Dynamic Deadband](#53-dynamic-deadband)
   - [Urgency Conditions](#54-urgency-conditions)
   - [BASELINE Mode](#55-baseline-mode)
   - [RTC Persistence & Fast Wi-Fi](#56-rtc-persistence--fast-wi-fi)
   - [Deep Sleep](#57-deep-sleep)
6. [MQTT Topics](#6-mqtt-topics)
7. [Backend](#7-backend)
8. [Dashboard](#8-dashboard)
9. [Mobile (Shell)](#9-mobile-shell)
10. [Running the Stack](#10-running-the-stack)
11. [Demo Procedure](#11-demo-procedure)
12. [Key Design Parameters](#12-key-design-parameters)
13. [PP2 Roadmap](#13-pp2-roadmap)

---

## 1. Project Overview

This system demonstrates **adaptive transmission interval control** for a waste-bin IoT node running on an ESP32. Rather than transmitting sensor readings at a fixed interval (the baseline), the node uses a rule-based **4-state Finite State Machine (FSM)** combined with an **AIMD (Additive Increase / Multiplicative Decrease)** algorithm to reduce unnecessary transmissions while guaranteeing that critical events (bin full, gas leak, rapid fill) are always reported within 60 seconds.

The primary metric is **Transmission Count Reduction (TCR)**:

```
TCR = (1 - TX_adaptive / TX_baseline) × 100%    target: ≥ 60%
```

---

## 2. System Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      ESP32 Node                         │
│  HC-SR04 ─┐                                             │
│  MQ-135  ─┤── readSensors() ── FSM ── publishReading()  │
│  MQ-5    ─┤                    │                        │
│  DHT22   ─┘              AIMD  │  deadband              │
└───────────────────────────┬────┘────────────────────────┘
                            │ MQTT  bin/bin01/data
                            ▼
                   ┌─────────────────┐
                   │  Mosquitto 1883  │  (laptop, PP1 proxy)
                   └────────┬────────┘
                            │
              ┌─────────────▼──────────────┐
              │   Node.js Backend :3001    │
              │  Express + Socket.io       │
              │  better-sqlite3            │
              └─────────────┬──────────────┘
                            │ WebSocket
              ┌─────────────▼──────────────┐
              │   React Dashboard :5173    │
              │  Recharts + Tailwind       │
              └────────────────────────────┘
```

---

## 3. Repository Structure

```
smart-bin-pp1/
├── firmware/                   ← PlatformIO / ESP32
│   ├── platformio.ini
│   ├── include/
│   │   └── config.h            ← All pins, thresholds, MQTT config
│   └── src/
│       ├── main.cpp            ← Setup, loop, publish, RTC restore
│       ├── fsm.h               ← 4-state FSM + AIMD logic
│       └── sensors.h           ← HC-SR04 median filter, SensorReading struct
├── backend/
│   ├── package.json
│   └── server.js               ← Express REST + Socket.io + MQTT + SQLite
├── dashboard/
│   ├── index.html
│   ├── vite.config.js
│   ├── tailwind.config.js
│   └── src/
│       ├── main.jsx
│       ├── index.css
│       └── App.jsx             ← Full dashboard UI
├── mobile/
│   ├── app.json
│   └── App.js                  ← Basic Expo shell (PP1 only)
└── README.md
```

---

## 4. Hardware

| Component | Model | ESP32 GPIO |
|---|---|---|
| Ultrasonic fill sensor | HC-SR04 | TRIG → **13**, ECHO → **14** |
| Air quality sensor 1 | MQ-135 | **34** (ADC1_CH6, input-only) |
| Air quality sensor 2 | MQ-5 | **35** (ADC1_CH7, input-only) |
| Temp / humidity sensor | DHT22 | **4** |
| Battery level | Simulated via MQTT | `bin/bin01/battery` topic |

> **Note — battery simulation:** The ESP32 `analogRead()` on ADC2 pins conflicts with Wi-Fi. Battery level for PP1 is injected over MQTT from the dashboard slider (`bin/bin01/battery`). The firmware stores it in `simulatedBattPct` and exposes it to the FSM exactly as a real ADC reading would.

> **HC-SR04 fill calculation:**
> ```
> fill% = (BIN_DEPTH_CM - measured_cm) / (BIN_DEPTH_CM - FILL_MAX_CM) × 100
> BIN_DEPTH_CM = 40 cm   (empty distance)
> FILL_MAX_CM  =  2 cm   (full distance)
> ```
> A **median-of-3** filter is applied to raw HC-SR04 readings before the fill percentage is computed.

---

## 5. Firmware

### 5.1 FSM Design

The firmware implements a **4-state FSM**. On every 1-second tick the FSM evaluates conditions in strict priority order:

```
┌─────────────────────────────────────────────────────────────────┐
│ Priority 1 — URGENCY (always checked first)                     │
│   fill ≥ 80%  OR  MQ-135 > 2500  OR  MQ-5 > 3500              │
│   OR  dF/dt > 10 %/hr   →  ALERT  (TX floor: 60 s)            │
├─────────────────────────────────────────────────────────────────┤
│ Priority 2 — BATTERY LOW                                        │
│   batt ≤ 30%  →  CONSERVE  (TX floor: 900 s)                  │
├─────────────────────────────────────────────────────────────────┤
│ Priority 3 — ACTIVITY                                           │
│   fill ≥ 60%  →  ACTIVE  (TX floor: 300 s)                    │
├─────────────────────────────────────────────────────────────────┤
│ Priority 4 — DEFAULT                                            │
│   →  STABLE  (AIMD-controlled interval: 300 s – 1800 s)       │
└─────────────────────────────────────────────────────────────────┘
```

State transitions are evaluated **every loop tick** — urgency can preempt any lower-priority state instantly.

**Publish trigger logic:**

```
publish if:
  (urgent AND stateChanged)   ← one immediate TX on ALERT entry
  OR timerFired               ← AoI floor elapsed
  OR deadbandCrossed          ← fill changed by ≥ ΔF*
```

The `urgent AND stateChanged` condition ensures a single immediate transmission when ALERT is first entered; subsequent ALERT ticks use the 60 s timer, preventing a flood of packets.

### 5.2 AIMD Interval Adaptation

In **STABLE** state the TX interval T* is controlled by battery level:

| Battery | Action | Effect |
|---|---|---|
| > 80% | Multiplicative decrease: `T* × 0.5` | Send more often (healthy) |
| < 70% | Additive increase: `T* + 60 s` | Save energy (draining) |
| 70–80% | Hold | Hysteresis band, no change |

Bounds: **T* ∈ [300 s, 1800 s]**. T* resets to 300 s when re-entering STABLE from any other state.

Reference: Giordano et al. (2023) — AIMD for IoT adaptive sampling.

### 5.3 Dynamic Deadband

A fill-change threshold ΔF* suppresses transmissions when the bin level is stable:

```
ΔF* = max(1%, 10% - dF/dt_hr × 0.5)
```

where `dF/dt_hr` is fill velocity in **%/hr**. Fast-filling bins get a tighter deadband (down to 1%); slowly changing bins use the full 10% deadband.

### 5.4 Urgency Conditions

Four conditions independently trigger ALERT. The `reason` field in the payload identifies which fired:

| Condition | Threshold | `reason` value |
|---|---|---|
| Bin fill | ≥ 80% | `urgency-fill` |
| MQ-135 gas | ADC > 2500 (idle ~575) | `urgency-gas135` |
| MQ-5 gas | ADC > 3500 (idle ~814) | `urgency-gas5` |
| Fill velocity | dF/dt > 10 %/hr | `urgency-rate` |

### 5.5 BASELINE Mode

Toggled by publishing `BASELINE` to `bin/bin01/mode`. In baseline mode:

- Fixed 60-second TX interval, no FSM, no filtering.
- `tx_count` resets to 0 on every mode switch.
- The dashboard separates ADAPTIVE and BASELINE counts by filtering on the `mode` field in each stored row.

### 5.6 RTC Persistence & Fast Wi-Fi

On wake from deep sleep (or brownout-reset) the firmware restores FSM context from RTC memory if `rtcMagic == 0xA1B2C3D4`:

```
fsm.state, fsm.txInterval, fsm.prevFill,
lastTxFill, txCount, simulatedBattPct
```

Wi-Fi reconnects faster using the cached BSSID + channel from the previous association.

### 5.7 Deep Sleep

Controlled by `DEEP_SLEEP_ENABLED` in `config.h`:

```c
#define DEEP_SLEEP_ENABLED 0   // 0 = live demo, 1 = energy measurement
```

When enabled, the node sleeps `min(txInterval, 30 s)` between samples (10 s in ALERT). Disabled for PP1 live demo so the serial monitor and MQTT stream stay continuous.

---

## 6. MQTT Topics

| Topic | Direction | Payload | Purpose |
|---|---|---|---|
| `bin/bin01/data` | ESP32 → broker | JSON (see below) | Sensor + FSM data |
| `bin/bin01/mode` | broker → ESP32 | `ADAPTIVE` or `BASELINE` | Switch mode |
| `bin/bin01/battery` | broker → ESP32 | Float string e.g. `"75.0"` | Set simulated battery % |
| `bin/bin01/override` | broker → ESP32 | Reserved | PP2 |

### Payload schema (`bin/bin01/data`)

```json
{
  "node_id":  "bin01",
  "ts":       1234567890,
  "fill_pct": 43.2,
  "temp_c":   28.5,
  "humidity": 61.0,
  "gas_135":  480,
  "gas_5":    720,
  "batt_pct": 80.0,
  "state":    "STABLE",
  "mode":     "ADAPTIVE",
  "tx_count": 14,
  "interval": 420,
  "deadband": 8.3,
  "dfdt":     1.2,
  "reason":   "timer"
}
```

---

## 7. Backend

**Port:** `3001`

| Endpoint | Method | Description |
|---|---|---|
| `/api/readings` | GET | Last N readings from SQLite (`?limit=200`) |
| `/api/stats` | GET | `{ tx_count_adaptive, tx_count_baseline }` |
| `/api/mode/:nodeId` | POST | Publish mode switch to MQTT |
| `/api/battery` | POST | Publish battery % to MQTT |
| `/api/reset` | POST | Delete all readings, emit `reset` via Socket.io |

Socket.io events emitted to dashboard:

- `reading` — every inbound MQTT packet (after DB insert)
- `reset` — after `/api/reset` clears the table

### Database schema

```sql
CREATE TABLE readings (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id   TEXT,    ts       INTEGER,
  fill_pct  REAL,    temp_c   REAL,    humidity REAL,
  gas_135   INTEGER, gas_5    INTEGER, batt_pct REAL,
  state     TEXT,    mode     TEXT,    tx_count INTEGER,
  interval  INTEGER, deadband REAL,    dfdt     REAL,
  reason    TEXT
);
```

> **Schema change notice:** If `smart-bin.db` exists from an older run, delete it before restarting — SQLite `CREATE TABLE IF NOT EXISTS` will not alter an existing table.

---

## 8. Dashboard

**Port:** `5173`

| Panel | Description |
|---|---|
| **TCR display** | Large TCR %, adaptive TX, baseline TX, TX saved, estimated energy saved (TCR × 0.7) |
| **Mode control** | Run BASELINE / Run ADAPTIVE / Reset Data buttons |
| **State badge** | Current FSM state (colour-coded) + last TX reason + dF/dt |
| **Battery slider** | Simulates battery 0–100%, POSTs to `/api/battery` on change, sends 80% on mount |
| **Stat cards** | Fill, Battery, Temp, Gas MQ135, Gas MQ5, dF/dt, TX Interval |
| **Fill chart** | Line chart with ALERT (80%) and ACTIVE (60%) reference lines |
| **Batt & Temp chart** | Dual line chart |
| **Gas chart** | MQ-135 and MQ-5 ADC values with alert reference lines (2500 / 3500) |
| **Risk panel** | MQ warmup status, gateway info, PP2 deployment note |

---

## 9. Mobile (Shell)

Basic Expo + React Native app polling `/api/readings` every 10 s and showing the latest reading. PP1 scope only — full mobile app is PP2.

---

## 10. Running the Stack

### Prerequisites

- PlatformIO CLI or VS Code PlatformIO extension
- Node.js ≥ 18
- Mosquitto MQTT broker (`mosquitto` on PATH)
- Expo CLI (mobile, optional for PP1)

### 1 — Start MQTT broker

```bash
mosquitto -v
```

### 2 — Flash firmware

Edit `firmware/include/config.h`:
```c
#define WIFI_SSID  "your-network"
#define WIFI_PASS  "your-password"
#define MQTT_HOST  "your-laptop-lan-ip"
```

```bash
cd firmware
pio run --target upload
pio device monitor          # 115200 baud
```

### 3 — Start backend

```bash
cd backend
npm install
npm run dev                  # http://localhost:3001
```

### 4 — Start dashboard

```bash
cd dashboard
npm install
npm run dev                  # http://localhost:5173
```

---

## 11. Demo Procedure

1. Flash firmware → open Serial Monitor → confirm `[RTC] fresh boot` then `[MQ] warmup complete` then `[FSM] started`.
2. Open dashboard at `http://localhost:5173`.
3. **Run BASELINE** — click the button, observe fixed 60 s TX cadence, watch TX count climb.
4. **Run ADAPTIVE** — click the button, observe FSM-driven intervals and TCR building.
5. **Trigger ALERT** — cover the HC-SR04 (fill = ~100%) or drag battery slider below 5% to trigger DEAD guard.
6. **Battery stress test** — drag slider to 25% → node enters CONSERVE (900 s interval). Drag to 85% → STABLE with MD decrease.
7. **Gas trigger** — to simulate: publish a high value directly via mosquitto:
   ```bash
   # On the broker machine
   mosquitto_pub -t bin/bin01/override -m "gas"
   # Or: hold a lighter near MQ-5 sensor
   ```
8. Observe TCR panel — target ≥ 60% reduction vs baseline.

---

## 12. Key Design Parameters

| Parameter | Value | Location |
|---|---|---|
| FILL_ALERT | 80% | `config.h` |
| FILL_ACTIVE | 60% | `config.h` |
| BATT_LOW (CONSERVE) | 30% | `config.h` |
| BATT_DEAD (halt TX) | 5% | `config.h` |
| GAS135_THRESHOLD | 2500 ADC | `config.h` |
| GAS5_THRESHOLD | 3500 ADC | `config.h` |
| DFDT_ALERT | 10 %/hr | `config.h` |
| AoI floor — ALERT | 60 s | `config.h` |
| AoI floor — ACTIVE | 300 s | `config.h` |
| AoI floor — CONSERVE | 900 s | `config.h` |
| AIMD T* range | 300 s – 1800 s | `config.h` |
| AIMD additive increase | +60 s / cycle | `config.h` |
| AIMD multiplicative decrease | × 0.5 | `config.h` |
| AIMD AI threshold | batt < 70% | `config.h` |
| AIMD MD threshold | batt > 80% | `config.h` |
| Deadband formula | max(1%, 10% − dF/dt × 0.5) | `fsm.h` |
| HC-SR04 filter | Median of 3 | `sensors.h` |
| BASELINE interval | 60 s fixed | `config.h` |
| MQ warmup | 5 s | `config.h` |
| Deep sleep | Disabled (live demo) | `config.h` |
| Backend port | 3001 | `server.js` |
| Dashboard port | 5173 | `vite.config.js` |

---

## 13. PP2 Roadmap

- Deploy broker on Raspberry Pi (replace laptop proxy)
- Real LiPo battery + fuel gauge IC (MAX17048)
- OTA firmware updates via MQTT
- Multi-node support (multiple bin IDs)
- Full mobile app (React Native + real-time Socket.io)
- Cloud backup (InfluxDB or Firebase) for historical analysis
- Enable `DEEP_SLEEP_ENABLED 1` for actual energy benchmarking
