#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <PubSubClient.h>
#include <BoundedDHT.h>
#include <ArduinoJson.h>
#include "config.h"
#include "sensors.h"
#include "ina219_meter.h"
#include "fsm.h"

// ── Globals ───────────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
ESP8266WiFiMulti wifiMulti;

// Known networks, in priority order. Credentials come from include/secrets.h.
struct WifiAP { const char* ssid; const char* pass; };
static const WifiAP WIFI_APS[] = {
    { WIFI_AP1_SSID, WIFI_AP1_PASS },   // 1. home router
    { WIFI_AP2_SSID, WIFI_AP2_PASS },   // 2. phone hotspot
};
static const uint8_t WIFI_AP_COUNT = sizeof(WIFI_APS) / sizeof(WIFI_APS[0]);
DHT          dht(PIN_DHT, DHT22);
Ina219Meter  powerMeter;
bool         powerMeterOnline  = false;
uint32_t     lastPowerSampleMs = 0;
uint32_t     lastPowerProbeMs  = 0;
char         powerSession[20]  = {};

FSMContext   fsm;
bool         baselineMode     = false;

uint32_t txCount              = 0;    // resets on mode switch; dashboard filters by mode field
uint32_t lastTxMs             = 0;
// Transmissions the policy called for that the broker could not take. Counted
// separately from txCount so an undelivered TX is never mistaken for one the
// policy suppressed -- see noteDroppedTx().
uint32_t droppedTxCount       = 0;
uint32_t lastDropMs           = 0;    // paces [TX-DROPPED] to the policy interval
uint32_t lastMqttAttemptMs    = 0;    // last reconnect attempt, for MQTT_RETRY_MS
bool     batteryDead          = false;
float    simulatedBattPct     = 80.0f; // updated via MQTT on bin/{id}/battery
float    lastTxFill           = 0.0f;  // deadband reference for this boot

#include "gateway_control.h"

// Ten samples per second during cooperative waits provide an on-node energy
// estimate while keeping the existing sensing and MQTT control flow intact.
void servicePowerMeter(bool force = false) {
    const uint32_t now = millis();
    if (powerMeterOnline) {
        if (force || now - lastPowerSampleMs >= 100UL) {
            lastPowerSampleMs = now;
            if (!powerMeter.sample()) {
                powerMeterOnline = false;
                Serial.println("[INA219] read failed; measured power unavailable");
            }
        }
        return;
    }
    if (force || now - lastPowerProbeMs >= 10000UL) {
        lastPowerProbeMs = now;
        if (powerMeter.begin(INA219_ADDRESS) && powerMeter.sample()) {
            powerMeterOnline = true;
            lastPowerSampleMs = millis();
            Serial.println("[INA219] online at 0x40; R100 / 32V-2A calibration");
        }
    }
}

void meteredDelay(uint32_t durationMs) {
    const uint32_t started = millis();
    while (millis() - started < durationMs) {
        servicePowerMeter();
        delay(20);
    }
}

// ── Wi-Fi (no ESP32 RTC cache/deep sleep in this separate bench profile) ──
void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    for (uint8_t i = 0; i < WIFI_AP_COUNT; ++i)
        wifiMulti.addAP(WIFI_APS[i].ssid, WIFI_APS[i].pass);
    // As in the current node01 profile, initial setup waits for a known AP.
    // Wi-Fi-off standalone sensing is not claimed by this build.
    while (wifiMulti.run(NODE_WIFI_ASSOC_TIMEOUT_MS) != WL_CONNECTED) {
        Serial.println("[WiFi] waiting for a known network");
        meteredDelay(500);
    }
    Serial.printf("[WiFi] connected; ip=%s\n", WiFi.localIP().toString().c_str());
}

