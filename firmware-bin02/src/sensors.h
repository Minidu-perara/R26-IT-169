#pragma once
#include <Arduino.h>

struct SensorReading {
    float fillPct;    // 0–100 % — last good value while fillFault is set
    float tempC;
    float humidity;
    int   gasRaw135;
    int   gasRaw5;
    float battPct;    // 0–100 % (simulated via MQTT; not physical battery measurement)
    bool  powerPresent;
    float busVoltageV;
    float currentMa;
    float powerMw;
    float energyMWh;
    uint32_t powerMeasurementMs;

    // Ultrasonic dropout tracking. fillPct is HELD (not zeroed) while faulting,
    // so a dropout can never look like the bin emptying. See readSensors().
    bool  fillFault;          // true while the fault counter is above zero
    bool  fillFaultSustained; // true once the counter exceeds FILL_FAULT_HOLD_MAX
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

    // Fill % for one distance at the given bin depth, clamped to [0, 100].
    // Distance shrinks as the bin fills.
    static float fillFromCm(float distanceCm, float depthCm) {
        float fill = ((depthCm - distanceCm) / (depthCm - FILL_MAX_CM)) * 100.0f;
        return constrain(fill, 0.0f, 100.0f);
    }

    // Returns fill % using median of 3 raw distance readings, at depthCm (the
    // runtime depth: calibrated, or the compiled BIN_DEPTH_CM default).
    // Returns -1 on sensor fault (fewer than two valid echoes).
    float readFillPct(float depthCm) {
        float s[3];
        for (int i = 0; i < 3; i++) {
            s[i] = measureCm();
            delay(60); // HC-SR04 needs >=60ms between triggers or the next
                       // trigger catches the previous burst's residual echo
                       // and reports a spuriously short distance (ringing)
        }
        // sort ascending (inline bubble, only 3 elements)
        for (int i = 0; i < 2; i++)
            for (int j = i + 1; j < 3; j++)
                if (s[i] > s[j]) { float t = s[i]; s[i] = s[j]; s[j] = t; }
        float median = s[1];
        if (median < 0) return -1.0f;
        return fillFromCm(median, depthCm);
    }
}
