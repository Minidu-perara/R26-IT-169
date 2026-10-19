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

    // Fill-velocity window. dF/dt is held between window closures so the
    // FSM sees a stable estimate rather than a per-tick difference.
    float    velRefFill   = 0.0f;
    uint32_t velRefMs     = 0;
    bool     velPrimed    = false;
    float    dFdtHr       = 0.0f;        // %/hr, the value the FSM acts on
};

// Dynamic deadband uses filling only. Emptying/collection produces a negative
// rate and must not inflate the threshold above its intended 10% ceiling.
static float computeDeadband(float dFdtHr) {
    const float positiveFillRate = max(0.0f, dFdtHr);
    return constrain(10.0f - positiveFillRate * 0.5f, 1.0f, 10.0f);
}

// ── Fill velocity over a bounded window ──────────────────────────────────
// Returns %/hr. Recomputed only once VEL_WINDOW_MS has elapsed; between
// closures the previous estimate is held. This is what stops HC-SR04
// quantisation and dropouts from being read as a 100,000 %/hr fill rate.
static void updateVelocity(FSMContext& ctx, float fillPct, uint32_t nowMs) {
    if (!ctx.velPrimed) {
        ctx.velRefFill = fillPct;
        ctx.velRefMs   = nowMs;
        ctx.velPrimed  = true;
        ctx.dFdtHr     = 0.0f;
        return;
    }
    uint32_t elapsed = nowMs - ctx.velRefMs;
    if (elapsed < VEL_WINDOW_MS) return;          // hold previous estimate

    float hours = elapsed / 3600000.0f;
    float v     = (fillPct - ctx.velRefFill) / hours;
    ctx.dFdtHr  = constrain(v, -DFDT_CLAMP, DFDT_CLAMP);

    ctx.velRefFill = fillPct;
    ctx.velRefMs   = nowMs;
}

// ── FSM transition ────────────────────────────────────────────────────────
// Returns true when the urgency condition is active this cycle.
// Priority: urgency check FIRST, then battery, then activity, then default.
// Thresholds are unchanged from the PP1 design; only the dF/dt estimate
// feeding the rate trigger and the timing of the AIMD step have changed.
bool fsmUpdate(FSMContext& ctx, float fillPct, float battPct,
               int gasRaw135, int gasRaw5,
               float& deadbandOut, float& dFdtOut, uint32_t nowMs) {

    updateVelocity(ctx, fillPct, nowMs);
    dFdtOut     = ctx.dFdtHr;
    deadbandOut = computeDeadband(ctx.dFdtHr);
    bool urgent = false;

    // ── 1. Urgency check (ALWAYS runs first) ─────────────────────────────
    if (fillPct >= FILL_ALERT
        || gasRaw135 > GAS135_THRESHOLD
        || gasRaw5   > GAS5_THRESHOLD
        || ctx.dFdtHr > DFDT_ALERT) {
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
    // ── 4. Default: STABLE. T* is left alone here; the AIMD step runs
    //      once per transmission in fsmOnTransmit(), not once per tick. ──
    else {
        if (ctx.state != State::STABLE) ctx.txInterval = AIMD_MIN;
        ctx.state = State::STABLE;
    }

    ctx.prevFill     = fillPct;
    ctx.lastSampleMs = nowMs;

    return urgent;
}

// ── AIMD step — called once per transmission, STABLE state only ──────────
// Running this per loop tick (the PP1 behaviour) halved T* every second
// while the battery was healthy, pinning it to AIMD_MIN and making the
// control law invisible in the data. AIMD is a per-cycle law, so it belongs
// on the transmission event.
void fsmOnTransmit(FSMContext& ctx, float battPct) {
    if (ctx.state != State::STABLE) return;

    if (battPct > BATT_AIMD_MD_THRESH) {
        uint32_t t = (uint32_t)(ctx.txInterval * AIMD_MD);
        ctx.txInterval = max(t, (uint32_t)AIMD_MIN);
    } else if (battPct < BATT_AIMD_AI_THRESH) {
        ctx.txInterval = min((uint32_t)(ctx.txInterval + AIMD_AI), (uint32_t)AIMD_MAX);
    }
    // 70% ≤ batt ≤ 80%: hysteresis band, hold current interval
}
