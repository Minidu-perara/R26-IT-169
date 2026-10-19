#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <PubSubClient.h>
#include <BoundedDHT.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "config.h"
#include "sensors.h"
#include "ina219_meter.h"
#include "fsm.h"

// ── RTC-persisted state (survives deep sleep) ─────────────────────────────
#define RTC_MAGIC 0xA1B2C3D4
RTC_DATA_ATTR uint32_t rtcMagic           = 0;
RTC_DATA_ATTR State    rtcState;
RTC_DATA_ATTR uint32_t rtcTxInterval;
RTC_DATA_ATTR float    rtcPrevFill;
RTC_DATA_ATTR float    rtcLastSentFill;
RTC_DATA_ATTR uint32_t rtcTxCount;
RTC_DATA_ATTR uint32_t rtcDroppedTxCount;
RTC_DATA_ATTR float    rtcSimulatedBattPct;

// Fill-velocity window (fsm.h). velRefMs is a millis() value, so it is stored
// for completeness only — see the restore block in setup() for why it is never
// restored as-is.
RTC_DATA_ATTR float    rtcVelRefFill;
RTC_DATA_ATTR uint32_t rtcVelRefMs;
RTC_DATA_ATTR bool     rtcVelPrimed;
RTC_DATA_ATTR float    rtcDFdtHr;

// Fast-reconnect cache. Storing the BSSID and channel lets WiFi.begin() skip
// the full channel scan; storing which AP in WIFI_APS[] they belonged to is
// what makes that safe once there is more than one known network -- without it
// the node would replay a cached BSSID with the wrong passphrase.
// NOTE: RTC slow memory survives deep sleep and a software reset. It does NOT
// survive removing power or pulling EN, so a cold power-cycle legitimately
// takes the scanning path.
RTC_DATA_ATTR uint8_t  rtcBssid[6];
RTC_DATA_ATTR int32_t  rtcChannel;
RTC_DATA_ATTR uint8_t  rtcApIndex         = 0;
RTC_DATA_ATTR bool     rtcWifiCached      = false;

// ── Globals ───────────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
WiFiMulti    wifiMulti;

// Known networks, in priority order. Credentials come from include/secrets.h.
struct WifiAP { const char* ssid; const char* pass; };
static const WifiAP WIFI_APS[] = {
    { WIFI_AP1_SSID, WIFI_AP1_PASS },   // 1. home router
    { WIFI_AP2_SSID, WIFI_AP2_PASS },   // 2. phone hotspot
};
static const uint8_t WIFI_AP_COUNT = sizeof(WIFI_APS) / sizeof(WIFI_APS[0]);
DHT          dht(PIN_DHT, DHT22);
Ina219Meter  powerMeter;

bool         powerMeterOnline    = false;
uint32_t     lastPowerSampleMs   = 0;
uint32_t     lastPowerProbeMs    = 0;
char         powerSession[16]    = {};

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
float    lastTxFill           = 0.0f;  // global so RTC can persist it
// Runtime bin depth: the calibrated depth from NVS, or the compiled
// BIN_DEPTH_CM default when nothing valid is stored. Every fill uses it.
float    binDepthCm           = BIN_DEPTH_CM;
// Last valid fill, held by readSensors() through an ultrasonic dropout. File
// scope so a calibration can reset it with the rest of the fill history.
float    lastGoodFill         = 0.0f;
// "Calibrate empty": onMessage() only records the request, loop() runs it.
bool     calPending           = false;
char     calRequestId[33]     = {};

#include "gateway_control.h"

// Sample the INA219 at 10 Hz during cooperative waits. This is still an
// on-node estimate rather than laboratory instrumentation, but it captures
// short Wi-Fi activity far better than sampling only with the 1 s sensor loop.
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

