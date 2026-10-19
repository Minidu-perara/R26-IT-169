# R26-IT-169 step F results: bin01, 20 Oct 2026

Every number carries one label:

- **[measured]** read from the stored rows (server receive time `rx_ts`, row counts) or from the node's INA219.
- **[modelled]** produced by a model with assumed parameters, not observed.
- **[inferred]** reasoned from measured data plus the firmware logic; not separately measured.

## Setup

| Item | Value |
|---|---|
| Node | bin01 (ESP32, firmware commit `c0b4f6e`, state-transition trigger included) |
| Power | Power bank, USB unplugged. The INA219 sits on the power bank's 5 V path, so it measures the whole node: ESP32, Wi-Fi radio, MQ-135 heater, sensors |
| Bin | Small bin, empty distance 15.8 to 16.8 cm, `BIN_DEPTH_CM` 14.0 cm (unchanged through step F). Fill = (14.0 − d) / 12 × 100 |
| Targets | 80 % = 4.4 cm, 60 % = 6.8 cm. Alert card held at 3.0 cm (92 %) |
| Battery input | 80 % (simulated over MQTT). This is inside the AIMD hold band (70 to 80 %), so T* stays at 300 s in STABLE |
| Gateway | EDGE laptop 192.168.1.3, Mosquitto + backend; the readings were exported through `/api/readings` (`eval/out/stepf_readings_20261020.json`) |

| Scenario | Phase | Run label | Window (logged by the controller) |
|---|---|---|---|
| Reference | BASELINE 15 min, fixed 60 s | `quiet-baseline-20261020` | 03:57:50 to 04:12:50 |
| Quiet (steady state) | ADAPTIVE 15 min, empty bin, nobody near | `quiet-adaptive-20261020` | 04:12:50 to 04:27:50 |
| Alert (stress case) | ADAPTIVE 20 min, card events | `alert-adaptive-20261020` | 04:27:50 to 04:47:51 |

BASELINE sends at a fixed rate, so the one BASELINE phase is the reference for both TCR figures. Its measured cadence was 60.2 s between rows (13 gaps, 60.2 to 60.3 s) **[measured]**.

## 1. Steady state: quiet run

| | BASELINE | ADAPTIVE (quiet) |
|---|---|---|
| Transmissions in 15 min | 14 **[measured]** | 2 **[measured]** |
| Rate | 56.0 per hour **[measured]** | 8.0 per hour **[measured]** |
| What sent them | fixed 60 s timer | `timer` × 2 at T* = 300 s **[measured]** |

**Steady-state TCR = 85.7 % [measured]** (raw count ratio, equal 15 min windows). Target was 60 %.

Over a long quiet period the rate settles at 12 per hour (T* = 300 s) against 59.8 per hour (60.2 s cadence), which is **about 80 % [inferred]**. The 15 min window catches 2 of the 3 timer slots, so the measured value is slightly above the long-run one. AIMD was not exercised: the battery input stayed in the 70 to 80 % hold band, so T* stayed at 300 s **[measured]**.

## 2. Stress case: alert run

| | BASELINE | ADAPTIVE (alert run) |
|---|---|---|
| Transmissions | 14 in 15 min **[measured]** | 22 in 20 min **[measured]** |
| Rate | 56.0 per hour **[measured]** | 66.0 per hour **[measured]** |

**Stress-case TCR = −17.9 % [measured]** (rate-normalised, because the windows differ in length): with two alert events in 20 minutes the adaptive node transmitted **more** than the fixed baseline.

What the 22 adaptive transmissions were **[measured]**:

| Trigger | Count | When |
|---|---|---|
| `deadband` | 10 | during the two card holds, one every ~11 s |
| `urgent-transition` | 5 | entering ALERT: 4 in event 1, 1 in event 2 |
| `state-transition` | 4 | ACTIVE twice in event 1, then back to STABLE after each event |
| `timer` | 3 | quiet stretches, T* = 300 s |

Per event: event 1 cost 12 transmissions, event 2 cost 7, and the quiet stretches 3 **[measured]**.

### Why: the deadband collapses in ALERT, and the small bin's jitter crosses it

1. Placing the card is a fill jump of about 90 % in one second. When the 60 s velocity window closes, dF/dt is clamped at 500 %/hr **[measured: `dfdt` = 500 on every deadband row]**.
2. The deadband formula is max(1 %, 10 % − dF/dt × 0.5), so at 500 %/hr it drops to its 1 % floor **[inferred from fsm.h, unchanged]**. ALERT only requires a report every 60 s (AoI floor), but any fill change of 1 % or more is reported, limited by the 10 s cooldown.
3. In bin01's 14 cm bin, one HC-SR04 echo step is about 0.33 cm, which is **2.7 % of fill** **[measured: fill values while the card was held stepped 86.4, 89.1, 91.8, 94.5 %, i.e. 2.7 % apart]**. One step of jitter is therefore larger than the 1 % deadband, and the node transmits at every cooldown: rows at 04:37:58, 04:38:09, 04:38:20, 04:38:31 in event 2 **[measured]**.
4. In a 40 cm bin the same echo step is 0.86 % of fill, below the 1 % floor, so a steady card would not trigger these reports **[inferred]**. The small bin multiplies the fill resolution by three.
5. Event 1 added 4 ALERT entries and 2 ACTIVE rows in 27 s because the card first sat at about 4.4 cm, right on the 80 % line (fill 75.5 to 83.7 %), and `urgent-transition` has no cooldown or hysteresis **[measured rows; cause inferred]**. The operator then moved the card to about 2 cm (fill 99.9 %).

