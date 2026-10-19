#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <PubSubClient.h>
#include <BoundedDHT.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
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
// Runtime bin depth: the calibrated depth from EEPROM, or the compiled
// BIN_DEPTH_CM default when nothing valid is stored. Every fill uses it.
float    binDepthCm           = BIN_DEPTH_CM;
// Last valid fill, held by readSensors() through an ultrasonic dropout (NAN,
// unknown, until a valid echo). File scope so a calibration can reset it
// with the rest of the fill history.
float    lastGoodFill         = NAN;
// "Calibrate empty": onMessage() only records the request, loop() runs it.
bool     calPending           = false;
char     calRequestId[33]     = {};

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

// ── Calibrate empty ───────────────────────────────────────────────────────
// bin/{node_id}/calibrate measures the empty bin and replaces the depth that
// every fill is computed against. onMessage() only records the request: the
// measurement takes about CAL_SPAN_MS, so it runs from loop(), outside the
// PubSubClient callback, with mqtt.loop() and the INA219 serviced between
// pings. The result is acknowledged on bin/{node_id}/calibrate/ack.

static bool calDepthValid(float depthCm) {
    return !isnan(depthCm) && depthCm > FILL_MAX_CM + 3.0f && depthCm < CAL_DEPTH_MAX_CM;
}

// Stored at offset 0 of the emulated EEPROM sector. The magic number and the
// checksum make erased flash (all 0xFF) or a damaged record read as nothing
// stored, so the compiled default is used.
struct CalRecord {
    uint32_t magic;
    float    depthCm;
    uint32_t check;
};

// FNV-1a over the fields before check.
static uint32_t calRecordCheck(const CalRecord& rec) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&rec);
    uint32_t h = 2166136261UL;
    for (size_t i = 0; i < offsetof(CalRecord, check); ++i) { h ^= p[i]; h *= 16777619UL; }
    return h;
}

// NAN when nothing valid is stored.
static float calLoadDepth() {
    CalRecord rec;
    EEPROM.begin(sizeof(CalRecord));
    EEPROM.get(0, rec);
    EEPROM.end();
    if (rec.magic != CAL_EEPROM_MAGIC || rec.check != calRecordCheck(rec)) return NAN;
    return rec.depthCm;
}

// Committed, then read back from flash, so success means the depth is stored.
static bool calSaveDepth(float depthCm) {
    CalRecord rec = { CAL_EEPROM_MAGIC, depthCm, 0 };
    rec.check = calRecordCheck(rec);
    EEPROM.begin(sizeof(CalRecord));
    EEPROM.put(0, rec);
    const bool committed = EEPROM.commit();
    EEPROM.end();
    return committed && calLoadDepth() == depthCm;
}

// Payload resolution: 0.1 cm for depths, 0.01 cm for the median and spread.
static double calRound(float value, float scale) {
    return roundf(value * scale) / (double)scale;
}

// Same request_id rule as the gateway lease: 1-32 chars of [A-Za-z0-9_-].
static bool calValidRequestId(const char* id) {
    const size_t len = strlen(id);
    if (!len || len > 32) return false;
    for (size_t i = 0; i < len; ++i) {
        const char c = id[i];
        if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_')) return false;
    }
    return true;
}

// From onMessage(). A payload that is not JSON, or has no valid request_id,
// still calibrates and is acknowledged with request_id "".
static void calQueueRequest(const byte* payload, unsigned int len) {
    char id[sizeof(calRequestId)] = {};
    JsonDocument doc;
    if (len <= 192 && !deserializeJson(doc, payload, len)) {
        const char* requested = doc["request_id"] | "";
        if (calValidRequestId(requested)) strcpy(id, requested);
    }
    if (calPending) {   // one queued request at a time
        Serial.printf("[CAL] request id=\"%s\" ignored: another is queued\n", id);
        return;
    }
    memcpy(calRequestId, id, sizeof(calRequestId));
    calPending = true;
    Serial.printf("[CAL] request id=\"%s\" queued\n", id);
}