// ── Wi-Fi ─────────────────────────────────────────────────────────────────
// Two association paths, and the log names which one ran plus how long it took,
// because the energy claim for this node rests on the cached path being
// measurably shorter -- radio-on time during association is the cost being
// avoided, so it has to be reported as a number rather than asserted.
//
//   CACHED : direct WiFi.begin() to the remembered BSSID/channel of the AP that
//            worked last time. No scan. Bounded by WIFI_FAST_TIMEOUT_MS.
//   SCAN   : wifiMulti.run(), which scans every channel and picks a known SSID.
//            Used on a cold boot, when the cache is stale, or when the
//            remembered AP is gone.
//
// wifiMulti.run() selects among the known networks by RSSI, not by the order of
// WIFI_APS[]; that order is the priority the cache and the logs use. The two
// networks are rarely up at once, so this only matters if the hotspot is on
// next to the router.

static int8_t wifiApIndexForSsid(const char* ssid) {
    for (uint8_t i = 0; i < WIFI_AP_COUNT; ++i)
        if (strcmp(ssid, WIFI_APS[i].ssid) == 0) return (int8_t)i;
    return -1;
}

// Polls at 50 ms, not 500 ms: association time is the measurement here, so the
// sampling granularity has to be small next to the difference being claimed.
static bool wifiWaitAssoc(uint32_t timeoutMs) {
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 >= timeoutMs) return false;
        meteredDelay(50);
    }
    return true;
}

static void wifiCacheCurrent(uint8_t apIndex) {
    memcpy(rtcBssid, WiFi.BSSID(), 6);
    rtcChannel    = WiFi.channel();
    rtcApIndex    = apIndex;
    rtcWifiCached = true;
}

static void wifiLogSuccess(const char* path, uint32_t assocMs) {
    const uint8_t* b = WiFi.BSSID();
    Serial.printf("[WiFi] path=%s ssid=\"%s\" assoc_ms=%lu ch=%d rssi=%d "
                  "bssid=%02X:%02X:%02X:%02X:%02X:%02X ip=%s\n",
                  path, WiFi.SSID().c_str(), (unsigned long)assocMs,
                  WiFi.channel(), WiFi.RSSI(),
                  b[0], b[1], b[2], b[3], b[4], b[5],
                  WiFi.localIP().toString().c_str());
}

void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);

    // -- Fast path: the AP that worked last time, by BSSID and channel -----
    // rtcMagic gates this the same way it gates the FSM restore: a cache left
    // by a different firmware image is not trustworthy. rtcApIndex is bounds-
    // checked because WIFI_APS[] can shrink between builds while RTC memory
    // still holds an index from the longer list.
    if (rtcWifiCached && rtcMagic == RTC_MAGIC && rtcApIndex < WIFI_AP_COUNT) {
        const WifiAP& ap = WIFI_APS[rtcApIndex];
        Serial.printf("[WiFi] trying cached AP \"%s\" ch=%ld "
                      "bssid=%02X:%02X:%02X:%02X:%02X:%02X (no scan)\n",
                      ap.ssid, (long)rtcChannel,
                      rtcBssid[0], rtcBssid[1], rtcBssid[2],
                      rtcBssid[3], rtcBssid[4], rtcBssid[5]);
        uint32_t t0 = millis();
        WiFi.begin(ap.ssid, ap.pass, rtcChannel, rtcBssid, true);
        if (wifiWaitAssoc(WIFI_FAST_TIMEOUT_MS)) {
            uint32_t assocMs = millis() - t0;
            wifiCacheCurrent(rtcApIndex);
            wifiLogSuccess("CACHED", assocMs);
            return;
        }
        Serial.printf("[WiFi] cached AP failed after %lu ms - falling back to scan\n",
                      (unsigned long)(millis() - t0));
        WiFi.disconnect(true, true);
        rtcWifiCached = false;
        meteredDelay(100);
    } else {
        Serial.println("[WiFi] no usable cache - scanning");
    }

    // -- Fallback: full scan across every known network ---------------------
    static bool multiInit = false;
    if (!multiInit) {
        for (uint8_t i = 0; i < WIFI_AP_COUNT; ++i)
            wifiMulti.addAP(WIFI_APS[i].ssid, WIFI_APS[i].pass);
        multiInit = true;
    }

    uint32_t t0 = millis();
    while (wifiMulti.run(WIFI_SCAN_TIMEOUT_MS) != WL_CONNECTED) {
        Serial.printf("[WiFi] no known AP after %lu ms - retrying scan\n",
                      (unsigned long)(millis() - t0));
        meteredDelay(500);
    }
    uint32_t assocMs = millis() - t0;

    int8_t idx = wifiApIndexForSsid(WiFi.SSID().c_str());
    if (idx >= 0) {
        wifiCacheCurrent((uint8_t)idx);
    } else {
        // Associated to something not in WIFI_APS[] -- should not happen via
        // WiFiMulti. Never cache an index that cannot be mapped back to a
        // passphrase for the fast path.
        rtcWifiCached = false;
        Serial.printf("[WiFi] SSID \"%s\" not in AP list - not caching\n",
                      WiFi.SSID().c_str());
    }
    wifiLogSuccess("SCAN", assocMs);
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

