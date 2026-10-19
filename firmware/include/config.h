#pragma once

// ── Wi-Fi ─────────────────────────────────────────────────────────────────
// Credentials live in include/secrets.h, which is gitignored. Everything else
// about the Wi-Fi policy stays here. If secrets.h is absent the build must
// fail loudly here, not later as an undefined WIFI_AP1_SSID in main.cpp.
#if defined(__has_include)
  #if !__has_include("secrets.h")
    #error "include/secrets.h is missing. Copy include/secrets.example.h to include/secrets.h and fill in the Wi-Fi credentials."
  #endif
#endif
#include "secrets.h"

#if !defined(WIFI_AP1_SSID) || !defined(WIFI_AP1_PASS) ||     !defined(WIFI_AP2_SSID) || !defined(WIFI_AP2_PASS)
  #error "include/secrets.h is incomplete. It must define WIFI_AP1_SSID/PASS and WIFI_AP2_SSID/PASS - see include/secrets.example.h."
#endif

// Multi-AP association budget. The node caches the BSSID, channel and AP index
// of the last successful association in RTC memory and retries that exact AP
// directly on the next boot/wake; this skips the full channel scan, which is
// the dominant radio-on cost of associating. WIFI_FAST_TIMEOUT_MS bounds that
// attempt so a moved or powered-down AP costs at most this much before the
// scanning fallback (wifiMulti.run) takes over.
#define WIFI_FAST_TIMEOUT_MS   5000UL
#define WIFI_SCAN_TIMEOUT_MS   8000UL   // per wifiMulti.run() attempt

// ── MQTT ──────────────────────────────────────────────────────────────────
// The broker address is whatever machine runs Mosquitto on the day, so it
// must not be a source edit. Override it at build time (one line):
//   pio run -e esp32dev -t upload --upload-port COM6 -DMQTT_HOST_OVERRIDE='\"10.72.205.7\"'
// The value below is only the fallback default.
#ifdef MQTT_HOST_OVERRIDE
  #define MQTT_HOST MQTT_HOST_OVERRIDE
#else
  #define MQTT_HOST "192.168.1.3"   // default broker LAN IP
#endif
#define MQTT_PORT   1883
#define NODE_ID     "bin01"

// Broker availability must never gate the control loop. setup() waits at most
// MQTT_SETUP_TIMEOUT_MS for a first connection and then starts the FSM
// regardless; loop() retries at most once every MQTT_RETRY_MS, one attempt at
// a time, so sensing and state evaluation keep running through an outage.
#define MQTT_SETUP_TIMEOUT_MS  30000UL
#define MQTT_RETRY_MS           5000UL

// topic helpers
#define TOPIC_DATA      "bin/" NODE_ID "/data"
#define TOPIC_MODE      "bin/" NODE_ID "/mode"
#define TOPIC_OVERRIDE  "bin/" NODE_ID "/override"
#define TOPIC_BATTERY   "bin/" NODE_ID "/battery"
#define TOPIC_CALIBRATE      "bin/" NODE_ID "/calibrate"
#define TOPIC_CALIBRATE_ACK  "bin/" NODE_ID "/calibrate/ack"

// ── Pin map (ESP32) ───────────────────────────────────────────────────────
#define PIN_TRIG    13
#define PIN_ECHO    14
#define PIN_MQ135   34   // ADC1_CH6, input-only
#define PIN_MQ5     35   // ADC1_CH7, input-only
#define PIN_DHT     4
#define PIN_INA_SDA 21
#define PIN_INA_SCL 22
#define INA219_ADDRESS 0x40
#define ENABLE_MQ5  0    // removed from the experiment after unstable floating readings
#define NODE_SENSOR_PROFILE "fill_dht_mq135_ina219"

// ── HC-SR04 ───────────────────────────────────────────────────────────────
// Per node: the distance at which fill reads 0 %. Set it 1.5-2 cm SHORTER than
// the measured empty-bin distance, so echo jitter at empty clamps to 0 %
// instead of reading as fill and tripping the 10 %/hr fill-rate ALERT.
// bin01, 20 Oct 2026: empty bin reads 15.8-16.8 cm (eval/hwcheck_bin01_20oct_empty.log),
// so 15.8 - 1.8 = 14.0 cm. Override at build time with -DBIN_DEPTH_CM=...
// This is the compiled default only: a valid depth stored in flash by
// "Calibrate empty" (below) replaces it at boot.
#ifndef BIN_DEPTH_CM
#define BIN_DEPTH_CM    14.0f
#endif
#define FILL_MAX_CM     2.0f    // full = sensor reads this close to lid

// ── Calibrate empty (bin/{node_id}/calibrate) ────────────────────────────
// The installer mounts the node over the empty bin and the node measures its
// own depth: CAL_PINGS echoes over CAL_SPAN_MS, and the median of the valid
// ones minus CAL_MARGIN_CM becomes the depth (the same margin as above).
// Refused with fewer than CAL_MIN_VALID valid echoes, a spread over
// CAL_MAX_SPREAD_CM (max - min after dropping the single lowest and highest
// echo, so one stray echo cannot veto it), or a median under CAL_MIN_MEDIAN_CM
// (something is in the bin). Stored in NVS (Preferences) and loaded at boot.
#define CAL_PINGS          15
#define CAL_SPAN_MS        5000UL
#define CAL_MIN_VALID      10
#define CAL_MAX_SPREAD_CM  1.0f
#define CAL_MIN_MEDIAN_CM  5.0f
#define CAL_MARGIN_CM      1.5f
#define CAL_DEPTH_MAX_CM   400.0f   // valid depth: > FILL_MAX_CM + 3 and below this
#define CAL_PREFS_NS       "bincal" // Preferences namespace and key (max 15 chars)
#define CAL_PREFS_KEY      "depth_cm"

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
#define DEADBAND_COOLDOWN_MS 10000UL // cap nonurgent change-trigger bursts at 6/min

// ── Deep sleep (0 = live demo, 1 = energy measurement) ───────────────────
#define DEEP_SLEEP_ENABLED 0

// -- Fill-velocity estimation --------------------------------------------
// dF/dt must be measured over a window long enough that sensor quantisation
// does not dominate. At a 1 s tick a 0.003 %/s step already reads as 10 %/hr,
// which made the rate trigger fire on noise. 60 s window, clamped.
#define VEL_WINDOW_MS   60000UL   // min elapsed time before recomputing dF/dt
#define DFDT_CLAMP      500.0f    // %/hr — guard against residual spikes
#define FILL_FAULT_HOLD_MAX 5     // consecutive faults tolerated before flagging