static void calPublishAck(const char* requestId, bool ok, float medianCm,
                          int valid, float spreadCm, const char* reason) {
    JsonDocument doc;
    doc["node_id"]    = NODE_ID;
    doc["request_id"] = requestId;
    doc["ok"]         = ok;
    doc["depth_cm"]   = calRound(binDepthCm, 10.0f);   // the depth now in use
    if (isnan(medianCm)) doc["median_cm"] = nullptr;
    else                 doc["median_cm"] = calRound(medianCm, 100.0f);
    doc["n"]          = valid;
    doc["pings"]      = CAL_PINGS;
    if (isnan(spreadCm)) doc["spread_cm"] = nullptr;
    else                 doc["spread_cm"] = calRound(spreadCm, 100.0f);
    doc["reason"]     = reason;
    doc["ts"]         = millis();
    char buf[256];   // worst case 197 bytes (32-char id, longest reason)
    const size_t needed = measureJson(doc);
    const bool fits = !doc.overflowed() && needed < sizeof(buf) &&
                      serializeJson(doc, buf, sizeof(buf)) == needed;
    // The explicit (payload, length, retained=false) overload: QoS 0, not retained.
    const bool sent = fits && mqtt.connected() &&
        mqtt.publish(TOPIC_CALIBRATE_ACK, reinterpret_cast<const uint8_t*>(buf),
                     (unsigned int)needed, false);
    Serial.printf("[CAL] ack %s %s\n", sent ? "published" : "FAILED",
                  fits ? buf : "(does not fit)");
}

// Waits until gapMs after sinceMs with the broker and the INA219 serviced.
// A message that arrives here only sets a flag, exactly as in loop().
static void calServiceWait(uint32_t sinceMs, uint32_t gapMs) {
    while (millis() - sinceMs < gapMs) {
        if (mqtt.connected()) mqtt.loop();
        servicePowerMeter();
        delay(10);
    }
}