So the stress figure measures the policy doing what it is designed to do during a fast fill (report closely), amplified by the small bin's coarse fill resolution and, in event 1, by an unsteady card. No threshold, AoI floor, AIMD constant or the deadband formula was changed for this run.

## 3. Alert recall, latency and false alerts (alert run)

| Event | Logged time | First ALERT row | Latency | Note |
|---|---|---|---|---|
| 1 | 04:30:00 (approximate) | 04:30:09, `urgent-transition`, fill 83.7 % | 9.5 s | Card placed early; time from the operator's recollection, the key press came at 04:31:29 |
| 2 | 04:37:07.6 (key press) | 04:37:04.4, `urgent-transition`, fill 89.1 % | −3.2 s | The node reported before the key was pressed: the key press lags the placement |
| 3 | not performed | none | | The card was not placed |

- **Alert recall: 2 / 2 events reported in ALERT within 60 s [measured]**; 1 / 1 counting only the cleanly timed event. With n = 2 this does not establish 100 % recall statistically; it shows that both deliberate fills above 80 % were reported within seconds.
- **Latency: ≤ 9.5 s from the (approximate) placement time; event 2 within about 1 s of placement [inferred: the node reported 3.2 s before the key press, and the key press followed the placement]**.
- **False alerts: 0 [measured]**. All 15 ALERT rows were real: 13 with fill ≥ 80 % and 2 fill-rate rows right after removal, each carrying the held dF/dt of 500 %/hr from a logged fill step. The empty bin clamped to 0 % throughout the quiet run, so jitter at empty raised no fill-rate ALERT **[measured: 0 ALERT rows in the quiet run]**.

## 4. Measured power and energy per mode (INA219)

| Phase | Mean power | Mean current | Energy counter | Over |
|---|---|---|---|---|
| BASELINE | 949.3 mW **[measured]** | 198.7 mA **[measured]** | 206.5 mWh **[measured]** | 0.218 h |
| ADAPTIVE, quiet | 954.7 mW **[measured]** | 199.0 mA **[measured]** | 79.8 mWh **[measured]** | 0.084 h |
| ADAPTIVE, alert | 950.9 mW **[measured]** | 198.9 mA **[measured]** | 255.1 mWh **[measured]** | 0.268 h |

Energy per hour equals the mean power: about 950 mWh per hour in every mode **[measured]**. Supply 4.66 to 4.80 V; row-sampled current 189 to 261 mA, the higher values at transmit time **[measured]**. Mean power is the node's 10 Hz energy counter differenced between the first and last row of each phase within one boot session (`a061f7b6`); no row failed the power-path check.

**There is no measurable power difference between the modes [measured]**: transmitting 86 % fewer messages did not lower the mean draw. With deep sleep disabled, the ESP32 stays awake, the Wi-Fi radio stays associated and the MQ-135 heater is always on; the heater is roughly 150 mA of the ~199 mA **[inferred from the datasheet heater figure; the heater-off run (F.3) was not done]**. Transmission is a small share of the energy in this configuration.

## 5. Battery life (modelled)

The duty-cycle model in `analyze.py` assumes deep sleep between transmissions, which the demo firmware does not use, and a 2000 mAh cell:

| Scenario | Adaptive mean interval | BLEF (TX window 1 / 2 / 3 s) |
|---|---|---|
| Quiet | 450 s **[measured]** | 7.29 / 7.39 / 7.42 × **[modelled]** |
| Alert (stress) | 54.5 s **[measured]** | 0.91 / 0.91 / 0.91 × **[modelled]** |

These factors are what the transmission reduction would be worth on a node that sleeps between reports **[modelled]**. They are not observed battery life, and section 4 shows that on the current always-on node the measured power does not change.

## 6. Limits and what was not done

- Short windows: 15 + 15 + 20 min; counts are small (14, 2, 22).
- Alert recall rests on 2 events; event 1's time is approximate and event 3 was not performed.
- One node only (bin01). bin02 is flashed and measured in step E after step F.
- AIMD was not exercised (battery input held at 80 %).
- Heater-off run (F.3) not done, so the MQ-135 heater's share of the power is inferred, not measured.
- Run on the evening bench in a small bin; the sensor geometry (14 cm depth, 2.7 % per echo step) is part of the result.

## 7. Files and reproduction

- Per-scenario reports and figures: `eval/out/quiet/` (steady state) and `eval/out/alert/` (stress case): `metrics.json`, `metrics.md`, `fig1` to `fig4`.
- Rows: `eval/out/stepf_readings_20261020.json` (all three labels, bin01, as served by the EDGE backend).
- Event logs as recorded by the controller: `eval/events_quiet_20261020.csv`, `eval/events_alert_20261020.csv`. Analysis copy with event 1's time corrected and annotated: `eval/events_alert_20261020_analysis.csv`. The first, aborted attempt: `eval/events_aborted_20261020.csv` (not analysed).

```
python eval/analyze.py --db stepf.db --node bin01 --baseline-label quiet-baseline-20261020 --adaptive-label quiet-adaptive-20261020 --events eval/events_quiet_20261020.csv --out-dir eval/out/quiet
python eval/analyze.py --db stepf.db --node bin01 --baseline-label quiet-baseline-20261020 --adaptive-label alert-adaptive-20261020 --events eval/events_alert_20261020_analysis.csv --out-dir eval/out/alert
```

`stepf.db` is the readings table rebuilt from the JSON export (or an `eval-snapshot.db` backup from the EDGE laptop).
