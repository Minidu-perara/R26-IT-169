# Claude Code task: closed-loop battery model with simulated solar input (R26-IT-169)

Read `smart-bin-pp1/CLAUDE.md`, `smart-bin-pp1/HANDOVER.md` and `smart-bin-pp1/HARDWARE.md` (verified 17 Oct) first.

**Scope:** firmware changes (Parts B and D) apply to **bin01 only**, the ESP32 in this repo. bin02 (ESP8266) runs separate firmware in `firmware-bin02/`; do not change or reflash it. The gateway model (Part A) only needs to run for bin01. Both boards share the same USB adapter serial, so identify bin01 by MAC `1c:c3:ab:fa:42:6c` before flashing. Flash with the same `-DMQTT_HOST_OVERRIDE=\"192.168.1.3\"` used on 17 Oct. Final viva is Tue 20 Oct 2026, so this change must be **additive and low-risk**. If anything in bin01's existing behaviour breaks, revert and stop.

## Goal

Today the FSM's battery input is a manual dashboard slider (`bin/{node_id}/battery`, held in `simulatedBattPct`). Replace the slider *as a source* (keep it working) with a **gateway-side battery model**:

    SoC(t) = SoC(t-1) + (η · P_solar(t) − P_load(t)) · Δt_virtual / C

- `P_load` = the node's **measured** power from INA219 telemetry (`energy_mwh`, `power_mw`, `power_session`), so drain comes from real data
- `P_solar` = a **deterministic simulated** solar curve
- The result is published on the existing `bin/{node_id}/battery` topic, so the firmware FSM consumes it exactly as it consumes the slider today

In Parts A–C, solar only drives the battery state. Part D then makes the CONSERVE rule (and only that rule) aware of net power, behind a runtime switch. All policy decisions stay inside `fsm.h`.

## How to run this prompt: four steps, ask the user after each one

This whole file is given at once, but you work through it in **four steps**. After each step:
1. Stop.
2. Post a short checkpoint report:
   - **Done:** what changed (files, one line each)
   - **Checked:** test or build results, plus the one thing the user should look at, from the "User checks" line of that step
   - **Problems:** anything that did not behave as specified
3. End with the question **"Step N done. Continue to step N+1?"** and **wait for the user's reply.** Do not start the next step until the user says yes.
4. If a step fails its check, do not continue. Report what failed, propose a fix, and ask whether to fix, roll back to the Part 0 snapshot, or stop.

| Step | Do | User checks before saying yes |
|---|---|---|
| **1** | Part 0 (snapshot) + Part A (gateway model, tests) | Tests pass; the energy card shows on the dashboard; starting the demo preset moves the battery value |
| **2** | Part B (state-transition trigger), build, flash bin01 on COM6 | Moving the battery slider across 30% produces a `state-transition` row within about 10 s |
| **3** | Part D (harvest-aware CONSERVE), the fsm_conserve_check table, build, flash | The CONSERVE rule switch is on the dashboard and shows **Threshold** after boot |
| **4** | Acceptance check, including both E4 runs | The numbers in the final report |

Part C (panel sensor) is **not** part of these four steps. Do it only if the user asks for it separately.

## Part 0: known-good snapshot (do this first)

- `git add -A && git commit -m "pre-viva known-good snapshot"` in `smart-bin-pp1`, and tag it `viva-known-good`.
- Copy the current built firmware binary (`.pio/build/esp32dev/firmware.bin`) to `_backup_viva/firmware-known-good.bin`, and write the one-line reflash command next to it in `_backup_viva/README.txt`.
- Rolling back must take one command.

## Part A: gateway (backend only, no reflash)