// Returns true when a new depth was measured, stored and applied.
bool runCalibration() {
    char requestId[sizeof(calRequestId)];
    memcpy(requestId, calRequestId, sizeof(requestId));
    calPending = false;   // a request arriving during the run queues the next one
    Serial.printf("[CAL] start id=\"%s\" depth=%.1f cm, %d pings over %lu ms\n",
                  requestId, binDepthCm, CAL_PINGS, (unsigned long)CAL_SPAN_MS);

    // One ping per slot. The slot is far above the HC-SR04's 60 ms minimum
    // gap, and the wait after the last ping keeps that gap before the
    // readSensors() that follows pings again.
    static_assert(CAL_SPAN_MS / CAL_PINGS >= 60, "HC-SR04 needs >= 60 ms between pings");
    static_assert(CAL_MIN_VALID >= 3 && CAL_MIN_VALID <= CAL_PINGS,
                  "the spread drops two echoes, so at least three are needed");
    float echoes[CAL_PINGS];
    int valid = 0;
    for (int i = 0; i < CAL_PINGS; ++i) {
        const uint32_t pingMs = millis();
        const float d = Ultrasonic::measureCm();   // -1 on timeout
        if (d > 0.0f) echoes[valid++] = d;
        calServiceWait(pingMs, CAL_SPAN_MS / CAL_PINGS);
    }

    // Ascending insertion sort, at most CAL_PINGS values.
    for (int i = 1; i < valid; ++i) {
        const float v = echoes[i];
        int j = i - 1;
        while (j >= 0 && echoes[j] > v) { echoes[j + 1] = echoes[j]; --j; }
        echoes[j + 1] = v;
    }
    const float medianCm = valid == 0 ? NAN
        : (valid % 2 ? echoes[valid / 2] : (echoes[valid / 2 - 1] + echoes[valid / 2]) / 2.0f);
    // Max - min after dropping the single lowest and the single highest echo.
    const float spreadCm = valid >= 3 ? echoes[valid - 2] - echoes[1] : NAN;

    const char* reason = "ok";
    float newDepthCm = NAN;
    if (valid < CAL_MIN_VALID)               reason = "too-few-echoes";
    else if (spreadCm > CAL_MAX_SPREAD_CM)   reason = "spread-too-large";
    else if (medianCm < CAL_MIN_MEDIAN_CM)   reason = "median-under-5cm";
    else {
        newDepthCm = roundf((medianCm - CAL_MARGIN_CM) * 10.0f) / 10.0f;   // 0.1 cm
        if (!calDepthValid(newDepthCm))      reason = "depth-out-of-range";
        else if (!calSaveDepth(newDepthCm))  reason = "save-failed";
    }
    const bool ok = strcmp(reason, "ok") == 0;
    const float oldDepthCm = binDepthCm;
    if (ok) {
        binDepthCm = newDepthCm;
        // The new depth moves every computed fill at once. Without this reset
        // the velocity window would read that step as a fill rate far above
        // DFDT_ALERT and raise a false fill-rate ALERT, and the deadband and
        // the dropout hold would compare against a fill on the old scale. The
        // new reference is the calibration median at the new depth, which is
        // 0 % by construction (the median sits CAL_MARGIN_CM beyond it).
        const float fill = Ultrasonic::fillFromCm(medianCm, binDepthCm);
        lastGoodFill   = fill;
        lastTxFill     = fill;
        fsm.prevFill   = fill;
        fsm.velRefFill = fill;
        fsm.velPrimed  = false;
        fsm.dFdtHr     = 0.0f;
    }
    Serial.printf("[CAL] result id=\"%s\" ok=%s reason=%s valid=%d/%d median=%.2f cm "
                  "spread=%.2f cm depth %.1f -> %.1f cm\n",
                  requestId, ok ? "true" : "false", reason, valid, CAL_PINGS,
                  medianCm, spreadCm, oldDepthCm, binDepthCm);
    calPublishAck(requestId, ok, medianCm, valid, spreadCm, reason);
    return ok;
}

