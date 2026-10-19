# R26-IT-169 -- matched run results

Generated 2026-10-20 04:43:51 Sri Lanka Standard Time, node **bin01**, labels `quiet-baseline-20261020` / `quiet-adaptive-20261020`

## Transmission Count Reduction (measured)

| Phase | Transmissions | Duration (h) | TX/hour | Window |
|---|---|---|---|---|
| BASELINE (60 s fixed) | 14 | 0.25 | 55.99 | logged start/end |
| ADAPTIVE (policy) | 2 | 0.25 | 8.0 | logged start/end |

**TCR = 85.71%** (raw count ratio, phases equal length). Target was >= 60%.

### What caused each adaptive transmission

| Reason | Count |
|---|---|
| `timer` | 2 |

T* ranged 300-300 s (mean 300.0 s); distinct values observed: [300].

## Alert Recall

Not measured: no alert events inside either phase. Requires a supervised session with logged ground-truth event times.

## ALERT rows by cause (ADAPTIVE, measured)

No ALERT rows in the adaptive phase.

## Measured power (INA219 on the 5 V input)

| Phase | Energy (mWh) | Hours | Mean power (mW) | Mean current (mA) | Sessions |
|---|---|---|---|---|---|
| BASELINE | 206.536 | 0.218 | 949.3 | 198.7 | 1 |
| ADAPTIVE | 79.822 | 0.084 | 954.7 | 199.0 | 1 |

Mean power is the 10 Hz on-node energy counter differenced within each boot session. It includes the MCU, the Wi-Fi radio and the MQ-135 heater.

## Battery Life Extension Factor (modelled)

> DEEP_SLEEP_ENABLED is 0 and no battery is discharged during the runs, so battery life cannot be observed. The INA219 measures power on the 5 V input (see Measured power), but with deep sleep off that power is dominated by the MCU, the radio and the MQ-135 heater. This is a duty-cycle model driven by the measured transmission rate.

Adaptive mean interval 450.1 s against a 60 s baseline.

| TX window (s) | Baseline life (days) | Adaptive life (days) | BLEF |
|---|---|---|---|
| 1.0 | 37.2 | 270.8 | **7.29x** |
| 2.0 | 19.6 | 145.1 | **7.39x** |
| 3.0 | 13.3 | 99.1 | **7.42x** |

BLEF 7.29x to 7.42x across the assumed transmission-window range. Target was > 2x.

## Data integrity

- Baseline gaps beyond 3x the expected interval (incl. window edges): 0
- Adaptive gaps beyond 3x the longest T* (incl. window edges): 0
- Rows flagged sensor_fault: 0
- Peak tx_dropped counter: 0
- Mode-switch straddler rows excluded: 0 baseline, 0 adaptive
- Rows outside the logged windows excluded: 0 baseline, 0 adaptive