1. **New module `backend/solar-sim.mjs`** with pure, unit-testable functions and no I/O:
   - `solarPowerMw(virtualHour, preset)`: zero before sunrise (06:00) and after sunset (18:00); between them `P_peak · sin(π·(h−6)/12) · cloud(h)`. `cloud(h)` is 1 except for an optional **scripted** dip (default 0.25× between 13:00 and 14:00). **No randomness.** The demo must be repeatable; the PP2 demo failed because it was not.
   - `stepSoc(state, dtRealS, loadMw, preset)`: returns the new state. Virtual seconds = `speed × dtRealS`. Charge efficiency η = 0.85. Clamp SoC to **[6, 100]**. `BATT_DEAD` is 5% and **latches TX off on the node**; the model must never send ≤ 5 unless the preset explicitly sets `allowDead: true`.
   - `loadFromReadings(rows)`: mean power from the Δ`energy_mwh` / Δ`rx_ts` of the last two readings that share the same `power_session` and `ina_present = 1`. If that is unavailable, return the preset's fallback (default 900 mW, about bin01's measured average on 1 Sep) with `source: "modelled"`. Never silently mix measured and modelled. Always return the source.
   - `simulateDay(preset, loadMw)`: runs one full virtual day offline and returns the trajectory (used by the tests).

2. **Presets** (exported constants, all values editable):
   - `demo`: one virtual day in **5 real minutes** (speed 288), start 06:00, start SoC 55%. **Auto-size** the capacity from the load so that a 12 h night drains 75% of capacity (`C = P_load · 43200 s / 0.75`), with `P_peak = 3 × P_load`. The goal is one virtual day that crosses **> 80%** (AIMD MD band), **< 70%** (AI band), **< 30%** (CONSERVE) and recovers **> 30%** (back to STABLE).
   - `eval`: one virtual day in 2 real hours (speed 12), same shape, for a longer logged run.
   - `realistic`: real capacity (2000 mAh × 3.7 V), `P_peak` 2000 mW, speed 1. Document that it is illustrative, not site-calibrated.

3. **Server wiring in `server.js`** (keep existing behaviour; the backend listens on **3002**, which is the `server.js` default and what the dashboard calls, even though CLAUDE.md says 3001; leave `server-next.js` alone):
   - `POST /api/solar/start {nodeId, preset, overrides?}`, `POST /api/solar/stop {nodeId}`, `GET /api/solar` (state for every node).
   - Tick every 2 s real time. Publish to `bin/{nodeId}/battery` when SoC changes by ≥ 0.5 points or every 10 s, whichever comes first.
   - Emit Socket.io event `solar` on every tick: `{node_id, virtual_time, solar_mw, load_mw, load_source, soc_pct, preset, running}`.
   - New table `energy_sim` (id, run_label, node_id, rx_ts, virtual_hour, solar_mw, load_mw, load_source, soc_pct, published INTEGER). Use the same `PRAGMA table_info` → `CREATE/ALTER` migration style the file already uses. Stamp the current `run_label`. Never delete rows.
   - While a simulation is running for a node, a manual `/api/battery` call **stops that node's simulation first** and returns `{stopped_solar: true}`, so two sources never fight over one topic.

4. **Dashboard (`App.jsx`)**: add an "Energy model" card next to the battery slider:
   - Start/stop button, preset select
   - Virtual clock, solar mW, load mW with a `measured` / `modelled` badge, simulated SoC %
   - A small line chart with solar mW and SoC over the run
   - Label it exactly: **"Simulated solar harvest · measured load (INA219) · simulated battery"**
   - When the simulation is running, show the slider as disabled with the note "battery driven by energy model"

5. **Tests** (`node --test`):
   - For loads of 150, 500 and 1000 mW, `simulateDay(demo)` crosses > 80, < 70, < 30 and back above 30, and never goes below 6.
   - `solarPowerMw` is 0 at 03:00 and 21:00, and peaks at 12:00 without a cloud dip.
   - `loadFromReadings` returns `modelled` when sessions differ or INA data is missing.

## Part B: firmware, report non-urgent state changes (the user approved this as one small change; skip Part B if told otherwise)

**Problem:** in `main.cpp` the ADAPTIVE branch transmits immediately only on `urgentTransition` (`urgent && stateChanged`). Entering CONSERVE or returning to STABLE is not reported until the next timer or deadband, which is 5–30 min in STABLE. The dashboard therefore shows a stale state. This also plausibly explains the PP2 demo showing STABLE throughout.