// ── MQTT callbacks ────────────────────────────────────────────────────────
void onMessage(char* topic, byte* payload, unsigned int len) {
    if (strcmp(topic, TOPIC_OVERRIDE) == 0) { gatewayHandle(payload, len); return; }
    char buf[64] = {};
    memcpy(buf, payload, min(len, (unsigned int)63));

    if (strcmp(topic, TOPIC_MODE) == 0) {
        bool wasBaseline = baselineMode;
        baselineMode = (strncmp(buf, "BASELINE", 8) == 0);
        if (baselineMode != wasBaseline) {
            gatewayCancel();
            txCount        = 0;   // reset counter on every mode switch
            droppedTxCount = 0;   // per-mode, so it stays comparable with txCount
            lastDropMs     = 0;
            lastTxMs       = millis();
        }
        Serial.printf("[MQTT] mode -> %s (txCount reset)\n", baselineMode ? "BASELINE" : "ADAPTIVE");
    } else if (strcmp(topic, TOPIC_BATTERY) == 0) {
        simulatedBattPct = atof(buf);
        Serial.printf("[MQTT] battery -> %.1f%%\n", simulatedBattPct);
    }
}

// Subscriptions live in the broker-side session and die with the socket, so
// they must be re-established on every successful connect, not just the first.
// A reconnect that restores the socket but silently loses the subscriptions
// leaves mode and battery commands dead mid-demo.
bool mqttSubscribeAll() {
    bool ok = true;
    ok &= mqtt.subscribe(TOPIC_MODE);
    ok &= mqtt.subscribe(TOPIC_OVERRIDE);
    ok &= mqtt.subscribe(TOPIC_BATTERY);
    Serial.printf("[MQTT] subscribed mode+override+battery: %s\n", ok ? "ok" : "FAILED");
    return ok;
}

// Exactly one connection attempt, then return. Never loops: the caller decides
// when to try again, so broker availability cannot gate the control loop.
bool mqttTryConnect() {
    if (mqtt.connected()) return true;
    Serial.print("[MQTT] connecting...");
    if (mqtt.connect(NODE_ID)) {
        Serial.println(" ok");
        mqttSubscribeAll();
        return true;
    }
    Serial.printf(" failed rc=%d\n", mqtt.state());
    return false;
}

// A transmission the policy called for but the broker could not take.
// txCount and fsmOnTransmit are NOT touched when local publish fails.
// Attempts/reconnections can still consume energy. txCount counts local QoS 0
// publish acceptance; backend receipt counts remain the delivery measurement.
//
// lastTxMs IS advanced, to the tick that logged the drop. Without this, the
// timerFired check in the caller (`now - lastTxMs >= interval`) stays true on
// every subsequent 1 Hz tick for as long as the broker is down -- once the
// timer has fired, it never un-fires, so a broker outage inflated
// droppedTxCount by ~3600/hour instead of counting one drop per interval.
// Advancing lastTxMs restarts that timer exactly as a real transmission
// would have, so the outage is metered at the FSM's own cadence.
//
// lastTxFill is deliberately left untouched -- it is the deadband reference,
// and it must stay at the last value accepted by the MQTT client, so that a
// real fill change during the outage is not missed once the broker returns.
// (For the deadband-triggered case specifically, leaving lastTxFill alone
// means deadbandCrossed can stay true every tick on its own; the lastDropMs
// gate below is what caps that path to one counted drop per interval too.)
void noteDroppedTx(const char* reason, uint32_t now, uint32_t interval) {
    if (lastDropMs != 0 && now - lastDropMs < interval) return;
    lastDropMs = now;
    droppedTxCount++;
    lastTxMs = now;
    Serial.printf("[TX-DROPPED] reason=%s dropped=%lu rc=%d (txCount held at %lu)\n",
                  reason, (unsigned long)droppedTxCount, mqtt.state(),
                  (unsigned long)txCount);
}

