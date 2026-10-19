# eval/ — R26-IT-169 experiment analysis

Drop `analyze.py` and `figures.py` into `Edge/smart-bin-pp1/eval/`. Run from the
repo root.

```bash
python eval/analyze.py                       # numbers  -> eval/out/metrics.{json,md}
python eval/figures.py                       # figures  -> eval/out/fig1..4.png
```

Defaults assume `backend/smart-bin.db` with run labels `E1-baseline` and
`E1-adaptive`. Override with `--db`, `--baseline-label`, `--adaptive-label`,
`--events`, `--out-dir`.

Requires `pandas` and `matplotlib`.

## The three experiments

**E1 — steady state, both modes.** The overnight run. Node parked in STABLE with
a static target, BASELINE for N hours then ADAPTIVE for the same N hours, battery
stepped from 85% to 65% at the midpoint so AIMD has something to react to.
Produces TCR and the T* timeline.

**E2 — supervised events.** Ground truth for Alert Recall. Move the target to
known distances at times you write down in `eval/events.csv`
(`iso_time,event_type,note`). Fill percentage from distance:
`fill = (40 - d_cm) / 38 × 100`. So 8 cm ≈ 84% (ALERT), 15 cm ≈ 66% (ACTIVE),
25 cm ≈ 39% (STABLE). Space events at least 90 s apart so the 60 s velocity
window can close between them.

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

**Alert Recall** is measured only when `events.csv` holds real logged times. An
event counts as recalled if an `urgency-*` transmission lands within 60 s
(`AoI_ALERT`). The script reports false positives separately, because 100% recall
with a pile of false positives is not a good result and must not be reported as
one.

**BLEF is modelled, not measured.** `DEEP_SLEEP_ENABLED` is 0 during the demo and
there is no current-measurement hardware, so battery life is never observed. The
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
