// bin01 hardware probe. Pins copied from firmware/include/config.h.
#include <Arduino.h>
#include <Wire.h>
#include <BoundedDHT.h>
#include "../../../firmware/src/ina219_meter.h"

#define PIN_TRIG 13
#define PIN_ECHO 14
#define PIN_DHT 4
#ifdef ESP8266
  // bin02 pins from node02-esp8266-firmware/include/config.h. No MQ-135.
  #define PIN_SDA 12
  #define PIN_SCL 5
#else
  #define PIN_MQ135 34
  #define PIN_SDA 21
  #define PIN_SCL 22
#endif

DHT dht(PIN_DHT, DHT22);
Ina219Meter ina;

static long ping() {
    digitalWrite(PIN_TRIG, LOW); delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
    return pulseIn(PIN_ECHO, HIGH, 30000UL);  // us, 0 = timeout
}

void setup() {
    Serial.begin(115200);
    delay(500);
    pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
    Wire.begin(PIN_SDA, PIN_SCL);
    dht.begin();
    Serial.println("\n[PROBE] start");
    Serial.printf("[PINS] TRIG=%d ECHO=%d DHT=%d SDA=%d SCL=%d\n",
                  PIN_TRIG, PIN_ECHO, PIN_DHT, PIN_SDA, PIN_SCL);
}

void loop() {
    // I2C scan
    Serial.print("[I2C] found:");
    int n = 0;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
    }
    Serial.println(n ? "" : " none");

    // INA219
    if (ina.begin(0x40) && ina.sample())
        Serial.printf("[INA219] bus=%.3f V current=%.1f mA\n", ina.busVoltageV(), ina.currentMa());
    else
        Serial.println("[INA219] not responding at 0x40");

    // DHT22
    float t = dht.readTemperature(), h = dht.readHumidity(true);
    if (isnan(t) || isnan(h)) Serial.println("[DHT22] read FAILED (NaN)");
    else Serial.printf("[DHT22] %.1f C  %.1f %%RH\n", t, h);

    // HC-SR04: 10 pings
    float cm[10]; int ok = 0, to = 0;
    for (int i = 0; i < 10; i++) {
        long us = ping();
        if (us == 0) to++; else cm[ok++] = us * 0.0343f / 2.0f;
        delay(60);
    }
    if (ok) {
        for (int i = 1; i < ok; i++) { float k = cm[i]; int j = i - 1; while (j >= 0 && cm[j] > k) { cm[j+1] = cm[j]; j--; } cm[j+1] = k; }
        Serial.printf("[HCSR04] min=%.1f med=%.1f max=%.1f cm timeouts=%d/10\n", cm[0], cm[ok/2], cm[ok-1], to);
    } else Serial.println("[HCSR04] all 10 pings TIMED OUT");

#ifndef ESP8266
    Serial.printf("[MQ135] raw=%d\n", analogRead(PIN_MQ135));
#endif
    yield();
    delay(1000);
}
