#pragma once
#include <Arduino.h>
#include "config.h"

// ── State definitions ─────────────────────────────────────────────────────
enum class State : uint8_t {
    STABLE   = 0,
    ACTIVE   = 1,
    ALERT    = 2,
    CONSERVE = 3,
};

const char* stateName(State s) {
    switch (s) {
        case State::STABLE:   return "STABLE";
        case State::ACTIVE:   return "ACTIVE";
        case State::ALERT:    return "ALERT";
        case State::CONSERVE: return "CONSERVE";
    }
    return "UNKNOWN";
}

// ── FSM context ───────────────────────────────────────────────────────────
struct FSMContext {
    State    state        = State::STABLE;
    uint32_t txInterval   = AIMD_MIN;    // T* starts at 300 s
    float    prevFill     = 0.0f;
    uint32_t lastSampleMs = 0;
};

// ── Dynamic deadband ΔF* = max(1%, 10% - dF/dt × 0.5) ───────────────────
// dFdt in %/s, returns deadband in %
static float computeDeadband(float dFdt) {
    float dFdt_hr = dFdt * 3600.0f;
    float db = 10.0f - dFdt_hr * 0.5f;
    return max(1.0f, db);
}

// ── FSM transition ────────────────────────────────────────────────────────
// Returns true when a TX should be sent this cycle.
// Priority: urgency check FIRST, then battery, then activity, then default.
bool fsmUpdate(FSMContext& ctx, float fillPct, float battPct,
               int gasRaw135, int gasRaw5,
               float& deadbandOut, float& dFdtOut, uint32_t nowMs) {

    uint32_t dt     = nowMs - ctx.lastSampleMs;
    float    dFdt   = (dt > 0) ? ((fillPct - ctx.prevFill) / (dt / 1000.0f)) : 0.0f;
    dFdtOut         = dFdt;
    deadbandOut     = computeDeadband(dFdt);
    bool     urgent = false;

    // ── 1. Urgency check (ALWAYS runs first) ─────────────────────────────
    if (fillPct >= FILL_ALERT
        || gasRaw135 > GAS135_THRESHOLD
        || gasRaw5   > GAS5_THRESHOLD
        || (dFdt * 3600.0f) > DFDT_ALERT) {
        ctx.state      = State::ALERT;
        ctx.txInterval = AoI_ALERT;
        urgent         = true;
    }
    // ── 2. Battery check ─────────────────────────────────────────────────
    else if (battPct <= BATT_LOW) {
        ctx.state      = State::CONSERVE;
        ctx.txInterval = AoI_CONSERVE;
    }
    // ── 3. Activity check ────────────────────────────────────────────────
    else if (fillPct >= FILL_ACTIVE) {
        ctx.state      = State::ACTIVE;
        ctx.txInterval = AoI_ACTIVE;
    }
    // ── 4. Default: STABLE with battery-driven AIMD ──────────────────────
    else {
        if (ctx.state != State::STABLE) ctx.txInterval = AIMD_MIN;
        ctx.state = State::STABLE;
        if (battPct > BATT_AIMD_MD_THRESH) {
            ctx.txInterval = (uint32_t)(ctx.txInterval * AIMD_MD);
            ctx.txInterval = max((uint32_t)ctx.txInterval, (uint32_t)AIMD_MIN);
        } else if (battPct < BATT_AIMD_AI_THRESH) {
            ctx.txInterval = min((uint32_t)(ctx.txInterval + AIMD_AI), (uint32_t)AIMD_MAX);
        }
        // 70% ≤ batt ≤ 80%: hysteresis band, keep current interval
    }

    ctx.prevFill     = fillPct;
    ctx.lastSampleMs = nowMs;

    return urgent;
}