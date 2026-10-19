#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "config.h"
#include "sensors.h"
#include "fsm.h"

// ── RTC-persisted state (survives deep sleep) ─────────────────────────────
#define RTC_MAGIC 0xA1B2C3D4
RTC_DATA_ATTR uint32_t rtcMagic           = 0;
RTC_DATA_ATTR State    rtcState;
RTC_DATA_ATTR uint32_t rtcTxInterval;
RTC_DATA_ATTR float    rtcPrevFill;
RTC_DATA_ATTR float    rtcLastSentFill;
RTC_DATA_ATTR uint32_t rtcTxCount;
RTC_DATA_ATTR float    rtcSimulatedBattPct;

RTC_DATA_ATTR uint8_t  rtcBssid[6];
RTC_DATA_ATTR int32_t  rtcChannel;
RTC_DATA_ATTR bool     rtcWifiCached      = false;

// ── Globals ───────────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
DHT          dht(PIN_DHT, DHT22);

FSMContext   fsm;
bool         baselineMode     = false;

uint32_t txCount              = 0;    // resets on mode switch; dashboard filters by mode field
uint32_t lastTxMs             = 0;
bool     batteryDead          = false;
float    simulatedBattPct     = 80.0f; // updated via MQTT on bin/{id}/battery
float    lastTxFill           = 0.0f;  // global so RTC can persist it

// ── Wi-Fi ─────────────────────────────────────────────────────────────────
void connectWifi() {
    Serial.printf("[WiFi] connecting to %s", WIFI_SSID);
    if (rtcWifiCached && rtcMagic == RTC_MAGIC)
        WiFi.begin(WIFI_SSID, WIFI_PASS, rtcChannel, rtcBssid, true);
    else
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    memcpy(rtcBssid, WiFi.BSSID(), 6);
    rtcChannel    = WiFi.channel();
    rtcWifiCached = true;
    Serial.printf("\n[WiFi] connected, IP=%s\n", WiFi.localIP().toString().c_str());
}

// ── MQTT callbacks ────────────────────────────────────────────────────────
void onMessage(char* topic, byte* payload, unsigned int len) {
    char buf[64] = {};
    memcpy(buf, payload, min(len, (unsigned int)63));

    if (strcmp(topic, TOPIC_MODE) == 0) {
        bool wasBaseline = baselineMode;
        baselineMode = (strncmp(buf, "BASELINE", 8) == 0);
        if (baselineMode != wasBaseline) {
            txCount  = 0;   // reset counter on every mode switch
            lastTxMs = millis();
        }
        Serial.printf("[MQTT] mode -> %s (txCount reset)\n", baselineMode ? "BASELINE" : "ADAPTIVE");
    } else if (strcmp(topic, TOPIC_BATTERY) == 0) {
        simulatedBattPct = atof(buf);
        Serial.printf("[MQTT] battery -> %.1f%%\n", simulatedBattPct);
    }
    // TOPIC_OVERRIDE: "ALERT" forces an immediate publish on next loop tick
    // (handled implicitly — any state override would need more fields; skipped for PP1)
}

void connectMqtt() {
    while (!mqtt.connected()) {
        Serial.print("[MQTT] connecting...");
        if (mqtt.connect(NODE_ID)) {
            Serial.println(" ok");
            mqtt.subscribe(TOPIC_MODE);
            mqtt.subscribe(TOPIC_OVERRIDE);
            mqtt.subscribe(TOPIC_BATTERY);
        } else {
            Serial.printf(" failed rc=%d, retry in 5s\n", mqtt.state());
            delay(5000);
        }
    }
}

// ── Publish ───────────────────────────────────────────────────────────────
void publishReading(const SensorReading& r, bool isBaseline,
                    const char* reason, float deadband, float dFdt) {
    JsonDocument doc;
    doc["node_id"]  = NODE_ID;
    doc["ts"]       = millis();
    doc["fill_pct"] = r.fillPct;
    doc["temp_c"]   = r.tempC;
    doc["humidity"] = r.humidity;
    doc["gas_135"]  = r.gasRaw135;
    doc["gas_5"]    = r.gasRaw5;
    doc["batt_pct"] = r.battPct;
    doc["state"]    = stateName(fsm.state);
    doc["mode"]     = isBaseline ? "BASELINE" : "ADAPTIVE";
    doc["tx_count"] = txCount;
    doc["interval"] = fsm.txInterval / 1000;
    doc["deadband"] = deadband;
    doc["dfdt"]     = dFdt * 3600.0f;
    doc["reason"]   = reason;

    char buf[320];
    size_t n = serializeJson(doc, buf);
    mqtt.publish(TOPIC_DATA, buf, n);
    Serial.printf("[TX] %s\n", buf);

    // ── Persist FSM state to RTC after every TX ───────────────────────────
    rtcState            = fsm.state;
    rtcTxInterval       = fsm.txInterval;
    rtcPrevFill         = fsm.prevFill;
    rtcLastSentFill     = lastTxFill;
    rtcTxCount          = txCount;
    rtcSimulatedBattPct = simulatedBattPct;
}

