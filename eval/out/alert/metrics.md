# R26-IT-169 -- matched run results

Generated 2026-10-20 04:48:44 Sri Lanka Standard Time, node **bin01**, labels `quiet-baseline-20261020` / `alert-adaptive-20261020`

## Transmission Count Reduction (measured)

| Phase | Transmissions | Duration (h) | TX/hour | Window |
|---|---|---|---|---|
| BASELINE (60 s fixed) | 14 | 0.25 | 55.99 | logged start/end |
| ADAPTIVE (policy) | 22 | 0.333 | 66.0 | logged start/end |

**TCR = -17.86%** (rate-normalised, phases differ in length). Target was >= 60%.

### What caused each adaptive transmission

| Reason | Count |
|---|---|
| `deadband` | 10 |
| `urgent-transition` | 5 |
| `state-transition` | 4 |
| `timer` | 3 |

T* ranged 60-300 s (mean 136.4 s); distinct values observed: [60, 300].

## Alert Recall

Measured against logged ground-truth event times.

- ADAPTIVE: 2/2 alert events recalled (**100.0%**); criterion: row in state ALERT within 60 s
  - latency from logged time: min -3.2 s, mean 3.1 s, max 9.5 s
  - 2026-10-20 04:30:00: recalled, 9.5 s, `urgent-transition`, fill 83.7 %, row 5170
  - 2026-10-20 04:37:07: recalled, -3.2 s, `urgent-transition`, fill 89.1 %, row 5183
- BASELINE: not measured (no alert events in this phase)

## ALERT rows by cause (ADAPTIVE, measured)

15 ALERT rows in 4 episodes. Rule: fill >= 80 % or gas above threshold: real reading. fill-rate: real if the row's dF/dt is >= 295 %/hr (a rise of 5 % or more in one ~61 s window); otherwise false. An episode is false when the row that raised it is false.

| Cause | Rows |
|---|---|
| fill | 13 |
| fill-rate | 2 |

| fill-rate ALERT basis | Rows |
|---|---|
| logged-fill-step | 2 |

**False alerts: 0 episodes (0 rows)** with no real fill change behind them.

## Measured power (INA219 on the 5 V input)

| Phase | Energy (mWh) | Hours | Mean power (mW) | Mean current (mA) | Sessions |
|---|---|---|---|---|---|
| BASELINE | 206.536 | 0.218 | 949.3 | 198.7 | 1 |
| ADAPTIVE | 255.051 | 0.268 | 950.9 | 198.9 | 1 |

Mean power is the 10 Hz on-node energy counter differenced within each boot session. It includes the MCU, the Wi-Fi radio and the MQ-135 heater.

## Battery Life Extension Factor (modelled)

> DEEP_SLEEP_ENABLED is 0 and no battery is discharged during the runs, so battery life cannot be observed. The INA219 measures power on the 5 V input (see Measured power), but with deep sleep off that power is dominated by the MCU, the radio and the MQ-135 heater. This is a duty-cycle model driven by the measured transmission rate.

Adaptive mean interval 54.5 s against a 60 s baseline.

| TX window (s) | Baseline life (days) | Adaptive life (days) | BLEF |
|---|---|---|---|
| 1.0 | 37.2 | 33.8 | **0.91x** |
| 2.0 | 19.6 | 17.8 | **0.91x** |
| 3.0 | 13.3 | 12.1 | **0.91x** |

BLEF 0.91x to 0.91x across the assumed transmission-window range. Target was > 2x.

## Data integrity

- Baseline gaps beyond 3x the expected interval (incl. window edges): 0
- Adaptive gaps beyond 3x the longest T* (incl. window edges): 0
- Rows flagged sensor_fault: 0
- Peak tx_dropped counter: 0
- Mode-switch straddler rows excluded: 0 baseline, 0 adaptive
- Rows outside the logged windows excluded: 0 baseline, 0 adaptive