// ── MQTT callbacks ────────────────────────────────────────────────────────
void onMessage(char* topic, byte* payload, unsigned int len) {
    if (strcmp(topic, TOPIC_OVERRIDE) == 0) { gatewayHandle(payload, len); return; }
    if (strcmp(topic, TOPIC_CALIBRATE) == 0) { calQueueRequest(payload, len); return; }
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
    ok &= mqtt.subscribe(TOPIC_CALIBRATE);
    Serial.printf("[MQTT] subscribed mode+override+battery+calibrate: %s\n", ok ? "ok" : "FAILED");
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
    doc["depth_cm"] = calRound(binDepthCm, 10.0f);   // the depth fill_pct used
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
    static uint16_t consecutiveFaults = 0;   // lastGoodFill is at file scope

    SensorReading r;
    float fill = Ultrasonic::readFillPct(binDepthCm);
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

    // Depth before anything computes a fill: the calibrated value in EEPROM,
    // or the compiled default when nothing valid is stored.
    const float storedDepthCm = calLoadDepth();
    if (calDepthValid(storedDepthCm)) {
        binDepthCm = storedDepthCm;
        Serial.printf("[CAL] depth %.1f cm from flash (compiled default %.1f cm)\n",
                      binDepthCm, (float)BIN_DEPTH_CM);
    } else {
        binDepthCm = BIN_DEPTH_CM;
        Serial.printf("[CAL] no valid stored depth; compiled default %.1f cm\n", binDepthCm);
    }

    Wire.begin(PIN_INA_SDA, PIN_INA_SCL);
    servicePowerMeter(true);

    connectWifi();

    // 896-byte JSON buffer + MQTT header (5), topic length (2), and topic.
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

    // "Calibrate empty" runs here, between ticks and outside the MQTT
    // callback. After a success this tick's reading, at the new depth, is
    // sent once as the "calibration" reading. A dead battery halts TX, so a
    // request then is dropped unanswered (the backend reports no ack) rather
    // than measured, stored and acked with no calibration reading to follow.
    // It is not kept for later: it would run a stale calibration on recovery.
    if (calPending && simulatedBattPct < BATT_DEAD) {
        calPending = false;
        Serial.printf("[CAL] request id=\"%s\" dropped: battery dead, TX halted\n", calRequestId);
    }
    const bool calibrated = calPending && runCalibration();

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
        // The calibration reading is one extra row. It leaves the fixed 60 s
        // schedule (lastTxMs) and the FSM alone.
        if (calibrated &&
            !(mqtt.connected() && publishReading(r, true, "calibration", "calibration", 0.0f, 0.0f)))
            Serial.println("[CAL] calibration reading not accepted by the MQTT client");
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
    // State carried by the last accepted ADAPTIVE transmission. Seeded from
    // the first evaluation, so booting does not add a transmission.
    static State reportedState = State::STABLE;
    static bool  reportedSeeded = false;
    float deadband = computeDeadband(fsm.dFdtHr), dFdt = fsm.dFdtHr;
    // No fill-based decision is possible before the first valid echo. Keep
    // telemetry null and the initial policy state until an actual measurement.
    bool urgent = !isnan(r.fillPct) &&
        fsmUpdate(fsm, r.fillPct, r.battPct, r.gasRaw135, r.gasRaw5, deadband, dFdt, now);

    bool stateChanged = (fsm.state != prevState);
    prevState = fsm.state;
    if (!reportedSeeded) { reportedState = fsm.state; reportedSeeded = true; }

    float fillDelta      = fabs(r.fillPct - lastTxFill);
    const uint32_t effectiveInterval = gatewayInterval(fsm.txInterval, now);
    bool timerFired      = (now - lastTxMs >= effectiveInterval);
    const bool urgentTransition = urgent && stateChanged;
    const bool deadbandReady = lastTxMs == 0 || now - lastTxMs >= DEADBAND_COOLDOWN_MS;
    bool deadbandCrossed = (fillDelta >= deadband) && deadbandReady;
    // Non-urgent state changes (into CONSERVE or ACTIVE, back to STABLE) used
    // to wait for the next timer or deadband, up to 30 min in STABLE, so the
    // dashboard showed a stale state. Report the new state once it differs
    // from the last one sent. ALERT is left to urgentTransition. Comparing
    // with the last sent state, not the previous tick, means a change during
    // the cooldown is still reported once the cooldown ends, and a flip that
    // returns to the sent state sends nothing. DEADBAND_COOLDOWN_MS caps a
    // threshold oscillation at one transmission per 10 s.
    const bool stateTransition = fsm.state != State::ALERT &&
                                 fsm.state != reportedState && deadbandReady;

    // The one reading after a successful calibration is sent whatever the
    // other triggers say, and is booked like any accepted transmission below.
    if (calibrated || urgentTransition || timerFired || deadbandCrossed || stateTransition) {
        const char* txTrigger;
        if (calibrated) {
            txTrigger = "calibration";
        } else if (urgentTransition) {
            txTrigger = "urgent-transition";
        } else if (timerFired) {
            txTrigger = effectiveInterval < fsm.txInterval ? "gateway-timer" : "timer";
        } else if (deadbandCrossed) {
            txTrigger = "deadband";
        } else {
            // Last, so this label counts only the transmissions this trigger added.
            txTrigger = "state-transition";
        }
        const char* stateCause;
        if (calibrated)                         stateCause = "calibration";
        else if (r.fillFaultSustained)          stateCause = "fill-sensor-fault";
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
            lastTxMs      = now;
            lastTxFill    = r.fillPct;
            reportedState = fsm.state;

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
