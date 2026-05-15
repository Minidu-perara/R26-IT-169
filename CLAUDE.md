R26-IT-169 Smart Bin Node — Claude Code Context

## Project
Edge-based energy-efficient IoT smart waste management system.
PP1 deadline: tomorrow. Solo project by Perera G.M.T (IT21196638).

## What's already designed (do not change these decisions)
- ESP32 firmware: 4-state FSM (STABLE/ACTIVE/ALERT/CONSERVE)
- AIMD interval adaptation within STABLE state (Giordano et al. 2023)
- Dynamic deadband from fill velocity: ΔF* = max(1%, 10% - dF/dt × 0.5)
- Urgency always overrides energy conservation (urgency check runs first)
- AoI floors per state: ALERT=60s, ACTIVE=300s, STABLE=1800s, CONSERVE=900s
- BASELINE mode: fixed 60s TX, no filtering (comparison baseline)
- Sensors: HC-SR04 (ultrasonic fill), MQ-series (gas), DHT22 (temp/humidity)
- Battery simulated via potentiometer on ADC pin 35 for demo

## Stack
- Firmware: PlatformIO + Arduino framework
- MQTT broker: Mosquitto local (localhost:1883)
- Backend: Node + Express + mqtt.js + Socket.io + better-sqlite3
- Dashboard: Vite + React + Tailwind + Recharts
- Mobile: Expo + React Native (basic shell only for PP1)

## Key constraints
- PP1 = 50% milestone. Mobile app is low priority — basic shell only.
- Demo must show side-by-side TX count: BASELINE vs ADAPTIVE
- All fills ≥ 80% must be transmitted within 60s (alert recall = 100%)
- MQTT topic convention: bin/{node_id}/data, bin/{node_id}/mode, bin/{node_id}/override
- Backend port: 3001. Dashboard port: 5173.

## What NOT to do
- Do not add ML models. Rule-based FSM only.
- Do not add authentication for PP1.
- Do not use cloud MQTT (Mosquitto local only).
- Do not change the FSM logic or threshold values without asking.