// NAN when nothing is stored (fresh NVS, or never calibrated).
static float calLoadDepth() {
    Preferences prefs;
    float depthCm = NAN;
    if (prefs.begin(CAL_PREFS_NS, true)) {
        depthCm = prefs.getFloat(CAL_PREFS_KEY, NAN);
        prefs.end();
    }
    return depthCm;
}

// Written, then read back, so success means the depth is in flash.
static bool calSaveDepth(float depthCm) {
    Preferences prefs;
    if (!prefs.begin(CAL_PREFS_NS, false)) return false;
    const bool written = prefs.putFloat(CAL_PREFS_KEY, depthCm) == sizeof(float);
    prefs.end();
    return written && calLoadDepth() == depthCm;
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

void persistTransmissionState();   // defined with the RTC state, below

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
        // RTC takes the reset history now, not only once the calibration
        // reading is accepted, so a reset never pairs the new depth in NVS
        // with a fill history on the old scale.
        persistTransmissionState();
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
    doc["gas_135"]  = r.gasRaw135;
    doc["gas_5"]    = nullptr; // deliberately removed after unstable readings
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

// Persist after the caller has updated lastTxFill and the per-transmission AIMD step.
void persistTransmissionState() {
    rtcState            = fsm.state;
    rtcTxInterval       = fsm.txInterval;
    rtcPrevFill         = fsm.prevFill;
    rtcLastSentFill     = lastTxFill;
    rtcTxCount          = txCount;
    rtcDroppedTxCount   = droppedTxCount;
    rtcSimulatedBattPct = simulatedBattPct;
    rtcVelRefFill       = fsm.velRefFill;
    rtcVelRefMs         = fsm.velRefMs;
    rtcVelPrimed        = fsm.velPrimed;
    rtcDFdtHr           = fsm.dFdtHr;
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
        consecutiveFaults++;
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
    r.gasRaw135 = analogRead(PIN_MQ135);
    r.gasRaw5   = 0; // disabled input; cannot trigger the MQ5 urgency branch
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
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    snprintf(powerSession, sizeof(powerSession), "%08lx", (unsigned long)esp_random());
    Serial.printf("[POWER] boot session=%s; live-connected 10 Hz integration\n", powerSession);
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
    dht.begin();

    // Depth before anything computes a fill: the calibrated value in NVS, or
    // the compiled default when nothing valid is stored.
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

    // ── Restore FSM state from RTC if magic matches ───────────────────────
    if (rtcMagic == RTC_MAGIC) {
        fsm.state        = rtcState;
        fsm.txInterval   = rtcTxInterval;
        fsm.prevFill     = rtcPrevFill;
        lastTxFill       = rtcLastSentFill;
        txCount          = rtcTxCount;
        droppedTxCount   = rtcDroppedTxCount;
        simulatedBattPct = rtcSimulatedBattPct;

        // Fill-velocity window. velRefMs is a millis() timestamp and millis()
        // restarts at 0 on wake, so a restored velRefMs would be compared
        // against a smaller "now" and the elapsed subtraction would wrap to a
        // ~49-day interval. Re-anchor the window to the current millis() and
        // leave it unprimed: the first tick after wake re-primes from the live
        // fill reading and reports 0 %/hr, which is the honest answer for a
        // rate that was never measured across the sleep gap.
        fsm.velRefFill = rtcVelRefFill;
        fsm.dFdtHr     = rtcDFdtHr;
        fsm.velRefMs   = millis();
        fsm.velPrimed  = false;

        Serial.println("[RTC] state restored from RTC memory");
    } else {
        rtcMagic = RTC_MAGIC;
        Serial.println("[RTC] fresh boot, initializing");
    }

    // 896-byte JSON buffer + MQTT header (5), topic length (2), and topic.
    // The old 256-byte default silently rejected payloads above 235 bytes.
    static_assert(896 + 5 + 2 + sizeof(TOPIC_DATA) - 1 <= 1024,
                  "MQTT buffer must fit the complete data packet");
    if (!mqtt.setBufferSize(1024))
        Serial.println("[MQTT] ERROR allocating 1024-byte packet buffer");
    Serial.printf("[BUILD] INA219 present, MQ5 removed; MQTT buffer=%u; bounded DHT\n",
                  (unsigned)mqtt.getBufferSize());
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMessage);

    // Bounded wait. An edge node must run its control loop whether or not the
    // broker answered, so this gives up after MQTT_SETUP_TIMEOUT_MS and
    // continues to MQ warmup and FSM start regardless.
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

    Serial.println("[MQ] warming up...");
    unsigned long warmStart = millis();
    while (millis() - warmStart < MQ_WARMUP_MS) {
        if (mqtt.connected()) mqtt.loop();
        meteredDelay(1000);
        const uint32_t elapsed = millis() - warmStart;
        Serial.printf("[MQ] warmup %lus remaining\n",
            elapsed < MQ_WARMUP_MS ? (MQ_WARMUP_MS - elapsed) / 1000 : 0UL);
    }
    Serial.println("[MQ] warmup complete");

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
    // A request during the MQ warm-up in setup() waits for it, so its ack can
    // miss the backend's 10 s wait (504; the late ack is still logged).
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
        if (calibrated) {
            if (mqtt.connected() && publishReading(r, true, "calibration", "calibration", 0.0f, 0.0f))
                persistTransmissionState();
            else
                Serial.println("[CAL] calibration reading not accepted by the MQTT client");
        }
        if (now - lastTxMs >= BASELINE_INTERVAL) {
            if (mqtt.connected() && publishReading(r, true, "baseline-timer", "baseline-fixed-policy", 0.0f, 0.0f)) {
                lastTxMs = now;
                persistTransmissionState();
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
    float deadband, dFdt;
    bool urgent = fsmUpdate(fsm, r.fillPct, r.battPct, r.gasRaw135, r.gasRaw5, deadband, dFdt, now);

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
        if (calibrated)                              stateCause = "calibration";
        else if (r.fillFaultSustained)               stateCause = "fill-sensor-fault";
        else if (fsm.state == State::CONSERVE)       stateCause = "low-battery";
        else if (fsm.state == State::ALERT && r.fillPct >= FILL_ALERT)
                                                     stateCause = "fill-alert";
        else if (fsm.state == State::ALERT && r.gasRaw135 > GAS135_THRESHOLD)
                                                     stateCause = "gas135-alert";
        else if (fsm.state == State::ALERT && r.gasRaw5 > GAS5_THRESHOLD)
                                                     stateCause = "gas5-alert";
        else if (fsm.state == State::ALERT)           stateCause = "fill-rate-alert";
        else if (fsm.state == State::ACTIVE)          stateCause = "active-fill";
        else                                          stateCause = "stable";
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
                          fsm.txInterval / 1000,
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
            persistTransmissionState();
        }
    }

#if DEEP_SLEEP_ENABLED
    uint32_t sleepMs = min(fsm.txInterval, (uint32_t)30000);
    if (fsm.state == State::ALERT) sleepMs = 10000;
    Serial.printf("[SLEEP] %lu ms\n", sleepMs);
    esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);
    esp_deep_sleep_start();
#else
    meteredDelay(1000);
#endif
}