// ── Sensor read ───────────────────────────────────────────────────────────
SensorReading readSensors() {
    SensorReading r;
    r.fillPct  = Ultrasonic::readFillPct();
    if (r.fillPct < 0) r.fillPct = 0;   // fault → treat as empty, don't false-alert
    r.tempC    = dht.readTemperature();
    r.humidity = dht.readHumidity();
    r.gasRaw135 = analogRead(PIN_MQ135);
    r.gasRaw5   = analogRead(PIN_MQ5);
    r.battPct   = simulatedBattPct;
    return r;
}

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
    dht.begin();

    connectWifi();

    // ── Restore FSM state from RTC if magic matches ───────────────────────
    if (rtcMagic == RTC_MAGIC) {
        fsm.state        = rtcState;
        fsm.txInterval   = rtcTxInterval;
        fsm.prevFill     = rtcPrevFill;
        lastTxFill       = rtcLastSentFill;
        txCount          = rtcTxCount;
        simulatedBattPct = rtcSimulatedBattPct;
        Serial.println("[RTC] state restored from RTC memory");
    } else {
        rtcMagic = RTC_MAGIC;
        Serial.println("[RTC] fresh boot, initializing");
    }

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMessage);
    connectMqtt();

    Serial.println("[MQ] warming up...");
    unsigned long warmStart = millis();
    while (millis() - warmStart < MQ_WARMUP_MS) {
        mqtt.loop();
        delay(1000);
        Serial.printf("[MQ] warmup %lus remaining\n",
            (MQ_WARMUP_MS - (millis() - warmStart)) / 1000);
    }
    Serial.println("[MQ] warmup complete");

    lastTxMs = millis();
    Serial.println("[FSM] started in ADAPTIVE mode");
}

// ── Loop ──────────────────────────────────────────────────────────────────
void loop() {
    if (!mqtt.connected()) connectMqtt();
    mqtt.loop();

    uint32_t now = millis();
    SensorReading r = readSensors();

    // ── Dead battery guard (runs before everything, including BASELINE) ───
    if (r.battPct < BATT_DEAD) {
        if (!batteryDead) {
            batteryDead = true;
            JsonDocument doc;
            doc["node_id"]  = NODE_ID;
            doc["ts"]       = now;
            doc["state"]    = "DEAD";
            doc["batt_pct"] = r.battPct;
            doc["mode"]     = baselineMode ? "BASELINE" : "ADAPTIVE";
            char buf[128];
            size_t n = serializeJson(doc, buf);
            mqtt.publish(TOPIC_DATA, buf, n);
            Serial.println("[BATT] dead battery — TX halted");
        }
        delay(5000);
        return;
    }

    // ── BASELINE branch ───────────────────────────────────────────────────
    if (baselineMode) {
        if (now - lastTxMs >= BASELINE_INTERVAL) {
            txCount++;
            publishReading(r, true, "baseline", 0.0f, 0.0f);
            lastTxMs = now;
        }
        delay(1000);
        return;
    }

    // ── ADAPTIVE branch ───────────────────────────────────────────────────
    static State prevState = State::STABLE;
    float deadband, dFdt;
    bool urgent = fsmUpdate(fsm, r.fillPct, r.battPct, r.gasRaw135, r.gasRaw5, deadband, dFdt, now);

    bool stateChanged = (fsm.state != prevState);
    prevState = fsm.state;

    float fillDelta      = fabs(r.fillPct - lastTxFill);
    bool timerFired      = (now - lastTxMs >= fsm.txInterval);
    bool deadbandCrossed = (fillDelta >= deadband);

    if ((urgent && stateChanged) || timerFired || deadbandCrossed) {
        const char* reason;
        if (urgent) {
            float dFdt_hr = dFdt * 3600.0f;
            if (r.fillPct >= FILL_ALERT)              reason = "urgency-fill";
            else if (r.gasRaw135 > GAS135_THRESHOLD)  reason = "urgency-gas135";
            else if (r.gasRaw5   > GAS5_THRESHOLD)    reason = "urgency-gas5";
            else                                      reason = "urgency-rate";
        } else if (timerFired) {
            reason = "timer";
        } else {
            reason = "deadband";
        }
        txCount++;
        publishReading(r, false, reason, deadband, dFdt);
        lastTxMs   = now;
        lastTxFill = r.fillPct;

        Serial.printf("[FSM] state=%s interval=%lus deadband=%.1f%% delta=%.1f%% reason=%s\n",
                      stateName(fsm.state),
                      fsm.txInterval / 1000,
                      deadband,
                      fillDelta,
                      reason);
    }

#if DEEP_SLEEP_ENABLED
    uint32_t sleepMs = min(fsm.txInterval, (uint32_t)30000);
    if (fsm.state == State::ALERT) sleepMs = 10000;
    Serial.printf("[SLEEP] %lu ms\n", sleepMs);
    esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);
    esp_deep_sleep_start();
#else
    delay(1000);
#endif
}
