#!/usr/bin/env python3
"""
R26-IT-169 -- evaluation of the joint adaptive control policy.

Computes the three claimed metrics from the SQLite telemetry captured during a
BASELINE/ADAPTIVE experiment pair:

  TCR   Transmission Count Reduction   -- directly measured
  AR    Alert Recall                   -- measured, requires ground truth
  BLEF  Battery Life Extension Factor  -- MODEL-BASED, not measured

Read the caveats printed at the end of the run. They are not decoration; they
are the answers to the questions an examiner will ask about these numbers.

Usage
  python eval/analyze.py
  python eval/analyze.py --db backend/smart-bin.db --baseline-label E1-baseline \
                         --adaptive-label E1-adaptive --events eval/events.csv \
                         --out-dir eval/out
"""

import argparse
import json
import os
import sqlite3
import sys
from datetime import datetime, timezone

import pandas as pd

# ---------------------------------------------------------------------------
# Energy model. Every parameter here is an assumption, not a measurement.
# They are collected in one place so they can be cited in the report and varied
# in the sensitivity table. Sources: ESP32 datasheet + Espressif power
# consumption application notes; the TX window figure is the dominant unknown,
# which is why BLEF is reported as a range rather than a point.
# ---------------------------------------------------------------------------
ENERGY_MODEL = {
    "I_deepsleep_mA": 0.010,   # 10 uA, deep sleep with RTC retention
    "I_idle_cpu_mA": 40.0,     # CPU awake, radio idle, sampling sensors
    "I_tx_window_mA": 120.0,   # mean draw across association + publish
    "t_tx_window_s": 2.0,      # duration of that window (the key unknown)
    "t_sense_s": 0.35,         # sensor read: 3x HC-SR04 + DHT22 + 2x ADC
    "battery_mAh": 2000.0,
}
TX_WINDOW_SENSITIVITY = [1.0, 2.0, 3.0]

AOI_ALERT_MS = 60_000        # alert must be reported within this window
BASELINE_INTERVAL_S = 60.0

URGENCY_PREFIX = "urgency-"


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------
def load(db_path):
    if not os.path.exists(db_path):
        sys.exit(f"ERROR: no database at {db_path}")
    con = sqlite3.connect(db_path)
    cols = {r[1] for r in con.execute("PRAGMA table_info(readings)")}
    for required in ("rx_ts", "run_label"):
        if required not in cols:
            sys.exit(
                f"ERROR: column '{required}' is missing from readings.\n"
                "This database predates the schema migration. Durations cannot "
                "be computed from device uptime, so the analysis would be wrong."
            )
    df = pd.read_sql_query("SELECT * FROM readings", con)
    con.close()
    # optional columns added late; treat absence as zero rather than crashing
    for opt in ("tx_dropped", "sensor_fault"):
        if opt not in df.columns:
            df[opt] = 0
    df["tx_dropped"] = pd.to_numeric(df["tx_dropped"], errors="coerce").fillna(0)
    df["sensor_fault"] = pd.to_numeric(df["sensor_fault"], errors="coerce").fillna(0)
    return df


def phase(df, label, expected_mode):
    """Rows for one experiment phase, with mode-switch straddlers removed.

    A few rows always land with the new run_label but the old mode, because the
    label is set over REST a moment before the mode command reaches the node.
    They are reported, not silently dropped: hiding them would hide a real
    source of error in the counts.
    """
    sel = df[df["run_label"] == label].copy()
    straddlers = sel[sel["mode"] != expected_mode]
    clean = sel[sel["mode"] == expected_mode].copy()
    clean = clean.sort_values("rx_ts").reset_index(drop=True)
    return clean, len(straddlers)


def span(rows):
    """(seconds, hours) covered by a phase, from wall clock not device uptime."""
    if len(rows) < 2:
        return 0.0, 0.0
    secs = (rows["rx_ts"].max() - rows["rx_ts"].min()) / 1000.0
    return secs, secs / 3600.0