**Change:** add a `stateTransition` trigger. When `stateChanged` is true and the new state is not ALERT (ALERT is already covered), transmit with `tx_trigger = "state-transition"`, subject to the existing `DEADBAND_COOLDOWN_MS` (10 s) so a threshold oscillation cannot cause a burst.
- Do **not** touch `fsm.h`, `config.h` thresholds, the AIMD step or the AoI floors.
- `fsmOnTransmit()` must still run once per successful transmission, as it does now.
- `analyze.py` counts these transmissions normally (they are real transmissions; do not exclude them from TCR). Add `state-transition` to any reason lists it uses.
- Also fix the cosmetic serial string `"[BUILD] INA219 + MQ5 removed"`: the INA219 is present, only MQ-5 is removed.
- Build and flash bin01 on COM6 using the existing `MQTT_HOST_OVERRIDE` build-flag method in HANDOVER.md.

## Part D: harvest-aware CONSERVE (user approved this policy change; thresholds below are the user's)

**Why:** CONSERVE today is `battery ≤ 30%`. It cannot tell 28% at noon while charging from 28% at night while draining. The new rule enters CONSERVE only when the battery is low **and** draining.

**Gateway:** on every solar-model tick, also publish **net power** = `η·P_solar − P_load` in mW (integer, can be negative) to `bin/{nodeId}/net_power`, using the same publish cadence as the battery topic. Log it as a `net_mw` column in `energy_sim`.

**Firmware:**
- **`config.h`**, new block "harvest-aware CONSERVE":
  - `BATT_CRIT 15.0f`
  - `BATT_EXIT 35.0f`
  - `NET_POWER_STALE_MS 60000UL`
  - `NET_EXIT_MW 50.0f`
  - Leave `BATT_LOW` (30) unchanged.
- **`main.cpp`:**
  - Subscribe to `bin/{id}/net_power` (keep the value in `netPowerMw`, plus `netPowerRxMs`) and `bin/{id}/conserve_rule` (payload `threshold` or `harvest`, kept in `conserveRule`).
  - **Default `conserveRule = threshold`**, so behaviour is identical to today until the rule is switched at runtime.
  - Re-subscribe both topics on reconnect, like the others.
  - Persist `conserveRule` in RTC memory like `simulatedBattPct`.
- **`fsm.h`:** replace only the battery branch (rule 2) with the following. Urgency (rule 1), activity (rule 3), STABLE (rule 4), the AIMD step and the AoI floors are untouched.

```
harvestUsable = (conserveRule == HARVEST) && netPowerFresh   // received within NET_POWER_STALE_MS
if !harvestUsable:
    lowBattery = battPct <= BATT_LOW                           // exactly today's rule
else:
    lowBattery = battPct < BATT_CRIT
              || (battPct < BATT_LOW && netPowerMw <= 0)
              || (ctx.state == CONSERVE
                  && !((netPowerMw > NET_EXIT_MW && battPct >= BATT_CRIT) || battPct > BATT_EXIT))
// Exit as soon as the node is clearly charging and above critical. A battery-only
// exit at 35% would keep it in CONSERVE longer than today's 30% rule on a sunny
// morning. The +50 mW margin stops cloud flicker from toggling the state.
```

- Pass `netPowerMw`, `harvestUsable` and the rule into `fsmUpdate()` as extra parameters. Do not read globals from inside `fsm.h`.
- **Payload:** add `net_power_mw` (null when stale), `conserve_rule` (`threshold` / `harvest`) and `conserve_basis` (`threshold`, `harvest`, or `harvest-stale-fallback`).
- Add `harvest-low-draining` / `harvest-critical` / `harvest-hold` values to `state_cause` where CONSERVE is entered by the new branch.
- Check the 896-byte telemetry buffer `static_assert` still holds.

**Backend and dashboard:**
- `POST /api/conserve-rule/:nodeId {rule}` publishes to the topic.
- The dashboard shows a two-button switch "CONSERVE rule: Threshold | Harvest-aware" on the node card, plus the current `conserve_basis` and net power, so the rule can be flipped back during the viva without reflashing.
- `gateway.mjs` `normalizeReading` accepts the three new fields, validated.

