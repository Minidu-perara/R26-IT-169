#!/usr/bin/env python3
"""
R26-IT-169 -- evaluation of the joint adaptive control policy.

Computes the three claimed metrics from the SQLite telemetry captured during a
BASELINE/ADAPTIVE experiment pair:

  TCR   Transmission Count Reduction   -- directly measured
  AR    Alert Recall                   -- measured, requires ground truth
  BLEF  Battery Life Extension Factor  -- MODEL-BASED, not measured

plus the classification of every ADAPTIVE ALERT row by cause, and the mean
power measured by the node's INA219.

Read the caveats printed at the end of the run. They are not decoration; they
are the answers to the questions an examiner will ask about these numbers.

Usage
  python eval/analyze.py
  python eval/analyze.py --db eval-snapshot.db --node bin01 \
                         --baseline-label matched-baseline-20261020 \
                         --adaptive-label matched-adaptive-20261020 \
                         --events eval/events.csv --out-dir eval/out

Every number is for ONE node (--node, default bin01). The backend stamps the
same run label on every node's rows, so mixing nodes would corrupt every metric.

events.csv (iso_time,event_type,note; edit it in a text editor, no commas in
notes; times without an offset are this PC's local time):
  baseline-start / baseline-end / adaptive-start / adaptive-end  phase windows
  alert   target placed at the ALERT distance (bin01: 3.0 cm; counts toward recall)
  active  target placed at the ACTIVE distance (bin01: 5.5 cm)
  clear   target removed
Log the phase windows. Without them the phase length falls back to the first
and last row of the label, which is too short for a sparse ADAPTIVE phase.
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

# Urgency thresholds, copied from firmware/include/config.h. Used only to say
# which condition put a row in ALERT; the node made the decision itself.
FILL_ALERT = 80.0            # FILL_ALERT, %
GAS135_THRESHOLD = 2500      # GAS135_THRESHOLD, raw ADC
GAS5_THRESHOLD = 3500        # GAS5_THRESHOLD, raw ADC (MQ-5 removed, kept for old rows)

# A fill-rate ALERT is backed by a real fill change when the node's own dF/dt on
# that row shows a rise of at least REAL_STEP_PCT within one velocity window
# (VEL_WINDOW_MS is 60 s; about 61 s with loop timing). A 5 % rise is about
# 295 %/hr; the smallest deliberate step in the protocol (empty to the ACTIVE
# target, about 70 %) hits the 500 %/hr clamp. Jitter of one HC-SR04 echo step
# (0.33 cm) is 2.75 % (162 %/hr) in bin01's 14 cm bin and 0.86 % (51 %/hr) in a
# 40 cm bin, so it reads as false; a 1 cm reflection jump in the small bin
# (8 %) would read as real, but at empty the depth margin clamps it to 0 %.
# dfdt is held between window closures, so a row still in ALERT after the
# target was removed carries the rise that caused it.
REAL_STEP_PCT = 5.0
VEL_WINDOW_S = 61.0
REAL_RATE_PCT_HR = REAL_STEP_PCT * 3600.0 / VEL_WINDOW_S
# A real fill-rate ALERT this long after a logged event counts as deliberate.
EVENT_EXPLAINS_MS = 180_000
# Event types that must raise an ALERT and so count toward recall.
ALERT_EVENT_TYPES = {"alert"}
WINDOW_EVENT_TYPES = {"baseline-start", "baseline-end", "adaptive-start", "adaptive-end"}
KNOWN_EVENT_TYPES = ALERT_EVENT_TYPES | {"active", "clear"} | WINDOW_EVENT_TYPES
# Electrical path check, the same rule as backend/gateway.mjs powerPathValid.
POWER_PATH_V = (4.5, 5.5)


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------
def load(db_path, node):
    """(rows of this node, rows of every node, node ids present)."""
    if not os.path.exists(db_path):
        sys.exit(f"ERROR: no database at {db_path}")
    con = sqlite3.connect(db_path)
    cols = {r[1] for r in con.execute("PRAGMA table_info(readings)")}
    for required in ("rx_ts", "run_label", "node_id"):
        if required not in cols:
            sys.exit(
                f"ERROR: column '{required}' is missing from readings.\n"
                "This database predates the schema migration. Durations cannot "
                "be computed from device uptime, so the analysis would be wrong."
            )
    all_rows = pd.read_sql_query("SELECT * FROM readings", con)
    con.close()
    all_rows["rx_ts"] = pd.to_numeric(all_rows["rx_ts"], errors="coerce")
    nodes = sorted(str(x) for x in all_rows["node_id"].dropna().unique())
    df = all_rows[all_rows["node_id"] == node].copy()
    if df.empty:
        sys.exit(f"ERROR: no rows for node '{node}'. Nodes present: {nodes}")
    # optional columns added late; treat absence as zero or missing rather than crashing
    for opt in ("tx_dropped", "sensor_fault"):
        if opt not in df.columns:
            df[opt] = 0
    for opt in ("tx_trigger", "gas_135", "gas_5", "dfdt", "ina_present", "bus_voltage_v",
                "current_ma", "power_mw", "energy_mwh", "power_session", "sensor_profile"):
        if opt not in df.columns:
            df[opt] = None
    df["tx_dropped"] = pd.to_numeric(df["tx_dropped"], errors="coerce").fillna(0)
    df["sensor_fault"] = pd.to_numeric(df["sensor_fault"], errors="coerce").fillna(0)
    for num in ("fill_pct", "gas_135", "gas_5", "dfdt", "ina_present", "bus_voltage_v",
                "current_ma", "power_mw", "energy_mwh"):
        df[num] = pd.to_numeric(df[num], errors="coerce")
    # tx_trigger is the current field; reason carries the same value and is the
    # only one on rows from older firmware.
    df["trigger"] = df["tx_trigger"].where(df["tx_trigger"].notna(), df["reason"])
    df = df.sort_values("rx_ts").reset_index(drop=True)
    return df, all_rows, nodes


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


def phase_window(all_rows, label, ev, name):
    """{start, end, source} for one phase.

    Logged start/end times are the truth. The fallback is the first and last row
    of the label across every node and mode, which is still short for a sparse
    ADAPTIVE phase (its first row can come up to 300 s after the switch), so it
    is reported as a fallback.
    """
    if ev is not None:
        s = ev[ev["event_type"] == f"{name}-start"]
        e = ev[ev["event_type"] == f"{name}-end"]
        if len(s) and len(e) and int(e["ts_ms"].iloc[-1]) > int(s["ts_ms"].iloc[0]):
            return {"start": int(s["ts_ms"].iloc[0]), "end": int(e["ts_ms"].iloc[-1]),
                    "source": "logged start/end"}
    sel = all_rows[all_rows["run_label"] == label]["rx_ts"].dropna()
    if len(sel) >= 2:
        return {"start": int(sel.min()), "end": int(sel.max()),
                "source": "FALLBACK first/last row of the label (all nodes); log start/end"}
    return None


def clip(rows, w):
    """Rows inside a logged window, and how many fell outside it."""
    if w is None or not w["source"].startswith("logged"):
        return rows, 0
    inside = rows[(rows["rx_ts"] >= w["start"]) & (rows["rx_ts"] <= w["end"])]
    return inside.reset_index(drop=True), len(rows) - len(inside)


def gaps(rows, expected_interval_s, w, tolerance=3.0):
    """Inter-transmission gaps far longer than the policy asked for.

    A gap of more than `tolerance` x the expected interval means the node was
    not transmitting when it should have been: a reboot, a broker outage, or a
    Wi-Fi drop. These have to surface, because otherwise they quietly inflate
    TCR -- transmissions that never happened look identical to transmissions
    that were correctly suppressed. The silence from the window start to the
    first row and from the last row to the window end is checked too.
    """
    if not len(rows):
        return []
    ts = list(rows["rx_ts"])
    if w is not None:
        ts = [w["start"]] + ts + [w["end"]]
    out = []
    for prev, cur in zip(ts, ts[1:]):
        g = (cur - prev) / 1000.0
        if g > expected_interval_s * tolerance:
            out.append({"at": iso(cur), "gap_s": round(float(g), 1),
                        "expected_s": expected_interval_s})
    return out


def iso(ms):
    if ms is None or pd.isna(ms):
        return None
    return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M:%S")


# ---------------------------------------------------------------------------
# TCR
# ---------------------------------------------------------------------------
def tcr(base, adapt, wb, wa):
    """Counts over the phase windows (wall clock), never over device uptime."""
    b_hours = (wb["end"] - wb["start"]) / 3_600_000.0 if wb else 0.0
    a_hours = (wa["end"] - wa["start"]) / 3_600_000.0 if wa else 0.0
    b_n, a_n = len(base), len(adapt)

    b_rate = b_n / b_hours if b_hours else 0.0
    a_rate = a_n / a_hours if a_hours else 0.0

    # Rate-normalised is the honest form whenever the two phases differ in
    # length. When they are equal it reduces to the raw count ratio anyway.
    raw = (1 - a_n / b_n) * 100 if b_n else 0.0
    norm = (1 - a_rate / b_rate) * 100 if b_rate else 0.0
    equal_length = b_hours > 0 and abs(a_hours - b_hours) / b_hours < 0.05

    reasons = {str(k): int(v) for k, v in adapt["trigger"].value_counts().items()} if a_n else {}
    intervals = {}
    if a_n:
        iv = pd.to_numeric(adapt["interval"], errors="coerce").dropna()
        if len(iv):
            intervals = {
                "min_s": int(iv.min()), "max_s": int(iv.max()),
                "mean_s": round(float(iv.mean()), 1),
                "distinct": sorted(int(x) for x in iv.unique()),
            }

    # mean time between adaptive transmissions over the whole window
    mean_gap = a_hours * 3600.0 / a_n if a_n else 0.0

    return {
        "baseline": {"tx": b_n, "hours": round(b_hours, 3), "per_hour": round(b_rate, 2),
                     "window": wb["source"] if wb else None},
        "adaptive": {"tx": a_n, "hours": round(a_hours, 3), "per_hour": round(a_rate, 2),
                     "mean_gap_s": round(mean_gap, 1), "window": wa["source"] if wa else None},
        "tcr_raw_pct": round(raw, 2),
        "tcr_normalised_pct": round(norm, 2),
        "phases_equal_length": bool(equal_length),
        "headline_pct": round(raw if equal_length else norm, 2),
        "adaptive_reasons": reasons,
        "adaptive_interval": intervals,
        "baseline_gaps": gaps(base, BASELINE_INTERVAL_S, wb),
        "adaptive_gaps": gaps(adapt, intervals.get("max_s", 300) if intervals else 300, wa),
        "tx_dropped_rows": int(adapt["tx_dropped"].max()) if a_n else 0,
        "sensor_fault_rows": int((adapt["sensor_fault"] > 0).sum()) if a_n else 0,
    }


# ---------------------------------------------------------------------------
# Events and alert recall
# ---------------------------------------------------------------------------
EVENTS_TEMPLATE = """iso_time,event_type,note
2000-01-01 14:00:00,baseline-start,EXAMPLE ROW - replace with real times
2000-01-01 14:05:00,alert,target at 3.0 cm (about 92 %) - expect ALERT
2000-01-01 14:06:15,clear,target removed
2000-01-01 14:30:00,baseline-end,EXAMPLE ROW
"""


def to_epoch_ms(text):
    """Event time to epoch ms. rx_ts is true epoch ms, so a naive time must be
    read as local time; reading it as UTC (the old behaviour) shifted every
    event by the UTC offset, 5.5 h in Sri Lanka, and no event could match."""
    dt = datetime.fromisoformat(str(text).strip().replace("Z", "+00:00").replace("z", "+00:00"))
    if dt.tzinfo is None:
        dt = dt.astimezone()
    return int(dt.timestamp() * 1000)


def read_events(events_path):
    """(events DataFrame, None) or (None, status dict)."""
    if not os.path.exists(events_path):
        os.makedirs(os.path.dirname(events_path) or ".", exist_ok=True)
        with open(events_path, "w", encoding="utf-8") as f:
            f.write(EVENTS_TEMPLATE)
        return None, {"status": "no ground truth",
                      "template_written": events_path,
                      "note": "Log real event times in this file and re-run."}
    with open(events_path, encoding="utf-8-sig") as f:
        text = f.read()
    if text.replace("\r\n", "\n").strip() == EVENTS_TEMPLATE.strip():
        return None, {"status": "events file still contains the template rows",
                      "note": f"Replace the example rows in {events_path} with real "
                              "event times and re-run."}
    ev = pd.read_csv(events_path, encoding="utf-8-sig", skipinitialspace=True, dtype=str)
    ev.columns = [str(c).strip().lower() for c in ev.columns]
    for col in ("iso_time", "event_type"):
        if col not in ev.columns:
            sys.exit(f"ERROR: {events_path} has no '{col}' column. "
                     "Header must be: iso_time,event_type,note")
    ev["event_type"] = ev["event_type"].fillna("").astype(str).str.strip().str.lower()
    ev = ev[ev["iso_time"].notna() & (ev["iso_time"].astype(str).str.strip() != "")].copy()
    if ev.empty:
        return None, {"status": "events file is empty"}
    stamps = []
    for i, x in ev["iso_time"].items():
        try:
            stamps.append(to_epoch_ms(x))
        except ValueError:
            sys.exit(f"ERROR: {events_path} line {i + 2}: cannot read time '{x}'. "
                     "Use YYYY-MM-DD HH:MM:SS (local time), and edit the file in a text "
                     "editor rather than Excel.")
    ev["ts_ms"] = stamps
    unknown = sorted(set(ev["event_type"]) - KNOWN_EVENT_TYPES)
    if unknown:
        print(f"WARNING: unknown event_type values ignored: {unknown}. "
              f"Known: {sorted(KNOWN_EVENT_TYPES)}")
    if "note" not in ev.columns:
        ev["note"] = ""
    return ev.sort_values("ts_ms").reset_index(drop=True), None


def assign_events(ev, wb, wa):
    """Give every non-window event exactly one phase, or none.

    Logged windows are used as they are. A fallback window gets a 60 s margin.
    When an event is inside both margined windows, the one it is closest to
    (0 when inside) wins. Events within 60 s of a phase edge are flagged.
    """
    out = {"baseline": [], "adaptive": [], "unassigned": [], "near_edge": []}
    if ev is None:
        return out
    wins = {"baseline": wb, "adaptive": wa}
    for idx, e in ev[~ev["event_type"].isin(WINDOW_EVENT_TYPES)].iterrows():
        t = e["ts_ms"]
        best, best_d = None, None
        for name, w in wins.items():
            if w is None:
                continue
            margin = 0 if w["source"].startswith("logged") else 60_000
            if not (w["start"] - margin <= t <= w["end"] + margin):
                continue
            d = 0 if w["start"] <= t <= w["end"] else min(abs(t - w["start"]), abs(t - w["end"]))
            if best is None or d < best_d:
                best, best_d = name, d
        if best is None:
            out["unassigned"].append(iso(t))
            continue
        out[best].append(idx)
        w = wins[best]
        if e["event_type"] in ALERT_EVENT_TYPES and (t - w["start"] < 60_000 or w["end"] - t < 60_000):
            out["near_edge"].append(iso(t))
    return out


def recall(rows, ev, mode, slack_ms):
    """Alert recall for one phase.

    ADAPTIVE: an alert event is recalled when the node sends a row in state
    ALERT within 60 s (AoI_ALERT), whatever trigger carried it.
    BASELINE: the FSM does not run in BASELINE and the state field is frozen,
    so the test is whether a row reporting fill >= 80 % arrived within 60 s.
    The window opens slack_ms before the logged time, because the operator logs
    an event a few seconds after placing the target (latency is then negative).
    Within it, the first row reporting fill >= 80 % (the ALERT target itself) is
    preferred; then the first qualifying row at or after the logged time; then
    an earlier one. This keeps a fill-rate ALERT still held from an earlier step
    from being credited to the event.
    """
    alerts = ev[ev["event_type"].isin(ALERT_EVENT_TYPES)] if ev is not None else []
    if not len(alerts):
        return {"status": "no alert events in this phase", "events": 0}
    if mode == "ADAPTIVE":
        pool = rows[rows["state"] == "ALERT"]
        criterion = "row in state ALERT within 60 s"
    else:
        pool = rows[rows["fill_pct"] >= FILL_ALERT]
        criterion = "row reporting fill >= 80 % within 60 s"
    per_event = []
    for _, e in alerts.iterrows():
        t = e["ts_ms"]
        window = pool[(pool["rx_ts"] >= t - slack_ms) & (pool["rx_ts"] <= t + AOI_ALERT_MS)]
        full = window[window["fill_pct"] >= FILL_ALERT]
        after = window[window["rx_ts"] >= t]
        hit = full if len(full) else (after if len(after) else window)
        note = str(e.get("note", "") or "")
        if len(hit):
            first = hit.iloc[0]
            per_event.append({"event": iso(t), "note": note, "recalled": True,
                              "latency_s": round(float(first["rx_ts"] - t) / 1000.0, 1),
                              "row_id": int(first["id"]), "trigger": first["trigger"],
                              "fill_pct": None if pd.isna(first["fill_pct"]) else round(float(first["fill_pct"]), 1)})
        else:
            per_event.append({"event": iso(t), "note": note, "recalled": False,
                              "latency_s": None, "row_id": None, "trigger": None, "fill_pct": None})
    n = len(per_event)
    hits = sum(1 for d in per_event if d["recalled"])
    lats = [d["latency_s"] for d in per_event if d["recalled"]]
    return {
        "status": "measured",
        "criterion": criterion,
        "events": n,
        "recalled": hits,
        "recall_pct": round(100.0 * hits / n, 1),
        "latency_s": {"min": min(lats), "max": max(lats),
                      "mean": round(sum(lats) / len(lats), 1)} if lats else None,
        "per_event": per_event,
    }


# ---------------------------------------------------------------------------
# ALERT classification (ADAPTIVE)
# ---------------------------------------------------------------------------
def alert_cause(row):
    """Which urgency condition held, in the firmware's own priority order."""
    if pd.notna(row["fill_pct"]) and row["fill_pct"] >= FILL_ALERT:
        return "fill"
    if (pd.notna(row["gas_135"]) and row["gas_135"] > GAS135_THRESHOLD) or \
       (pd.notna(row["gas_5"]) and row["gas_5"] > GAS5_THRESHOLD):
        return "gas"
    return "fill-rate"


