#pragma once

#include <Arduino.h>
#include <Wire.h>

// Minimal INA219 driver for the common breakout fitted with an R100 (0.1 ohm)
// shunt. The calibration is the standard 32 V / 2 A profile:
//   current LSB = 100 uA, current register / 10 = mA,
//   power LSB = 2 mW.
// Keeping this local avoids adding a network dependency immediately before the
// experiment. Confirm the breakout resistor is marked R100 before using these
// current and power values as research evidence.
class Ina219Meter {
public:
    bool begin(uint8_t address = 0x40) {
        address_ = address;
        present_ = writeRegister(REG_CALIBRATION, CALIBRATION_32V_2A) &&
                   writeRegister(REG_CONFIG, CONFIG_32V_2A_CONTINUOUS);
        if (!present_) return false;
        uint16_t config = 0;
        present_ = readRegister(REG_CONFIG, config) && config == CONFIG_32V_2A_CONTINUOUS;
        return present_;
    }

    bool sample() {
        if (!present_) return false;

        uint16_t busRaw = 0, currentRawBits = 0, powerRaw = 0;
        // The calibration register can be cleared by a brownout/reset on the
        // sensor. Rewriting it makes a silent zero-current failure visible only
        // when the I2C transaction itself fails.
        if (!writeRegister(REG_CALIBRATION, CALIBRATION_32V_2A) ||
            !readRegister(REG_BUS_VOLTAGE, busRaw) ||
            !readRegister(REG_CURRENT, currentRawBits) ||
            !readRegister(REG_POWER, powerRaw)) {
            present_ = false;
            return false;
        }

        const float busV = static_cast<float>((busRaw >> 3) * 4) / 1000.0f;
        const float currentMa = static_cast<float>(static_cast<int16_t>(currentRawBits)) / 10.0f;
        const float powerMw = static_cast<float>(powerRaw) * 2.0f;
        const uint32_t now = millis();

        if (hasSample_) {
            const uint32_t elapsedMs = now - lastSampleMs_;
            // Trapezoidal integration. Sampling at 10 Hz during cooperative
            // waits captures far more radio activity than the 1 s sensor loop.
            energyMWh_ += ((lastPowerMw_ + powerMw) * 0.5f) *
                          (static_cast<float>(elapsedMs) / 3600000.0f);
        }

        busVoltageV_ = busV;
        currentMa_ = currentMa;
        powerMw_ = powerMw;
        lastPowerMw_ = powerMw;
        lastSampleMs_ = now;
        hasSample_ = true;
        return true;
    }

    bool present() const { return present_ && hasSample_; }
    float busVoltageV() const { return busVoltageV_; }
    float currentMa() const { return currentMa_; }
    float powerMw() const { return powerMw_; }
    float energyMWh() const { return energyMWh_; }
    uint32_t measurementMs() const { return hasSample_ ? lastSampleMs_ : 0; }

private:
    static constexpr uint8_t REG_CONFIG = 0x00;
    static constexpr uint8_t REG_BUS_VOLTAGE = 0x02;
    static constexpr uint8_t REG_POWER = 0x03;
    static constexpr uint8_t REG_CURRENT = 0x04;
    static constexpr uint8_t REG_CALIBRATION = 0x05;
    static constexpr uint16_t CALIBRATION_32V_2A = 4096;
    static constexpr uint16_t CONFIG_32V_2A_CONTINUOUS = 0x399F;

    bool writeRegister(uint8_t reg, uint16_t value) {
        Wire.beginTransmission(address_);
        Wire.write(reg);
        Wire.write(static_cast<uint8_t>(value >> 8));
        Wire.write(static_cast<uint8_t>(value & 0xFF));
        return Wire.endTransmission() == 0;
    }

    bool readRegister(uint8_t reg, uint16_t& value) {
        Wire.beginTransmission(address_);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0) return false;
        if (Wire.requestFrom(static_cast<int>(address_), 2) != 2) return false;
        value = (static_cast<uint16_t>(Wire.read()) << 8) | Wire.read();
        return true;
    }

    uint8_t address_ = 0x40;
    bool present_ = false;
    bool hasSample_ = false;
    uint32_t lastSampleMs_ = 0;
    float busVoltageV_ = NAN;
    float currentMa_ = NAN;
    float powerMw_ = NAN;
    float lastPowerMw_ = 0.0f;
    float energyMWh_ = 0.0f;
};