**Offline unit check:** add `firmware/test/fsm_conserve_check.md`, a table of input → expected state that Claude Code verifies by reasoning through the code and lists in its report:
- 28% / net −800 / harvest → CONSERVE
- 28% / net +300 / harvest → STABLE
- 28% / any / threshold → CONSERVE
- 12% / net +300 / harvest → CONSERVE
- In CONSERVE, 20% / net +300 / harvest → STABLE (charging, above critical)
- In CONSERVE, 20% / net +20 / harvest → stay CONSERVE (below the 50 mW margin)
- In CONSERVE, 12% / net +300 / harvest → stay CONSERVE (critical)
- In CONSERVE, 36% / net −100 / harvest → STABLE (above 35%)
- 28% / harvest / net stale → CONSERVE (fallback)
- Fill 85% / 20% battery / harvest → ALERT

## Part C (optional, only if the user has wired the panel): measured harvest on bin01

Hardware the user will fit: a mini solar panel, 5 V / 100 mA (about 0.5 W peak), 100×69 mm. It is wired as a **harvest sensor only, not in the power path**:
- The panel feeds a fixed load resistor (47 Ω, 1 W or higher).
- The voltage across that resistor goes through a divider (for example 20 kΩ over 10 kΩ, so 5 V becomes about 1.67 V) into **GPIO32 (ADC1_CH4)**. ADC2 pins conflict with Wi-Fi; 34/35 are taken.
- Board power stays on USB. Do not add a real battery or charger.

Firmware:
- Add `readHarvestMw()` that samples GPIO32 (average of 8 reads), rescales by the divider ratio and returns `V²/R` in mW, with ratio and R as `#define`s in `config.h` under a clearly separate "harvest sensor" block. No threshold changes.
- Add `harvest_mw` and `harvest_present` to the payload, guarded by `ENABLE_HARVEST_SENSOR` (default 0, so the default build is unchanged).
- Check the 896-byte telemetry buffer `static_assert` still holds.

Gateway: add `solarSource: "curve" | "measured"` to the presets.
- With `measured`, use the latest `harvest_mw` scaled by a preset `panelScale` (default 1). This lets the model show a larger panel without pretending it was measured.
- Fall back to the curve when harvest data is missing, and record `solar_source` in `energy_sim`.
- Indoors the panel will read close to 0. That is expected; the demo preset keeps using the curve.

bin02 (ESP8266) has one ADC, already used by the gas sensor, so it stays on the curve.

## Acceptance check (run once, then stop)

1. Start backend and dashboard, set run label `solar-demo-<date>`, set bin01 to ADAPTIVE with the sensor mounted in the bin at an empty-bin distance.
2. `POST /api/solar/start {nodeId:"bin01", preset:"demo"}`.
3. Over one 5-minute virtual day, confirm in the DB that bin01 rows show **STABLE → CONSERVE → STABLE**, with `state-transition` rows at each change, and that `energy_sim` has rows with `load_source = measured`.
4. **Rule A/B (Part D):** set run label `e4-threshold-<date>`, keep `conserve_rule = threshold`, and run one demo day. Then set run label `e4-harvest-<date>`, switch to `harvest`, and run one more demo day with the same preset. For each run, report:
   - virtual hours spent in CONSERVE
   - minimum SoC
   - number of transmissions
   - the virtual time of each CONSERVE entry and exit
   
   Expected: both rules enter CONSERVE at about the same evening time. The harvest rule should leave it soon after sunrise, while the threshold rule waits for 30%. If the harvest run spends **more** time in CONSERVE, report that; do not tune the thresholds to make it look better.
5. Report: the SoC trajectory min/max, the virtual time of each state change, the reasons on the transmitted rows, and anything that did not behave as specified. Finish by switching `conserve_rule` back to `threshold` and leaving the node running.

Wait for each run **once** (about 6 minutes each). Do not sit in a polling loop. If no rows arrive within 2 minutes of start, stop and report what you see (broker, Wi-Fi, network profile) instead of retrying.

## Do not

- Add ML, authentication or cloud MQTT
- Change any threshold other than adding the Part D constants; touch urgency, activity, AIMD or the AoI floors
- Make `harvest` the boot default; the node must start on `threshold`
- Delete or rewrite existing readings
- Present simulated SoC as measured battery charge anywhere in the UI or logs
