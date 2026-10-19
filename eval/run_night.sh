#!/bin/bash
# Overnight BASELINE vs ADAPTIVE evaluation run.
# BASELINE_SECS=10800, ADAPTIVE_SECS=10800, battery drops 85 -> 65 at the
# midpoint of each phase (BATT_STEP_AT=5400). 30-min row-count snapshots.
# [RUN-ALERT] on any non-2xx API response or zero new rows since the last
# snapshot.
set -uo pipefail

NODE_ID=bin01
API=http://localhost:3001
BASELINE_SECS=10800
ADAPTIVE_SECS=10800
BATT_STEP_AT=5400
SNAPSHOT_EVERY=1800

ts() { date '+%Y-%m-%d %H:%M:%S'; }

alert() {
    echo "[RUN-ALERT] $(ts) $1"
}

post() {
    local path=$1 body=$2
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$API$path" \
        -H "Content-Type: application/json" -d "$body")
    if [[ ! "$code" =~ ^2 ]]; then
        alert "POST $path body=$body -> http=$code"
    fi
    echo "$(ts) POST $path $body -> $code"
}

total_rows() {
    local stats code
    stats=$(curl -s -o /tmp_stats.$$ -w '%{http_code}' "$API/api/stats")
    code=$stats
    if [[ ! "$code" =~ ^2 ]]; then
        alert "GET /api/stats -> http=$code"
        rm -f /tmp_stats.$$
        echo "-1"
        return
    fi
    local body
    body=$(cat /tmp_stats.$$)
    rm -f /tmp_stats.$$
    local a b
    a=$(echo "$body" | grep -o '"tx_count_adaptive":[0-9]*' | grep -o '[0-9]*')
    b=$(echo "$body" | grep -o '"tx_count_baseline":[0-9]*' | grep -o '[0-9]*')
    echo "$(( ${a:-0} + ${b:-0} ))"
}

run_phase() {
    local label=$1 mode=$2 phase_secs=$3
    post "/api/run" "{\"label\":\"$label\"}"
    post "/api/mode/$NODE_ID" "{\"mode\":\"$mode\"}"
    post "/api/battery" "{\"value\":85}"

    local start=$SECONDS
    local last_total
    last_total=$(total_rows)
    echo "$(ts) [$label] phase start, baseline row total=$last_total"

    local batt_stepped=0
    while (( SECONDS - start < phase_secs )); do
        sleep "$SNAPSHOT_EVERY"
        local elapsed=$(( SECONDS - start ))

        if (( elapsed >= BATT_STEP_AT && batt_stepped == 0 )); then
            post "/api/battery" "{\"value\":65}"
            batt_stepped=1
        fi

        local now_total
        now_total=$(total_rows)
        echo "$(ts) [$label] t=${elapsed}s total_rows=$now_total"

        if [[ "$now_total" == "-1" ]]; then
            : # already alerted inside total_rows
        elif [[ "$now_total" == "$last_total" ]]; then
            alert "$label: zero new rows since last snapshot (t=${elapsed}s, total=$now_total)"
        fi
        last_total=$now_total
    done
    echo "$(ts) [$label] phase complete"
}

echo "$(ts) === run_night.sh starting ==="
run_phase "baseline" "BASELINE" "$BASELINE_SECS"
run_phase "adaptive" "ADAPTIVE" "$ADAPTIVE_SECS"
echo "$(ts) === run_night.sh complete ==="
