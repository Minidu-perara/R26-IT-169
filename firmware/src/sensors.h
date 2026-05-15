#pragma once
#include <Arduino.h>

struct SensorReading {
    float fillPct;    // 0–100 %
    float tempC;
    float humidity;
    int   gasRaw135;
    int   gasRaw5;
    float battPct;    // 0–100 % (simulated via potentiometer)
};

// Median-of-3 filter for HC-SR04
namespace Ultrasonic {
    static float measureCm() {
        digitalWrite(PIN_TRIG, LOW);
        delayMicroseconds(2);
        digitalWrite(PIN_TRIG, HIGH);
        delayMicroseconds(10);
        digitalWrite(PIN_TRIG, LOW);
        long dur = pulseIn(PIN_ECHO, HIGH, 30000); // 30 ms timeout
        if (dur == 0) return -1.0f;
        return dur * 0.0343f / 2.0f;
    }

    // Returns fill % using median of 3 raw distance readings.
    // Returns -1 on sensor fault (all three timed out).
    float readFillPct() {
        float s[3];
        for (int i = 0; i < 3; i++) {
            s[i] = measureCm();
            delay(30);
        }
        // sort ascending (inline bubble, only 3 elements)
        for (int i = 0; i < 2; i++)
            for (int j = i + 1; j < 3; j++)
                if (s[i] > s[j]) { float t = s[i]; s[i] = s[j]; s[j] = t; }
        float median = s[1];
        if (median < 0) return -1.0f;
        // distance shrinks as bin fills; clamp result to [0, 100]
        float fill = ((BIN_DEPTH_CM - median) / (BIN_DEPTH_CM - FILL_MAX_CM)) * 100.0f;
        return constrain(fill, 0.0f, 100.0f);
    }
}