def classify_alerts(adapt, ev, slack_ms):
    """Classify every ADAPTIVE ALERT row by cause and decide which are false.

    fill and gas ALERTs report a real reading above a threshold. A fill-rate
    ALERT is real when the row's own dF/dt shows a rise of at least
    REAL_STEP_PCT in one velocity window (REAL_RATE_PCT_HR); it is deliberate
    when a logged event precedes it by at most EVENT_EXPLAINS_MS. A fill-rate
    ALERT below that rate (sensor jitter tripping the 10 %/hr rule) is false.
    An episode is a run of consecutive ALERT rows. It is false when the row
    that raised it is false; false_rows counts every false row.
    """
    al = adapt[adapt["state"] == "ALERT"]
    if not len(al):
        return {"rows": 0, "episodes": 0, "false_episodes": 0, "false_rows": 0,
                "by_cause": {}, "rate_basis": {}, "false_episode_list": [],
                "unlogged_step_rows": [], "rule": None}
    ev_ts = list(ev["ts_ms"]) if ev is not None and len(ev) else []
    labels = {}
    for idx, row in al.iterrows():
        cause = alert_cause(row)
        if cause != "fill-rate":
            labels[idx] = (cause, "threshold")
            continue
        t = row["rx_ts"]
        logged = any(e - slack_ms <= t <= e + EVENT_EXPLAINS_MS for e in ev_ts)
        if pd.notna(row["dfdt"]) and row["dfdt"] >= REAL_RATE_PCT_HR:
            labels[idx] = (cause, "logged-fill-step" if logged else "unlogged-fill-step")
        else:
            labels[idx] = (cause, "no-fill-change")

    # episodes: consecutive ALERT rows within the phase
    is_alert = (adapt["state"] == "ALERT").astype(int)
    episode_id = (is_alert.diff().fillna(is_alert) == 1).cumsum()
    episodes = []
    for _, grp in al.groupby(episode_id[al.index]):
        entry = grp.index[0]
        first = grp.iloc[0]
        episodes.append({"start": iso(first["rx_ts"]), "rows": len(grp),
                         "entry_cause": labels[entry][0],
                         "entry_basis": labels[entry][1],
                         "false": labels[entry][1] == "no-fill-change",
                         "entry_fill_pct": None if pd.isna(first["fill_pct"]) else round(float(first["fill_pct"]), 1),
                         "entry_dfdt": None if pd.isna(first["dfdt"]) else round(float(first["dfdt"]), 1)})
    by_cause, rate_basis = {}, {}
    for cause, basis in labels.values():
        by_cause[cause] = by_cause.get(cause, 0) + 1
        if cause == "fill-rate":
            rate_basis[basis] = rate_basis.get(basis, 0) + 1
    false_eps = [e for e in episodes if e["false"]]
    unlogged = [iso(al.loc[i, "rx_ts"]) for i, (c, b) in labels.items() if b == "unlogged-fill-step"]
    return {
        "rows": int(len(al)),
        "by_cause": by_cause,
        "rate_basis": rate_basis,
        "episodes": len(episodes),
        "episodes_by_entry_cause": {k: int(v) for k, v in
                                    pd.Series([e["entry_cause"] for e in episodes]).value_counts().items()},
        "false_episodes": len(false_eps),
        "false_rows": sum(1 for c, b in labels.values() if b == "no-fill-change"),
        "false_episode_list": false_eps,
        "unlogged_step_rows": unlogged,
        "rule": (f"fill >= {FILL_ALERT:.0f} % or gas above threshold: real reading. "
                 f"fill-rate: real if the row's dF/dt is >= {REAL_RATE_PCT_HR:.0f} %/hr "
                 f"(a rise of {REAL_STEP_PCT:.0f} % or more in one ~{VEL_WINDOW_S:.0f} s window); "
                 "otherwise false. An episode is false when the row that raised it is false."),
    }


