# Claude Code task: finalise, flash and prove the demo (R26-IT-169, viva Tue 20 Oct 4:00 pm)

Read `CLAUDE.md`, `HARDWARE.md` and `HANDOVER_2026-10-17.md` first. They are the source of truth for
pins, MACs, ports and decisions.

## How to run this prompt

Work through the steps in order. **After each step, stop and post:** what you did, the test or build
output, anything that failed, then ask **"Step N done. Continue to step N+1?"** and wait for "yes".
If a step fails, stop and ask whether to fix, roll back to the Step 0 snapshot, or skip.
Never change FSM thresholds or the policy logic unless a step below says so.

## Setup this prompt assumes

- **Edge laptop (this PC):** runs Mosquitto, the backend, the solar emulator and the database.
  It stays on the `Smart-Network` router with a **static IP 192.168.1.3**. Nothing about the
  firmware's broker address changes.
- **Presentation laptop (second PC):** only a browser and the slides. Nothing is installed on it.
  It joins `Smart-Network` and opens `http://192.168.1.3:3002`.
- No solar panel is fitted. Solar is **emulated on the gateway** (simulated harvest + measured
  INA219 load). The harvest-aware CONSERVE firmware rule (Part D of `PROMPT_solar_energy_model.md`)
  is **not** built; it stays as designed future work.

---

## Step 0: snapshot and run label

1. Make sure `.gitignore` excludes `node_modules/`, `*.db`, `*.db-wal`, `*.db-shm`, `firmware/.pio/`,
   `dashboard/dist/` and `firmware/include/secrets.h`. Then `git add -A` and commit
   "snapshot before viva finalisation". Confirm `secrets.h` is not in the commit.
2. Copy `backend/smart-bin-gateway.db*` into `backend/_db_backup_<today>/`.
3. With the backend running, set the run label: `POST /api/run {"label":"bench-<today>"}`.

## Step 1: the pending code changes

The user has made code changes that are not yet flashed. **Ask the user where they are** (this repo,
`firmware-bin02/`, or the other laptop) and what they change. Then:
1. Show the diff against the Step 0 snapshot, file by file, in plain language.
2. Flag anything that touches FSM thresholds, AoI floors, AIMD constants or the deadband formula, and
   ask before keeping it.
3. Build bin01 (`pio run -e esp32dev`). Build bin02 from wherever its firmware lives. Report warnings.
Do not flash yet.

## Step 2: reassemble check and flash

The user has reassembled both nodes. For each node, one at a time:
1. Ask the user to plug in only that node. Read its MAC with esptool and confirm it matches
   `HARDWARE.md` (bin01 `1c:c3:ab:fa:42:6c`, bin02 `84:0d:8e:b4:0a:42`). Stop if it does not match.
2. Flash with `-DMQTT_HOST_OVERRIDE=\"192.168.1.3\"`. If esptool fails auto-reset, use the PlatformIO
   upload path and retry once.
3. Listen to serial for 2 minutes. Confirm every sensor reads (HC-SR04 about 40 cm over an empty bin,
   DHT22, INA219, MQ-135 on bin01) and that a row lands in the backend.

## Step 3: two-laptop access

Change the backend so the presentation laptop can open the dashboard:
1. `server.js` listens on `process.env.BIND_HOST || '0.0.0.0'` instead of `127.0.0.1`.
2. Build the dashboard (`npm run build` in `dashboard/`) and serve `dashboard/dist` from Express at
   `/`, so the dashboard and API share one origin (no CORS changes needed for that path). In
   `App.jsx`, default `BACKEND` to `window.location.origin` when `VITE_BACKEND_URL` is not set and the
   page is not on port 5173. Keep `npm run dev` on 5173 working for development.
3. Allow `DASHBOARD_ORIGIN` to be a comma-separated list.
4. Add a Windows firewall rule for TCP 3002 (print the exact `netsh` command and ask the user to run it
   as admin if needed).
5. Test: from this PC open `http://192.168.1.3:3002`; ask the user to open the same URL on the
   presentation laptop and confirm live updates arrive.

## Step 4: solar emulation (gateway only)

Do **Part A** and **Part B** of `PROMPT_solar_energy_model.md` only. Skip Part C and Part D.
- Part A: simulated battery = previous level − measured INA219 energy + simulated solar harvest,
  accelerated day (24 h in about 4 min), published on `bin/{id}/battery`; energy card on the dashboard.
  Label it "emulated solar" in the UI.
- Part B: the node sends a `state-transition` message on every state change (one reflash of bin01,
  MAC check first).
Check: during the emulated night bin01 enters CONSERVE and the dashboard shows it within about 10 s;
at sunrise it returns to STABLE.

## Step 5: the evidence run (bin01)

With the HC-SR04 mounted over an empty bin and nobody moving in front of it:
1. Label `matched-baseline-<today>`, BASELINE mode, 30 minutes.
2. Label `matched-adaptive-<today>`, ADAPTIVE mode, 30 minutes, same setup. During the last 10 minutes
   ask the user to add fill twice (to above 60% and then above 80%) so ALERT recall is tested.
3. Label `heater-off-<today>`, ADAPTIVE, 10 minutes with the MQ-135 unplugged (ask the user).
4. Run `eval/analyze.py` and `eval/figures.py` on these labels. Write `eval/out/metrics.md` with:
   transmissions per hour in each mode, TCR, alert recall with latency and false positives, mean
   current and mWh per mode, and the heater-off current. Label every battery-life figure "modelled".

## Step 6: demo rehearsal script

Write `DEMO_SCRIPT.md`: the exact clicks and hand actions for a 4-minute live demo showing
STABLE → ACTIVE (target ~15 cm) → ALERT (target ~8 cm) → emulated night → CONSERVE → sunrise →
STABLE, plus BASELINE vs ADAPTIVE transmission counts side by side. Include a fallback line for
each step if the hardware misbehaves. Then stop.

## Do not

- Do not add ML, authentication or cloud MQTT.
- Do not delete or reset the database. `/api/reset` stays disabled.
- Do not install anything on the presentation laptop.