def gaps(rows, expected_interval_s, tolerance=3.0):
    """Inter-transmission gaps far longer than the policy asked for.

    A gap of more than `tolerance` x the expected interval means the node was
    not transmitting when it should have been: a reboot, a broker outage, or a
    Wi-Fi drop. These have to surface, because otherwise they quietly inflate
    TCR -- transmissions that never happened look identical to transmissions
    that were correctly suppressed.
    """
    if len(rows) < 2:
        return []
    d = rows["rx_ts"].diff().dropna() / 1000.0
    out = []
    for idx, g in d.items():
        if g > expected_interval_s * tolerance:
            out.append({
                "at": iso(rows.loc[idx, "rx_ts"]),
                "gap_s": round(float(g), 1),
                "expected_s": expected_interval_s,
            })
    return out


def iso(ms):
    if pd.isna(ms):
        return None
    return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M:%S")


# ---------------------------------------------------------------------------
# TCR
# ---------------------------------------------------------------------------
def tcr(base, adapt):
    b_secs, b_hours = span(base)
    a_secs, a_hours = span(adapt)
    b_n, a_n = len(base), len(adapt)

    b_rate = b_n / b_hours if b_hours else 0.0
    a_rate = a_n / a_hours if a_hours else 0.0

    # Rate-normalised is the honest form whenever the two phases differ in
    # length. When they are equal it reduces to the raw count ratio anyway.
    raw = (1 - a_n / b_n) * 100 if b_n else 0.0
    norm = (1 - a_rate / b_rate) * 100 if b_rate else 0.0
    equal_length = b_hours > 0 and abs(a_hours - b_hours) / b_hours < 0.05

    reasons = adapt["reason"].value_counts().to_dict() if a_n else {}
    intervals = {}
    if a_n:
        iv = pd.to_numeric(adapt["interval"], errors="coerce").dropna()
        if len(iv):
            intervals = {
                "min_s": int(iv.min()), "max_s": int(iv.max()),
                "mean_s": round(float(iv.mean()), 1),
                "distinct": sorted(int(x) for x in iv.unique()),
            }

    mean_gap = a_secs / (a_n - 1) if a_n > 1 else 0.0

    return {
        "baseline": {"tx": b_n, "hours": round(b_hours, 3), "per_hour": round(b_rate, 2)},
        "adaptive": {"tx": a_n, "hours": round(a_hours, 3), "per_hour": round(a_rate, 2),
                     "mean_gap_s": round(mean_gap, 1)},
        "tcr_raw_pct": round(raw, 2),
        "tcr_normalised_pct": round(norm, 2),
        "phases_equal_length": bool(equal_length),
        "headline_pct": round(raw if equal_length else norm, 2),
        "adaptive_reasons": reasons,
        "adaptive_interval": intervals,
        "baseline_gaps": gaps(base, BASELINE_INTERVAL_S),
        "adaptive_gaps": gaps(adapt, intervals.get("mean_s", 300) if intervals else 300),
        "tx_dropped_rows": int(adapt["tx_dropped"].max()) if a_n else 0,
        "sensor_fault_rows": int((adapt["sensor_fault"] > 0).sum()) if a_n else 0,
    }


# ---------------------------------------------------------------------------
# Alert recall
# ---------------------------------------------------------------------------
EVENTS_TEMPLATE = """iso_time,event_type,note
2026-08-30 12:00:00,fill,moved target to 8cm - expect urgency-fill
2026-08-30 12:03:00,fill,moved target back to 25cm
"""