// ── Publish ───────────────────────────────────────────────────────────────
bool publishReading(const SensorReading& r, bool isBaseline,
                    const char* txTrigger, const char* stateCause,
                    float deadband, float dFdt) {
    JsonDocument doc;
    doc["node_id"]  = NODE_ID;
    doc["ts"]       = millis();
    doc["fill_pct"] = r.fillPct;
    doc["temp_c"]   = r.tempC;
    doc["humidity"] = r.humidity;
    doc["gas_135"]  = nullptr;  // not fitted; never fabricate zero measurements
    doc["gas_5"]    = nullptr;
    doc["sensor_profile"] = NODE_SENSOR_PROFILE;
    doc["power_session"] = powerSession;
    doc["power_profile"] = "ina219-r100-32v2a";
    doc["power_mode"] = "live-connected-10hz";
    doc["ina_present"] = r.powerPresent;
    if (r.powerPresent) {
        doc["bus_voltage_v"] = r.busVoltageV;
        doc["current_ma"] = r.currentMa;
        doc["power_mw"] = r.powerMw;
        doc["energy_mwh"] = r.energyMWh;
        doc["power_sample_ms"] = r.powerMeasurementMs;
    } else {
        doc["bus_voltage_v"] = nullptr;
        doc["current_ma"] = nullptr;
        doc["power_mw"] = nullptr;
        doc["energy_mwh"] = nullptr;
        doc["power_sample_ms"] = nullptr;
    }
    doc["batt_pct"] = r.battPct;
    doc["state"]    = r.battPct < BATT_DEAD ? "DEAD" : stateName(fsm.state);
    doc["mode"]     = isBaseline ? "BASELINE" : "ADAPTIVE";
    doc["tx_count"] = txCount + 1;  // commit only after local publish acceptance
    doc["tx_dropped"] = droppedTxCount;   // policy-fired TX the broker could not take
    doc["interval"] = (isBaseline ? BASELINE_INTERVAL : fsm.txInterval) / 1000;
    doc["deadband"] = deadband;
    doc["dfdt"]     = dFdt;               // fsm.h already returns %/hr
    doc["reason"]   = txTrigger; // retained for older dashboard/API clients
    doc["tx_trigger"] = txTrigger;
    doc["state_cause"] = stateCause;
    // Only present when the ultrasonic has been faulting for longer than
    // FILL_FAULT_HOLD_MAX reads, so the held fill value is visible in the data.
    if (r.fillFaultSustained) doc["sensor_fault"] = true;

    gatewayTelemetry(doc, isBaseline, r.battPct);
    char buf[896];   // bounded telemetry including gateway and INA219 evidence
    const size_t needed = measureJson(doc);
    if (doc.overflowed() || needed >= sizeof(buf)) {
        Serial.printf("[TX-FAILED] JSON does not fit: %u bytes\n", (unsigned)needed);
        return false;
    }
    const size_t n = serializeJson(doc, buf);
    if (n != needed || !mqtt.publish(TOPIC_DATA, buf, n)) {
        Serial.printf("[TX-FAILED] publish rejected: bytes=%u buffer=%u rc=%d\n",
                      (unsigned)n, (unsigned)mqtt.getBufferSize(), mqtt.state());
        return false;
    }
    // PubSubClient publishes at QoS 0. This means accepted by the local client,
    // not acknowledged/stored by the broker. Verify receipt against backend rows.
    ++txCount;
    Serial.printf("[TX] %s\n", buf);
    return true;
}

