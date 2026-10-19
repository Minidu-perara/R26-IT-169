# eval/ — R26-IT-169 experiment analysis

Drop `analyze.py` and `figures.py` into `Edge/smart-bin-pp1/eval/`. Run from the
repo root.

```bash
python eval/analyze.py                       # numbers  -> eval/out/metrics.{json,md}
python eval/figures.py                       # figures  -> eval/out/fig1..4.png
```

Defaults assume `backend/smart-bin.db` with run labels `E1-baseline` and
`E1-adaptive`. Override with `--db`, `--node`, `--baseline-label`,
`--adaptive-label`, `--events`, `--event-slack-s`, `--out-dir`.

Every metric is for one node: `--node` (default `bin01`). The backend stamps the
same run label on every node's rows, so the scripts never mix nodes.
`figures.py` takes the node from `metrics.json` (or `--node`).

Requires `pandas` and `matplotlib`.

## The three experiments

**E1 — steady state, both modes.** The overnight run. Node parked in STABLE with
a static target, BASELINE for N hours then ADAPTIVE for the same N hours, battery
stepped from 85% to 65% at the midpoint so AIMD has something to react to.
Produces TCR and the T* timeline.

**E2 — supervised events.** Ground truth for Alert Recall. Move the target to
known distances at times you write down in `eval/events.csv`
(`iso_time,event_type,note`; `event_type` is `alert` for the ALERT target,
`active` for the ACTIVE target, `clear` for target removed, plus `baseline-start`, `baseline-end`,
`adaptive-start`, `adaptive-end` for the phase windows; only `alert` rows count
toward recall; times without an offset are this PC's local time). Edit the file
in a text editor, not Excel, and keep commas out of the notes. Log the four
phase-window rows: without them the phase length falls back to the first and
last row, which is too short for a sparse ADAPTIVE phase and distorts TCR.
Fill percentage from distance:
`fill = (D - d_cm) / (D - 2) × 100`, where D is the node's `BIN_DEPTH_CM`
(bin01: 14.0 cm, see HARDWARE.md). For bin01: 60 % = 6.8 cm, 80 % = 4.4 cm, so
the targets are **5.5 cm (71 %, ACTIVE)** and **3.0 cm (92 %, ALERT)**. D is set
1.5-2 cm shorter than the measured empty distance, so jitter at empty clamps to
0 % instead of turning into a fill-rate ALERT. Hold the ALERT target for at
least 75 s (BASELINE reports every ~61 s),
write the time down just after placing the target, keep events at least 2 min
from the phase edges and at least 125 s apart (a step's fill rate is held until
the second velocity-window closure, up to ~122 s).

A sudden step is also a fast fill rate. Placing the ACTIVE target shows ACTIVE
within a second, then a fill-rate ALERT 0-61 s later for about 61 s, then
ACTIVE again. The ALERT target goes straight to ALERT (fill >= 80 %).

**E3 — gateway footprint.** Sample mosquitto and node CPU/RSS during a run into
`eval/footprint.csv`. Supports the claim that the gateway tier fits Pi-class
hardware. Not required for the headline metrics.

## What each metric does and does not prove

**TCR** is measured. It is a count ratio over equal wall-clock windows, computed
from `rx_ts` (server wall clock) and `COUNT(*)`, never from the device-side
`tx_count`, which resets on every mode switch and reboot. If every adaptive
transmission has `reason = "timer"`, the run measured the AoI floor and AIMD only;
the deadband and urgency paths were never exercised and E2 is required before
claiming them.

**Alert Recall** is measured only when `events.csv` holds real logged times. In
ADAPTIVE, an `alert` event counts as recalled if a row in state `ALERT` lands
within 60 s (`AoI_ALERT`), whatever trigger carried it (current firmware labels
the entry `urgent-transition`; older firmware used `urgency-*`). In BASELINE the
FSM does not run, so the test is a row reporting fill >= 80 % within 60 s. The
window opens `--event-slack-s` (30 s) before the logged time to allow for
logging lag.

**False alerts** are counted separately, because 100% recall with a pile of
false alerts is not a good result. Every ADAPTIVE `ALERT` row is classified by
cause: fill >= 80 %, gas above threshold, or fill rate. A fill-rate ALERT is
real when the row's own dF/dt shows a rise of at least 5 % in one ~61 s
velocity window (about 295 %/hr; a deliberate step to 15 cm or 8 cm hits the
500 %/hr clamp), and false when it does not (HC-SR04 jitter of one echo step is
about 51 %/hr, which still trips the 10 %/hr rule). An episode is false when the
row that raised it is false. Fill-rate ALERTs after a real rise that was not
logged are listed for checking.

**Measured power** comes from the node's INA219 on the 5 V input: the 10 Hz
energy counter, differenced within each boot session, gives mWh and mean mW
and mA per phase. Rows that fail the backend's power-path check (4.5-5.5 V,
more than 1 mA) are excluded and counted.

**BLEF is modelled, not measured.** `DEEP_SLEEP_ENABLED` is 0 during the demo and
no battery is discharged in the runs, so battery life is never observed (the
INA219 measures power, which with deep sleep off is dominated by the MCU, radio
and heater). The
figure comes from a duty-cycle model driven by the measured transmission rate,
with every assumption in `ENERGY_MODEL` at the top of `analyze.py`. It is reported
as a range across the assumed transmission window because that window is the
dominant unknown. Label it "modelled" everywhere it appears.

## Data integrity checks

`analyze.py` reports, and you should read before trusting any number:

- inter-transmission gaps beyond 3× the expected interval, which mean a reboot,
  a broker outage or a Wi-Fi drop. These silently inflate TCR, because a
  transmission that never happened looks identical to one correctly suppressed
- rows flagged `sensor_fault`
- peak `tx_dropped`, which separates suppressed from undelivered
- mode-switch straddler rows, excluded from the counts but counted and reported