# ---------------------------------------------------------------------------
# Measured power (INA219)
# ---------------------------------------------------------------------------
def power(rows):
    """Mean power and current from the node's 10 Hz energy counter.

    energy_mwh restarts at every boot, so it is differenced within each
    power_session and the deltas are summed. Rows that fail the backend's
    power-path check (supply outside 4.5-5.5 V, or current or power near zero:
    the shunt is not in the supply path) are excluded. Row-sampled current is
    also given, but rows are taken at transmit time, so it is a secondary figure.
    """
    ina = rows[(rows["ina_present"] == 1) & rows["energy_mwh"].notna()]
    valid = ina[(ina["bus_voltage_v"] >= POWER_PATH_V[0]) & (ina["bus_voltage_v"] <= POWER_PATH_V[1]) &
                (ina["current_ma"] > 1) & (ina["power_mw"] > 1)]
    excluded = len(ina) - len(valid)
    if len(ina) < 2:
        return {"status": "no INA219 data in this phase"}
    if len(valid) < 2:
        return {"status": "check power path: INA219 rows fail the 4.5-5.5 V / >1 mA check",
                "rows_excluded": excluded}
    sessions, total_mwh, total_h, bad = [], 0.0, 0.0, 0
    for sess, g in valid.groupby(valid["power_session"].fillna("legacy-unknown")):
        if len(g) < 2:
            continue
        g = g.sort_values("rx_ts")
        d_mwh = float(g["energy_mwh"].iloc[-1] - g["energy_mwh"].iloc[0])
        hours = (g["rx_ts"].iloc[-1] - g["rx_ts"].iloc[0]) / 3_600_000.0
        monotonic = bool((g["energy_mwh"].diff().dropna() >= 0).all())
        if not monotonic or d_mwh < 0 or hours <= 0:
            bad += 1
            continue
        sessions.append({"power_session": sess, "rows": len(g), "hours": round(hours, 3),
                         "energy_mwh": round(d_mwh, 3)})
        total_mwh += d_mwh
        total_h += hours
    if not total_h:
        return {"status": "no usable energy counter session", "sessions_rejected": bad,
                "rows_excluded": excluded}
    mean_mw = total_mwh / total_h
    mean_v = float(valid["bus_voltage_v"].mean())
    return {
        "status": "measured",
        "energy_mwh": round(total_mwh, 3),
        "hours": round(total_h, 3),
        "mean_power_mw": round(mean_mw, 1),
        "mean_bus_v": round(mean_v, 3),
        "mean_current_ma": round(mean_mw / mean_v, 1) if mean_v else None,
        "tx_row_current_ma": round(float(valid["current_ma"].mean()), 1),
        "sessions": sessions,
        "sessions_rejected": bad,
        "rows_excluded": excluded,
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
        "why": ("DEEP_SLEEP_ENABLED is 0 and no battery is discharged during the runs, "
                "so battery life cannot be observed. The INA219 measures power on the "
                "5 V input (see Measured power), but with deep sleep off that power is "
                "dominated by the MCU, the radio and the MQ-135 heater. This is a "
                "duty-cycle model driven by the measured transmission rate."),
        "model": m,
        "adaptive_mean_interval_s": round(adapt_mean_gap_s, 1),
        "blef_nominal": nominal["blef"],
        "blef_range": [min(r["blef"] for r in table), max(r["blef"] for r in table)],
        "sensitivity": table,
    }


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def recall_lines(name, r):
    if r.get("status") != "measured":
        return [f"- {name}: not measured ({r.get('status')})"]
    L = [f"- {name}: {r['recalled']}/{r['events']} alert events recalled "
         f"(**{r['recall_pct']}%**); criterion: {r['criterion']}"]
    if r["latency_s"]:
        L.append(f"  - latency from logged time: min {r['latency_s']['min']} s, "
                 f"mean {r['latency_s']['mean']} s, max {r['latency_s']['max']} s")
    for e in r["per_event"]:
        L.append(f"  - {e['event']}: " + (f"recalled, {e['latency_s']} s, `{e['trigger']}`, "
                                         f"fill {e['fill_pct']} %, row {e['row_id']}"
                                         if e["recalled"] else "**MISSED**"))
    return L


