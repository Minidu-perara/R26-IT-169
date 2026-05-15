#pragma once

// ── Wi-Fi ─────────────────────────────────────────────────────────────────
#define WIFI_SSID   "Redmi Note 14"
#define WIFI_PASS   "qqqqqqqq"

// ── MQTT ──────────────────────────────────────────────────────────────────
#define MQTT_HOST   "10.97.85.244"   // broker LAN IP
#define MQTT_PORT   1883
#define NODE_ID     "bin01"

// topic helpers
#define TOPIC_DATA      "bin/" NODE_ID "/data"
#define TOPIC_MODE      "bin/" NODE_ID "/mode"
#define TOPIC_OVERRIDE  "bin/" NODE_ID "/override"
#define TOPIC_BATTERY   "bin/" NODE_ID "/battery"

// ── Pin map (ESP32) ───────────────────────────────────────────────────────
#define PIN_TRIG    13
#define PIN_ECHO    14
#define PIN_MQ135   34   // ADC1_CH6, input-only
#define PIN_MQ5     35   // ADC1_CH7, input-only
#define PIN_DHT     4

// ── HC-SR04 ───────────────────────────────────────────────────────────────
#define BIN_DEPTH_CM    40.0f   // empty bin = sensor to bin floor distance
#define FILL_MAX_CM     2.0f    // full = sensor reads this close to lid

// ── FSM thresholds ────────────────────────────────────────────────────────
#define FILL_ALERT      80.0f   // % — triggers ALERT state
#define FILL_ACTIVE     60.0f   // % — triggers ACTIVE state
#define BATT_LOW        30.0f   // % — triggers CONSERVE state
#define BATT_DEAD        5.0f   // % — dead battery guard, halt TX
#define GAS135_THRESHOLD  2500  // ADC raw — MQ-135 alert (idle ~575)
#define GAS5_THRESHOLD    3500  // ADC raw — MQ-5 alert (idle ~814)
#define DFDT_ALERT        10.0f // %/hr — rapid fill rate triggers ALERT
#define MQ_WARMUP_MS      5000UL   // sensor warmup before TX

// ── AoI interval floors (ms) ──────────────────────────────────────────────
#define AoI_ALERT     60000UL
#define AoI_ACTIVE    300000UL
#define AoI_STABLE    1800000UL
#define AoI_CONSERVE  900000UL

// ── AIMD parameters (STABLE state) ────────────────────────────────────────
#define AIMD_AI              60000UL  // additive increase step: +60 s
#define AIMD_MD              0.5f     // multiplicative decrease factor
#define AIMD_MIN             300000UL // floor: 300 s (T* start)
#define AIMD_MAX             1800000UL// ceiling: 1800 s
#define BATT_AIMD_AI_THRESH  70.0f   // batt < this → apply AI (send less)
#define BATT_AIMD_MD_THRESH  80.0f   // batt > this → apply MD (send more)

// ── Baseline mode ─────────────────────────────────────────────────────────
#define BASELINE_INTERVAL  60000UL   // fixed 60 s TX in BASELINE mode

// ── Deep sleep (0 = live demo, 1 = energy measurement) ───────────────────
#define DEEP_SLEEP_ENABLED 0
