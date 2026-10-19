R26-IT-169 Smart Bin Node — Claude Code Context

## Project
Edge-based energy-efficient IoT smart waste management system.
Solo project by Perera G.M.T (IT21196638).
PP1 (50% milestone) delivered. **PP2 demo: Tue 1 Sep 2026.**

## What's already designed (do not change these decisions)
- ESP32 firmware: 4-state FSM (STABLE/ACTIVE/ALERT/CONSERVE), re-evaluated every 1 s tick
- AIMD interval adaptation within STABLE state (Giordano et al. 2023).
  One step per transmission, applied by `fsmOnTransmit()` in fsm.h — NOT per loop tick.
- Dynamic deadband from fill velocity: ΔF* = max(1%, 10% - dF/dt × 0.5)
- dF/dt is in **%/hr** everywhere — fsm.h, the payload and the dashboard. It is
  measured over a 60 s window (VEL_WINDOW_MS) and clamped to ±500 %/hr (DFDT_CLAMP).
- Urgency always overrides energy conservation (urgency check runs first)
- AoI floors per state: ALERT=60s, ACTIVE=300s, STABLE=1800s, CONSERVE=900s
- BASELINE mode: fixed 60s TX, no filtering (comparison baseline)
- A failed HC-SR04 read holds the last good fill value and raises a fault flag —
  it is never converted to 0, which made dropouts read as the bin emptying and
  fired false urgency-rate ALERTs. Past FILL_FAULT_HOLD_MAX (5) consecutive
  faults the payload carries `"sensor_fault": true`.

## Hardware / pins (config.h)
Full detail and measured values: `HARDWARE.md` (verified 17 Oct 2026). Two nodes:
- **bin01**: ESP32-D0WD-V3, MAC 1c:c3:ab:fa:42:6c, COM6 (CP210x 10C4:EA60).
  HC-SR04 TRIG 13 / ECHO 14 (median-of-3 filter), DHT22 pin 4, MQ-135 ADC 34,
  INA219 SDA 21 / SCL 22 at 0x40 on the 5 V USB input. MQ-5 (pin 35) is removed
  (`ENABLE_MQ5 0`). Firmware: `firmware/`.
- **bin02**: ESP8266EX (ESP-12E), MAC 84:0d:8e:b4:0a:42. HC-SR04 TRIG 13 / ECHO 14,
  DHT22 pin 4, INA219 SDA 12 / SCL 5 at 0x40. No gas sensor. Firmware lives in
  this repo at `firmware-bin02/` (moved in on 19 Oct 2026).
- Both boards share a CP210x adapter serial (0001), so tell them apart by MAC.
- No solar panel or battery is fitted.
- Battery is **not** a potentiometer — ADC2 conflicts with Wi-Fi. Battery % is
  injected over MQTT on `bin/{node_id}/battery` (dashboard slider) and held in
  `simulatedBattPct`, which the FSM reads exactly as it would a real ADC value.

## Stack
- Firmware: PlatformIO + Arduino framework
- MQTT broker: Mosquitto local (localhost:1883)
- Backend: Node + Express + mqtt.js + Socket.io + better-sqlite3 — port 3002
- Dashboard: Vite + React + Tailwind + Recharts — port 5173
- Mobile: Expo + React Native (basic shell only, polls /api/readings)

## Backend / data contract
- MQTT topics: `bin/{node_id}/data` (up), `bin/{node_id}/mode`,
  `bin/{node_id}/battery`, `bin/{node_id}/override` (down; override reserved for PP2)
- Every stored row carries `rx_ts` (server `Date.now()` at receipt). The device's
  `ts` is `millis()` and resets on every boot, so it cannot window an experiment.
- `run_label` tags an evaluation run — `POST /api/run { label }` to set,
  `GET /api/run` to read. Stamped on every row inserted afterwards.
- `/api/stats` counts stored rows per mode (`COUNT(*)`). The device's `tx_count`
  resets on every mode switch and reboot, so it must never be used as a total.
- Express and Socket.io both allow origin `http://localhost:5173`.
- Dashboard TX counters increment locally per `reading` event and reconcile
  against `/api/stats` on mount, on mode switch and on socket reconnect.

## Key constraints
- Demo must show side-by-side TX count: BASELINE vs ADAPTIVE
- All fills ≥ 80% must be transmitted within 60s (alert recall = 100%)
- Mobile app is low priority — basic shell only.

## What NOT to do
- Do not add ML models. Rule-based FSM only.
- Do not add authentication.
- Do not use cloud MQTT (Mosquitto local only).
- Do not change the FSM logic or threshold values without asking.
- Do not re-scale `dfdt` by 3600 anywhere — it is already %/hr.
- Do not substitute a fill value on a sensor fault.