def markdown(res):
    t, r, b = res["tcr"], res["alert_recall"], res["blef"]
    L = ["# R26-IT-169 -- matched run results", "",
         f"Generated {res['generated']}, node **{res['node']}**, "
         f"labels `{res['labels']['baseline']}` / `{res['labels']['adaptive']}`", "",
         "## Transmission Count Reduction (measured)", "",
         "| Phase | Transmissions | Duration (h) | TX/hour | Window |",
         "|---|---|---|---|---|",
         f"| BASELINE (60 s fixed) | {t['baseline']['tx']} | {t['baseline']['hours']} | {t['baseline']['per_hour']} | {t['baseline']['window']} |",
         f"| ADAPTIVE (policy) | {t['adaptive']['tx']} | {t['adaptive']['hours']} | {t['adaptive']['per_hour']} | {t['adaptive']['window']} |",
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
        L += ["Measured against logged ground-truth event times.", ""]
        L += recall_lines("ADAPTIVE", r["adaptive"]) + recall_lines("BASELINE", r["baseline"])
        if r["unassigned_events"]:
            L.append(f"- Events outside both phases (ignored): {', '.join(r['unassigned_events'])}")
        if r["near_edge"]:
            L.append(f"- Alert events within 60 s of a phase edge (check): {', '.join(r['near_edge'])}")
        L.append("")
    else:
        L += [f"Not measured: {r.get('status')}. "
              "Requires a supervised session with logged ground-truth event times.", ""]

    c = res["alert_classes"]
    L += ["## ALERT rows by cause (ADAPTIVE, measured)", ""]
    if c["rows"]:
        L += [f"{c['rows']} ALERT rows in {c['episodes']} episodes. Rule: {c['rule']}", "",
              "| Cause | Rows |", "|---|---|"]
        for k, v in sorted(c["by_cause"].items(), key=lambda x: -x[1]):
            L.append(f"| {k} | {v} |")
        if c["rate_basis"]:
            L += ["", "| fill-rate ALERT basis | Rows |", "|---|---|"]
            for k, v in sorted(c["rate_basis"].items(), key=lambda x: -x[1]):
                L.append(f"| {k} | {v} |")
        L += ["", f"**False alerts: {c['false_episodes']} episodes ({c['false_rows']} rows)** "
                  "with no real fill change behind them."]
        for e in c["false_episode_list"][:10]:
            L.append(f"- {e['start']}: fill {e['entry_fill_pct']} %, dF/dt {e['entry_dfdt']} %/hr, {e['rows']} row(s)")
        if c["unlogged_step_rows"]:
            L.append(f"Fill-rate ALERTs after a real but unlogged fill rise (check these): "
                     f"{', '.join(c['unlogged_step_rows'][:10])}")
        L.append("")
    else:
        L += ["No ALERT rows in the adaptive phase.", ""]

    p_b, p_a = res["power"]["baseline"], res["power"]["adaptive"]
    L += ["## Measured power (INA219 on the 5 V input)", "",
          "| Phase | Energy (mWh) | Hours | Mean power (mW) | Mean current (mA) | Sessions |",
          "|---|---|---|---|---|---|"]
    for name, p in (("BASELINE", p_b), ("ADAPTIVE", p_a)):
        if p.get("status") == "measured":
            L.append(f"| {name} | {p['energy_mwh']} | {p['hours']} | {p['mean_power_mw']} | "
                     f"{p['mean_current_ma']} | {len(p['sessions'])} |")
        else:
            L.append(f"| {name} | {p.get('status')} | | | | |")
    if "measured" in (p_b.get("status"), p_a.get("status")):
        heater = " and the MQ-135 heater" if res.get("has_mq135") else ""
        L += ["", "Mean power is the 10 Hz on-node energy counter differenced within each boot "
                  f"session. It includes the MCU, the Wi-Fi radio{heater}."]
    L.append("")

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
          f"- Baseline gaps beyond 3x the expected interval (incl. window edges): {len(t['baseline_gaps'])}",
          f"- Adaptive gaps beyond 3x the longest T* (incl. window edges): {len(t['adaptive_gaps'])}",
          f"- Rows flagged sensor_fault: {t['sensor_fault_rows']}",
          f"- Peak tx_dropped counter: {t['tx_dropped_rows']}",
          f"- Mode-switch straddler rows excluded: "
          f"{res['straddlers']['baseline']} baseline, {res['straddlers']['adaptive']} adaptive",
          f"- Rows outside the logged windows excluded: "
          f"{res['clipped']['baseline']} baseline, {res['clipped']['adaptive']} adaptive", ""]
    return "\n".join(L)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--db", default="backend/smart-bin.db")
    p.add_argument("--node", default="bin01",
                   help="node_id to analyse; run labels are shared by every node")
    p.add_argument("--baseline-label", default="E1-baseline")
    p.add_argument("--adaptive-label", default="E1-adaptive")
    p.add_argument("--events", default="eval/events.csv")
    p.add_argument("--event-slack-s", type=float, default=30.0,
                   help="seconds before a logged event time that still count, for logging lag")
    p.add_argument("--out-dir", default="eval/out")
    a = p.parse_args()
    slack_ms = int(a.event_slack_s * 1000)

    df, all_rows, nodes = load(a.db, a.node)
    base, s_b = phase(df, a.baseline_label, "BASELINE")
    adapt, s_a = phase(df, a.adaptive_label, "ADAPTIVE")

    if not len(base) or not len(adapt):
        print(f"Labels present for node {a.node}:",
              sorted(str(x) for x in df["run_label"].dropna().unique()))
        sys.exit(f"ERROR: baseline rows={len(base)}, adaptive rows={len(adapt)}. "
                 "Check --node / --baseline-label / --adaptive-label.")

    ev, ev_status = read_events(a.events)
    wb = phase_window(all_rows, a.baseline_label, ev, "baseline")
    wa = phase_window(all_rows, a.adaptive_label, ev, "adaptive")
    base, c_b = clip(base, wb)
    adapt, c_a = clip(adapt, wa)

    assigned = assign_events(ev, wb, wa)
    ev_base = ev.loc[assigned["baseline"]] if ev is not None else None
    ev_adapt = ev.loc[assigned["adaptive"]] if ev is not None else None
    if ev is None:
        recall_res = ev_status
    else:
        rb = recall(base, ev_base, "BASELINE", slack_ms)
        ra = recall(adapt, ev_adapt, "ADAPTIVE", slack_ms)
        established = "measured" in (rb.get("status"), ra.get("status"))
        recall_res = {
            "status": "measured" if established else "no alert events inside either phase",
            "adaptive": ra,
            "baseline": rb,
            "unassigned_events": assigned["unassigned"],
            "near_edge": assigned["near_edge"],
        }

    t = tcr(base, adapt, wb, wa)
    profiles = " ".join(str(x) for x in df["sensor_profile"].dropna().unique())
    res = {
        "generated": datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z"),
        "db": os.path.abspath(a.db),
        "node": a.node,
        "nodes_in_db": nodes,
        "has_mq135": "mq135" in profiles,
        "labels": {"baseline": a.baseline_label, "adaptive": a.adaptive_label},
        "window": {"baseline_from": iso(wb["start"]) if wb else None, "baseline_to": iso(wb["end"]) if wb else None,
                   "baseline_source": wb["source"] if wb else None,
                   "adaptive_from": iso(wa["start"]) if wa else None, "adaptive_to": iso(wa["end"]) if wa else None,
                   "adaptive_source": wa["source"] if wa else None},
        "straddlers": {"baseline": s_b, "adaptive": s_a},
        "clipped": {"baseline": c_b, "adaptive": c_a},
        "tcr": t,
        "alert_recall": recall_res,
        "alert_classes": classify_alerts(adapt, ev_adapt, slack_ms),
        "power": {"baseline": power(base), "adaptive": power(adapt)},
        "blef": blef(t["adaptive"]["mean_gap_s"]),
    }

    os.makedirs(a.out_dir, exist_ok=True)
    with open(os.path.join(a.out_dir, "metrics.json"), "w", encoding="utf-8") as f:
        json.dump(res, f, indent=2)
    with open(os.path.join(a.out_dir, "metrics.md"), "w", encoding="utf-8") as f:
        f.write(markdown(res))

    # ---- stdout summary -------------------------------------------------
    print("=" * 68)
    print(f"R26-IT-169  matched run  -  node {a.node}")
    print("=" * 68)
    print(f"BASELINE  {t['baseline']['tx']:>5} tx over {t['baseline']['hours']:>5.2f} h "
          f"= {t['baseline']['per_hour']:>6.2f}/h   window: {t['baseline']['window']}")
    print(f"ADAPTIVE  {t['adaptive']['tx']:>5} tx over {t['adaptive']['hours']:>5.2f} h "
          f"= {t['adaptive']['per_hour']:>6.2f}/h   window: {t['adaptive']['window']}")
    print(f"\nTCR = {t['headline_pct']}%   (target >= 60%)")
    print(f"  raw {t['tcr_raw_pct']}% | normalised {t['tcr_normalised_pct']}% | "
          f"equal-length phases: {t['phases_equal_length']}")
    print(f"\nAdaptive transmissions by reason: {t['adaptive_reasons']}")
    if t["adaptive_interval"]:
        print(f"T* observed: {t['adaptive_interval']['distinct']}")

    r = res["alert_recall"]
    if r.get("status") == "measured":
        for name in ("adaptive", "baseline"):
            x = r[name]
            if x.get("status") == "measured":
                print(f"\nAlert Recall {name.upper()} = {x['recall_pct']}%  "
                      f"({x['recalled']}/{x['events']}); {x['criterion']}; latency {x['latency_s']}")
                for e in x["per_event"]:
                    print(f"    {e['event']}  " + (f"recalled {e['latency_s']} s  {e['trigger']}  "
                                                  f"fill {e['fill_pct']}  row {e['row_id']}"
                                                  if e["recalled"] else "MISSED"))
            else:
                print(f"\nAlert Recall {name.upper()}: {x.get('status')}")
        if r["unassigned_events"]:
            print(f"  events outside both phases: {r['unassigned_events']}")
        if r["near_edge"]:
            print(f"  alert events within 60 s of a phase edge: {r['near_edge']}")
    else:
        print(f"\nAlert Recall: {r.get('status')}")
    c = res["alert_classes"]
    print(f"\nADAPTIVE ALERT rows: {c['rows']} in {c['episodes']} episodes; by cause {c['by_cause']}; "
          f"fill-rate basis {c['rate_basis']}")
    print(f"False alerts (no real fill change behind them): {c['false_episodes']} episodes, "
          f"{c['false_rows']} rows")
    for name in ("baseline", "adaptive"):
        pw = res["power"][name]
        if pw.get("status") == "measured":
            print(f"Power {name.upper()}: {pw['mean_power_mw']} mW, {pw['mean_current_ma']} mA, "
                  f"{pw['energy_mwh']} mWh over {pw['hours']} h ({len(pw['sessions'])} session(s), "
                  f"{pw['rows_excluded']} row(s) failed the power-path check)")
        else:
            print(f"Power {name.upper()}: {pw.get('status')}")

    b = res["blef"]
    if b.get("blef_nominal"):
        print(f"\nBLEF = {b['blef_nominal']}x  (MODELLED, range "
              f"{b['blef_range'][0]}-{b['blef_range'][1]}x)   (target > 2x)")

    print("\n-- integrity --")
    print(f"  baseline gaps: {len(t['baseline_gaps'])}   adaptive gaps: {len(t['adaptive_gaps'])}")
    print(f"  sensor_fault rows: {t['sensor_fault_rows']}   peak tx_dropped: {t['tx_dropped_rows']}")
    print(f"  rows outside logged windows: {c_b} baseline, {c_a} adaptive")
    for g in (t["baseline_gaps"] + t["adaptive_gaps"])[:5]:
        print(f"    gap {g['gap_s']}s at {g['at']} (expected ~{g['expected_s']}s)")

    print("\n-- what these numbers do and do not prove --")
    print("  TCR is measured. It is the count ratio over equal wall-clock windows.")
    if not all(w and w["source"].startswith("logged") for w in (wb, wa)):
        print("  WARNING: a phase window is a fallback from row times. Log baseline-start/end")
        print("  and adaptive-start/end in the events file, or TCR uses a too-short ADAPTIVE span.")
    print("  If every adaptive transmission has reason='timer', this run measured")
    print("  the AoI floor and AIMD only; deadband and urgency were never exercised")
    print("  and need a separate supervised session.")
    if r.get("status") != "measured":
        print("  Alert Recall is NOT established. Do not claim 100% without it.")
    print("  BLEF is modelled from transmission rate, not measured. Say so on the slide.")
    print(f"\nWrote {a.out_dir}/metrics.json and {a.out_dir}/metrics.md")


if __name__ == "__main__":
    main()