// ── Sensor read ───────────────────────────────────────────────────────────
// readFillPct() returns -1 when the HC-SR04 median read times out. That is a
// sensor dropout, not an empty bin. Substituting 0 (the PP1 behaviour) turned
// every dropout into an instant 45% -> 0% step, which produced an enormous
// dF/dt and a false urgency-rate ALERT: 55 of the 113 transmissions in the
// 12 May run, with a recorded dF/dt of 261,322 %/hr. Hold the last good value
// instead — the fault path must never fabricate a fill value that can trip
// urgency.
SensorReading readSensors() {
    static float    lastGoodFill      = NAN; // unknown until a valid echo
    static uint16_t consecutiveFaults = 0;

    SensorReading r;
    float fill = Ultrasonic::readFillPct();
    if (fill < 0.0f) {
        if (consecutiveFaults < UINT16_MAX) ++consecutiveFaults;
        r.fillPct = lastGoodFill;          // hold; do NOT substitute 0
        if (consecutiveFaults == FILL_FAULT_HOLD_MAX + 1)
            Serial.printf("[FILL] WARN ultrasonic fault x%u — holding %.1f%%\n",
                          consecutiveFaults, lastGoodFill);
    } else {
        if (consecutiveFaults > FILL_FAULT_HOLD_MAX)
            Serial.printf("[FILL] ultrasonic recovered after %u faults\n",
                          consecutiveFaults);
        consecutiveFaults = 0;
        lastGoodFill      = fill;
        r.fillPct         = fill;
    }
    r.fillFault          = (consecutiveFaults > 0);
    r.fillFaultSustained = (consecutiveFaults > FILL_FAULT_HOLD_MAX);

    r.tempC    = dht.readTemperature();
    r.humidity = dht.readHumidity();
    static bool dhtFault = false;
    const bool failedDht = isnan(r.tempC) || isnan(r.humidity);
    if (failedDht && !dhtFault)
        Serial.println("[DHT] read failed; temperature/humidity unavailable; sensing continues");
    else if (!failedDht && dhtFault)
        Serial.println("[DHT] recovered");
    dhtFault = failedDht;
    // Adapter inputs only: keep absent gas branches inactive in the shared
    // FSM. Published gas fields are null, and no floating ADC input is read.
    r.gasRaw135 = 0;
    r.gasRaw5   = 0;
    r.battPct   = simulatedBattPct;
    servicePowerMeter(true);
    r.powerPresent = powerMeterOnline && powerMeter.present();
    r.busVoltageV = r.powerPresent ? powerMeter.busVoltageV() : NAN;
    r.currentMa = r.powerPresent ? powerMeter.currentMa() : NAN;
    r.powerMw = r.powerPresent ? powerMeter.powerMw() : NAN;
    r.energyMWh = r.powerPresent ? powerMeter.energyMWh() : NAN;
    r.powerMeasurementMs = r.powerPresent ? powerMeter.measurementMs() : 0;
    return r;
}

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    snprintf(powerSession, sizeof(powerSession), "%06lx-%08lx",
             (unsigned long)ESP.getChipId(), (unsigned long)ESP.getCycleCount());
    Serial.printf("[POWER] boot session=%s; live-connected 10 Hz integration\n", powerSession);
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
    dht.begin();

    Wire.begin(PIN_INA_SDA, PIN_INA_SCL);
    servicePowerMeter(true);

    connectWifi();

    // 448-byte JSON buffer + MQTT header (5), topic length (2), and topic.
    // The old 256-byte default silently rejected payloads above 235 bytes.
    static_assert(896 + 5 + 2 + sizeof(TOPIC_DATA) - 1 <= 1024,
                  "MQTT buffer must fit the complete data packet");
    if (!mqtt.setBufferSize(1024))
        Serial.println("[MQTT] ERROR allocating 1024-byte packet buffer");
    Serial.printf("[BUILD] bin02 ESP8266 fill+DHT+INA219; MQTT buffer=%u; bounded DHT\n",
                  (unsigned)mqtt.getBufferSize());
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMessage);

    // Bounded wait. An edge node must run its control loop whether or not the
    // broker answered, so this gives up after MQTT_SETUP_TIMEOUT_MS and
    // continues to FSM start regardless; no gas heater warmup is needed.
    Serial.printf("[MQTT] broker %s:%d, waiting up to %lus\n",
                  MQTT_HOST, MQTT_PORT, MQTT_SETUP_TIMEOUT_MS / 1000);
    uint32_t mqttStart = millis();
    while (!mqttTryConnect()) {
        if (millis() - mqttStart >= MQTT_SETUP_TIMEOUT_MS) {
            Serial.printf("[MQTT] no broker after %lus - starting anyway; the FSM "
                          "runs and transmissions are logged as [TX-DROPPED]\n",
                          MQTT_SETUP_TIMEOUT_MS / 1000);
            break;
        }
        meteredDelay(MQTT_RETRY_MS);
    }
    lastMqttAttemptMs = millis();

    lastTxMs = millis();
    Serial.println("[FSM] started in ADAPTIVE mode");
}