def recall(adapt, events_path):
    if not os.path.exists(events_path):
        os.makedirs(os.path.dirname(events_path) or ".", exist_ok=True)
        with open(events_path, "w") as f:
            f.write(EVENTS_TEMPLATE)
        return {"status": "no ground truth",
                "template_written": events_path,
                "note": "Log real event times in this file and re-run."}

    with open(events_path) as f:
        if f.read().strip() == EVENTS_TEMPLATE.strip():
            return {"status": "events file still contains the template rows",
                    "note": f"Replace the example rows in {events_path} with real "
                            "event times and re-run."}

    ev = pd.read_csv(events_path)
    if ev.empty:
        return {"status": "events file is empty"}

    # Force millisecond resolution explicitly. pandas 2 defaults to nanosecond
    # and pandas 3 to microsecond, so a bare .astype('int64') silently produces
    # a value that is off by 1000x on one of them. rx_ts is milliseconds.
    ev["ts_ms"] = pd.to_datetime(ev["iso_time"]).astype("datetime64[ms]").astype("int64")

    urg = adapt[adapt["reason"].astype(str).str.startswith(URGENCY_PREFIX)].copy()

    detected, matched_rows = [], set()
    for _, e in ev.iterrows():
        lo, hi = e["ts_ms"], e["ts_ms"] + AOI_ALERT_MS
        hit = urg[(urg["rx_ts"] >= lo) & (urg["rx_ts"] <= hi)]
        if len(hit):
            first = hit.iloc[0]
            matched_rows.add(int(first["id"]))
            detected.append({"event": e["iso_time"], "type": e.get("event_type", "?"),
                             "recalled": True,
                             "latency_s": round((first["rx_ts"] - e["ts_ms"]) / 1000.0, 1),
                             "reason": first["reason"]})
        else:
            detected.append({"event": e["iso_time"], "type": e.get("event_type", "?"),
                             "recalled": False, "latency_s": None, "reason": None})

    n = len(detected)
    hits = sum(1 for d in detected if d["recalled"])
    lats = [d["latency_s"] for d in detected if d["recalled"]]
    fps = urg[~urg["id"].isin(matched_rows)]

    return {
        "status": "measured",
        "events": n,
        "recalled": hits,
        "recall_pct": round(100.0 * hits / n, 1) if n else 0.0,
        "latency_s": {"min": min(lats), "max": max(lats),
                      "mean": round(sum(lats) / len(lats), 1)} if lats else None,
        "false_positives": int(len(fps)),
        "false_positive_reasons": fps["reason"].value_counts().to_dict() if len(fps) else {},
        "per_event": detected,
    }


# ---------------------------------------------------------------------------
# BLEF (modelled)
# ---------------------------------------------------------------------------
def avg_current_mA(T_s, m, t_tx):
    t_awake = t_tx + m["t_sense_s"]
    if T_s <= t_awake:                       # duty cycle saturated
        return m["I_tx_window_mA"]
    charge = (m["I_deepsleep_mA"] * (T_s - t_awake)
              + m["I_idle_cpu_mA"] * m["t_sense_s"]
              + m["I_tx_window_mA"] * t_tx)
    return charge / T_s


def blef(adapt_mean_gap_s):
    m = ENERGY_MODEL
    if not adapt_mean_gap_s:
        return {"status": "not computable, no adaptive transmissions"}
    table = []
    for t_tx in TX_WINDOW_SENSITIVITY:
        ib = avg_current_mA(BASELINE_INTERVAL_S, m, t_tx)
        ia = avg_current_mA(adapt_mean_gap_s, m, t_tx)
        lb, la = m["battery_mAh"] / ib, m["battery_mAh"] / ia
        table.append({"t_tx_window_s": t_tx,
                      "baseline_mA": round(ib, 4), "adaptive_mA": round(ia, 4),
                      "baseline_life_days": round(lb / 24, 1),
                      "adaptive_life_days": round(la / 24, 1),
                      "blef": round(la / lb, 2)})
    nominal = next(r for r in table if r["t_tx_window_s"] == m["t_tx_window_s"])
    return {
        "status": "MODELLED, not measured",
        "why": ("DEEP_SLEEP_ENABLED is 0 during the demo and there is no current "
                "measurement hardware, so battery life cannot be observed. This "
                "is a duty-cycle model driven by the measured transmission rate."),
        "model": m,
        "adaptive_mean_interval_s": round(adapt_mean_gap_s, 1),
        "blef_nominal": nominal["blef"],
        "blef_range": [min(r["blef"] for r in table), max(r["blef"] for r in table)],
        "sensitivity": table,
    }


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def markdown(res):
    t, r, b = res["tcr"], res["alert_recall"], res["blef"]
    L = ["# R26-IT-169 -- Experiment 1 results", "",
         f"Generated {res['generated']}", "",
         "## Transmission Count Reduction", "",
         "| Phase | Transmissions | Duration (h) | TX/hour |",
         "|---|---|---|---|",
         f"| BASELINE (60 s fixed) | {t['baseline']['tx']} | {t['baseline']['hours']} | {t['baseline']['per_hour']} |",
         f"| ADAPTIVE (policy) | {t['adaptive']['tx']} | {t['adaptive']['hours']} | {t['adaptive']['per_hour']} |",
         "",
         f"**TCR = {t['headline_pct']}%** "
         f"({'raw count ratio, phases equal length' if t['phases_equal_length'] else 'rate-normalised, phases differ in length'}). "
         f"Target was >= 60%.", ""]

    if t["adaptive_reasons"]:
        L += ["### What caused each adaptive transmission", "",
              "| Reason | Count |", "|---|---|"]
        for k, v in sorted(t["adaptive_reasons"].items(), key=lambda x: -x[1]):
            L.append(f"| `{k}` | {v} |")
        L.append("")

    if t["adaptive_interval"]:
        i = t["adaptive_interval"]
        L += [f"T* ranged {i['min_s']}-{i['max_s']} s (mean {i['mean_s']} s); "
              f"distinct values observed: {i['distinct']}.", ""]

    L += ["## Alert Recall", ""]
    if r.get("status") == "measured":
        L += [f"- Events: {r['events']}",
              f"- Recalled within the 60 s AoI floor: {r['recalled']} (**{r['recall_pct']}%**)",
              f"- False positives: **{r['false_positives']}**"]
        if r["latency_s"]:
            L.append(f"- Detection latency: min {r['latency_s']['min']} s, "
                     f"mean {r['latency_s']['mean']} s, max {r['latency_s']['max']} s")
        L.append("")
    else:
        L += [f"Not measured: {r.get('status')}. "
              "Requires a supervised session with logged ground-truth event times.", ""]

    L += ["## Battery Life Extension Factor (modelled)", ""]
    if b.get("status", "").startswith("MODELLED"):
        L += [f"> {b['why']}", "",
              f"Adaptive mean interval {b['adaptive_mean_interval_s']} s against a 60 s baseline.", "",
              "| TX window (s) | Baseline life (days) | Adaptive life (days) | BLEF |",
              "|---|---|---|---|"]
        for s in b["sensitivity"]:
            L.append(f"| {s['t_tx_window_s']} | {s['baseline_life_days']} | "
                     f"{s['adaptive_life_days']} | **{s['blef']}x** |")
        L += ["", f"BLEF {b['blef_range'][0]}x to {b['blef_range'][1]}x across the "
                  f"assumed transmission-window range. Target was > 2x.", ""]
    else:
        L += [b.get("status", "not computed"), ""]

    L += ["## Data integrity", "",
          f"- Baseline gaps beyond 3x the expected interval: {len(t['baseline_gaps'])}",
          f"- Adaptive gaps beyond 3x the expected interval: {len(t['adaptive_gaps'])}",
          f"- Rows flagged sensor_fault: {t['sensor_fault_rows']}",
          f"- Peak tx_dropped counter: {t['tx_dropped_rows']}",
          f"- Mode-switch straddler rows excluded: "
          f"{res['straddlers']['baseline']} baseline, {res['straddlers']['adaptive']} adaptive", ""]
    return "\n".join(L)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--db", default="backend/smart-bin.db")
    p.add_argument("--baseline-label", default="E1-baseline")
    p.add_argument("--adaptive-label", default="E1-adaptive")
    p.add_argument("--events", default="eval/events.csv")
    p.add_argument("--out-dir", default="eval/out")
    a = p.parse_args()

    df = load(a.db)
    base, s_b = phase(df, a.baseline_label, "BASELINE")
    adapt, s_a = phase(df, a.adaptive_label, "ADAPTIVE")

    if not len(base) or not len(adapt):
        print("Labels present in this database:",
              sorted(str(x) for x in df["run_label"].dropna().unique()))
        sys.exit(f"ERROR: baseline rows={len(base)}, adaptive rows={len(adapt)}. "
                 "Check --baseline-label / --adaptive-label.")

    t = tcr(base, adapt)
    res = {
        "generated": datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z"),
        "db": os.path.abspath(a.db),
        "labels": {"baseline": a.baseline_label, "adaptive": a.adaptive_label},
        "window": {"baseline_from": iso(base["rx_ts"].min()), "baseline_to": iso(base["rx_ts"].max()),
                   "adaptive_from": iso(adapt["rx_ts"].min()), "adaptive_to": iso(adapt["rx_ts"].max())},
        "straddlers": {"baseline": s_b, "adaptive": s_a},
        "tcr": t,
        "alert_recall": recall(adapt, a.events),
        "blef": blef(t["adaptive"]["mean_gap_s"]),
    }

    os.makedirs(a.out_dir, exist_ok=True)
    with open(os.path.join(a.out_dir, "metrics.json"), "w") as f:
        json.dump(res, f, indent=2)
    with open(os.path.join(a.out_dir, "metrics.md"), "w") as f:
        f.write(markdown(res))

    # ---- stdout summary -------------------------------------------------
    print("=" * 68)
    print("R26-IT-169  Experiment 1")
    print("=" * 68)
    print(f"BASELINE  {t['baseline']['tx']:>5} tx over {t['baseline']['hours']:>5.2f} h "
          f"= {t['baseline']['per_hour']:>6.2f}/h")
    print(f"ADAPTIVE  {t['adaptive']['tx']:>5} tx over {t['adaptive']['hours']:>5.2f} h "
          f"= {t['adaptive']['per_hour']:>6.2f}/h")
    print(f"\nTCR = {t['headline_pct']}%   (target >= 60%)")
    print(f"  raw {t['tcr_raw_pct']}% | normalised {t['tcr_normalised_pct']}% | "
          f"equal-length phases: {t['phases_equal_length']}")
    print(f"\nAdaptive transmissions by reason: {t['adaptive_reasons']}")
    if t["adaptive_interval"]:
        print(f"T* observed: {t['adaptive_interval']['distinct']}")

    r = res["alert_recall"]
    if r.get("status") == "measured":
        print(f"\nAlert Recall = {r['recall_pct']}%  ({r['recalled']}/{r['events']}), "
              f"false positives = {r['false_positives']}")
    else:
        print(f"\nAlert Recall: {r.get('status')}")

    b = res["blef"]
    if b.get("blef_nominal"):
        print(f"\nBLEF = {b['blef_nominal']}x  (MODELLED, range "
              f"{b['blef_range'][0]}-{b['blef_range'][1]}x)   (target > 2x)")

    print("\n-- integrity --")
    print(f"  baseline gaps: {len(t['baseline_gaps'])}   adaptive gaps: {len(t['adaptive_gaps'])}")
    print(f"  sensor_fault rows: {t['sensor_fault_rows']}   peak tx_dropped: {t['tx_dropped_rows']}")
    for g in (t["baseline_gaps"] + t["adaptive_gaps"])[:5]:
        print(f"    gap {g['gap_s']}s at {g['at']} (expected ~{g['expected_s']}s)")

    print("\n-- what these numbers do and do not prove --")
    print("  TCR is measured. It is the count ratio over equal wall-clock windows.")
    print("  If every adaptive transmission has reason='timer', this run measured")
    print("  the AoI floor and AIMD only; deadband and urgency were never exercised")
    print("  and need a separate supervised session.")
    if r.get("status") != "measured":
        print("  Alert Recall is NOT established. Do not claim 100% without it.")
    print("  BLEF is modelled from transmission rate, not measured. Say so on the slide.")
    print(f"\nWrote {a.out_dir}/metrics.json and {a.out_dir}/metrics.md")


if __name__ == "__main__":
    main()