// ── Loop ──────────────────────────────────────────────────────────────────
void loop() {
    // At most one reconnect attempt per MQTT_RETRY_MS, then straight on to the
    // FSM. Never a loop: sensing and state evaluation must keep their schedule
    // through a broker outage.
    uint32_t now = millis();
    if (!mqtt.connected() && now - lastMqttAttemptMs >= MQTT_RETRY_MS) {
        lastMqttAttemptMs = now;
        mqttTryConnect();
        now = millis();   // a refused attempt still costs socket time
    }
    if (mqtt.connected()) mqtt.loop();

    SensorReading r = readSensors();
    // A mode callback in mqtt.loop() can reset lastTxMs after the timestamp
    // captured above. Refresh it to avoid unsigned elapsed-time underflow.
    now = millis();
    gatewayService(now);

    // ── Dead battery guard (runs before everything, including BASELINE) ───
    if (r.battPct >= BATT_DEAD) batteryDead = false;
    if (r.battPct < BATT_DEAD) {
        if (!batteryDead) {
            if (mqtt.connected() && publishReading(r, baselineMode, "battery-dead", "battery-dead", 0, 0)) {
                batteryDead = true;   // latch only after local publish acceptance
                Serial.println("[BATT] dead battery — TX halted");
            } else {
                noteDroppedTx("battery-dead", now, MQTT_RETRY_MS);
            }
        }
        meteredDelay(5000);
        return;
    }

    // ── BASELINE branch ───────────────────────────────────────────────────
    if (baselineMode) {
        if (now - lastTxMs >= BASELINE_INTERVAL) {
            if (mqtt.connected() && publishReading(r, true, "baseline-timer", "baseline-fixed-policy", 0.0f, 0.0f)) {
                lastTxMs = now;
            } else {
                noteDroppedTx("baseline", now, BASELINE_INTERVAL);
            }
        }
        meteredDelay(1000);
        return;
    }

    // ── ADAPTIVE branch ───────────────────────────────────────────────────
    static State prevState = State::STABLE;
    float deadband = computeDeadband(fsm.dFdtHr), dFdt = fsm.dFdtHr;
    // No fill-based decision is possible before the first valid echo. Keep
    // telemetry null and the initial policy state until an actual measurement.
    bool urgent = !isnan(r.fillPct) &&
        fsmUpdate(fsm, r.fillPct, r.battPct, r.gasRaw135, r.gasRaw5, deadband, dFdt, now);

    bool stateChanged = (fsm.state != prevState);
    prevState = fsm.state;

    float fillDelta      = fabs(r.fillPct - lastTxFill);
    const uint32_t effectiveInterval = gatewayInterval(fsm.txInterval, now);
    bool timerFired      = (now - lastTxMs >= effectiveInterval);
    const bool urgentTransition = urgent && stateChanged;
    const bool deadbandReady = lastTxMs == 0 || now - lastTxMs >= DEADBAND_COOLDOWN_MS;
    bool deadbandCrossed = (fillDelta >= deadband) && deadbandReady;

    if (urgentTransition || timerFired || deadbandCrossed) {
        const char* txTrigger;
        if (urgentTransition) {
            txTrigger = "urgent-transition";
        } else if (timerFired) {
            txTrigger = effectiveInterval < fsm.txInterval ? "gateway-timer" : "timer";
        } else {
            txTrigger = "deadband";
        }
        const char* stateCause;
        if (r.fillFaultSustained)               stateCause = "fill-sensor-fault";
        else if (fsm.state == State::CONSERVE)  stateCause = "low-battery";
        else if (fsm.state == State::ALERT && r.fillPct >= FILL_ALERT)
                                                stateCause = "fill-alert";
        else if (fsm.state == State::ALERT)      stateCause = "fill-rate-alert";
        else if (fsm.state == State::ACTIVE)     stateCause = "active-fill";
        else                                     stateCause = "stable";
        if (!mqtt.connected() || !publishReading(r, false, txTrigger, stateCause, deadband, dFdt)) {
            // The policy fired but the local publish was not accepted. noteDroppedTx()
            // restarts the interval timer (lastTxMs) but leaves lastTxFill,
            // txCount and the AIMD step untouched -- see its comment.
            noteDroppedTx(txTrigger, now, effectiveInterval);
        } else {
            lastTxMs   = now;
            lastTxFill = r.fillPct;

            Serial.printf("[FSM] state=%s interval=%lus deadband=%.1f%% delta=%.1f%% reason=%s\n",
                          stateName(fsm.state),
                          (unsigned long)(fsm.txInterval / 1000),
                          deadband,
                          fillDelta,
                          txTrigger);

            // AIMD is a per-cycle control law, so the step belongs on the
            // transmission event. Running it on every 1 s loop tick (the PP1
            // behaviour, since removed from fsmUpdate) halved T* every second
            // while the battery was healthy and pinned it to the 300 s floor —
            // which is why interval reads 300 on every row of the 12 May data.
            // It runs after the log so the logged interval matches the one in
            // the payload: both report the T* that governed the cycle just
            // closed; the new T* takes effect from the next tick.
            fsmOnTransmit(fsm, r.battPct);
        }
    }

    meteredDelay(1000);
}
