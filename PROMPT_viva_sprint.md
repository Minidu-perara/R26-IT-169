# Claude Code task: viva sprint (R26-IT-169, final + viva + website viva Tue 20 Oct 2026, 4:00 pm, A304)

Read `CLAUDE.md`, `HARDWARE.md`, `PROMPT_finalize_and_flash.md` and `../../HANDOVER_2026-10-19.md` first.
This prompt **replaces the setup section and the step order** of `PROMPT_finalize_and_flash.md`.
The detail inside its steps still applies unless this file says otherwise.

## How to run this prompt

Work through the steps in order. **After each step, stop and post:** what you did, the build/test output,
anything that failed, then ask **"Step X done. Continue to step Y?"** and wait for "yes".
If a step fails, stop and ask: fix, roll back to the Step B snapshot, or skip.
Never change FSM thresholds, AoI floors, AIMD constants or the deadband formula.
No em dashes in any file you write.

## Roles (decided 19 Oct, late; supersedes the handover)

- **DEV laptop (this PC, where you run):** all code, builds, flashing over USB, the website, git.
  The repo should be at `C:\dev\Edge\smart-bin-pp1` (moved out of OneDrive). If it is still under a
  `OneDrive` path, stop and tell the user before anything else.
- **EDGE laptop (old `minidus-laptop`, static 192.168.1.3 on `Smart-Network`):** Mosquitto, backend on
  :3002, the live database, and it serves the dashboard. It goes to A304 for the demo.
- **You cannot run commands on the EDGE laptop.** For anything there, print the exact PowerShell
  commands in one block, ask the user to run them and paste the output, then check it.
- Code reaches the EDGE laptop through git: commit and push here, `git pull` there
  (remote `origin` = `github.com/Minidu-perara/R26-IT-169`). The live DB exists only on the EDGE laptop.
- Talk to the EDGE backend from here at `http://192.168.1.3:3002` (both PCs on `Smart-Network`).

---

## Step A: dev laptop check (no changes)

Check and report a table of: `git --version`, `node -v` (must be v24.x), `python --version`,
`pio --version` (or `%USERPROFILE%\.platformio\penv\Scripts\pio.exe --version`), `python -m esptool version`,
the repo path (no `OneDrive`, ideally no spaces), and whether `PLATFORMIO_CORE_DIR` is set.
If the Windows user folder has a space and a PlatformIO build fails on paths, suggest
`setx PLATFORMIO_CORE_DIR C:\.platformio` and a new terminal. Print install hints only for what is missing.

## Step B: snapshot (old step 0)

1. Check `.gitignore` covers `node_modules/`, `*.db`, `*.db-wal`, `*.db-shm`, `firmware/.pio/`,
   `firmware-bin02/.pio/`, `dashboard/dist/`, `firmware/include/secrets.h`, `firmware-bin02/include/secrets.h`.
2. `git add -A`, commit "snapshot before viva finalisation", confirm neither `secrets.h` is in it, push.
3. Print for the EDGE laptop: stop backend, copy `backend\smart-bin-gateway.db*` to
   `backend\_db_backup_<today>\`, `git pull`, `npm install` in `backend` and `dashboard`.

## Step C: code changes before the single flash (old step 1, extended)

1. **Pending changes:** ask the user where their unflashed changes are. Show each diff in plain language;
   flag anything touching thresholds, AoI floors, AIMD or the deadband and ask before keeping it.
2. **State-change message:** implement **Part B only** of `PROMPT_solar_energy_model.md` (publish a
   `state-transition` message on every FSM state change) in **both** `firmware/` and `firmware-bin02/`,
   plus the backend/dashboard handling it needs, so CONSERVE and STABLE changes show within seconds.
   No threshold or policy change. This is done now so each board is flashed once.
3. **bin02 fill fault:** check whether `firmware-bin02` turns a failed HC-SR04 ping into 0 % (known
   issue 1 in `HARDWARE.md`). If it does, port bin01's hold-last-good + `sensor_fault` logic
   (`CLAUDE.md`: never substitute a fill value). Show the diff and ask before keeping it.
4. Build both: `pio run -e esp32dev` in `firmware/`, `pio run -e node02_esp8266` in `firmware-bin02/`.
   Report warnings. Do not flash yet.

## Step D: two-laptop serving (old step 3, code part)

Do old step 3 items 1 to 3 (bind `0.0.0.0`, serve `dashboard/dist` from Express, `DASHBOARD_ORIGIN`
list, `BACKEND` default to `window.location.origin`). Keep `npm run dev` on 5173 working.
Build the dashboard here to prove it compiles. Commit and push.
Print for the EDGE laptop: `git pull`, `npm install` + `npm run build` in `dashboard`, start Mosquitto
with `eval\mosquitto_lan.conf`, `npm start` in `backend`, firewall rules for 1883 and 3002 if missing.
Test from here: `http://192.168.1.3:3002/api/health` and open `http://192.168.1.3:3002` in a browser.

## Step E: flash both nodes (old step 2)

The user has reassembled both nodes. One node at a time:
1. Ask the user to switch the node's battery **off** (or unplug its power bank) and connect only that
   board by USB. Read the MAC with esptool and match `HARDWARE.md`. Stop on mismatch.
2. Flash with `-DMQTT_HOST_OVERRIDE=\"192.168.1.3\"` via the PlatformIO upload path; retry once.
3. Serial for 2 minutes: every sensor reads (HC-SR04 about 40 cm over the empty bin, DHT22, INA219,
   MQ-135 on bin01). Confirm new rows for that node at `http://192.168.1.3:3002/api/readings`.
4. Ask the user to unplug USB and switch the battery on; confirm rows keep arriving on battery power.

## Step F: evidence run (old step 5, driven from here)

Set labels with `POST http://192.168.1.3:3002/api/run`. bin01 mounted over an empty bin, nobody moving.
1. `matched-baseline-<today>`: BASELINE, 30 min.
2. `matched-adaptive-<today>`: ADAPTIVE, 30 min. In the last 10 min ask the user to add fill twice
   (above 60 %, then above 80 %) to test alert recall.
3. Optional, only if the user says there is time: `heater-off-<today>`, ADAPTIVE, 10 min, MQ-135 unplugged.
4. Get the data: print for the EDGE laptop, in `backend`:
   `node -e "require('better-sqlite3')('smart-bin-gateway.db').backup('eval-snapshot.db').then(()=>console.log('ok'))"`
   then ask the user to copy `eval-snapshot.db` to this PC (USB stick or shared folder).
5. Run `eval/analyze.py` and `eval/figures.py` on that snapshot and these labels. Write
   `eval/out/metrics.md`: transmissions/hour per mode, TCR, alert recall with latency and false
   positives, mean current and mWh per mode, heater-off current if run. Label every figure
   **measured**, **modelled** or **inferred**; every battery-life figure is "modelled".

## Step G: website (`../research-website/`)

The guide is `Research\Markings and Schema\5. Website\Website Design Guide.pdf`: tabs Home, Domain,
Milestones, Documents, Presentations, About, Contact; HTML/CSS only; max 20 MB. Edit
`website-src/build.py`, then rebuild with `python build.py ..\website`. Changes:
1. Presentations: link the proposal presentation (`Research\Presenations\R26-IT-169_Proposal_Presentation.pdf`)
   and PP2 (`R26-IT-169_PP2.pptx`, or a PDF export if the user provides one); copy them into `website/slides/`.
   Final presentation stays "Available after submission" until the user gives the file.
2. Documents: add the end-of-May bi-weekly log (`Research\Logs\R26-IT-169_BiWeekly_May_EndOfMonth.docx`,
   as PDF if LibreOffice/Word export is available, else the docx).
3. Add a short **Findings** section (Home or Domain) from `eval/out/metrics.md`, keeping the
   measured / modelled labels. If Step F is not done, ask before adding anything.
4. About: add a photo slot (`img/researcher.jpg`) only if the user provides a photo.
5. Write a link checker: every local `href`/`src` in `website/` resolves to a file. Report total size.
6. Zip `website/` to `R26-IT-169_website.zip` next to it for upload.

## Step H: demo script (old step 6, without solar)

Write `DEMO_SCRIPT.md`: exact clicks and hand actions for a 4-minute demo on the EDGE laptop's dashboard:
STABLE, ACTIVE (target about 15 cm), ALERT (about 8 cm), battery slider to 25 % so CONSERVE shows
(via the state-change message), back to STABLE, then BASELINE vs ADAPTIVE transmission counts side by side.
One fallback line per step. List the headline numbers from `metrics.md` to say aloud. Then stop.

## Skipped unless the user asks after Step H

- Solar emulation (Part A of `PROMPT_solar_energy_model.md`): present as designed future work.
- Harvest-aware CONSERVE rule (Part D): future work.

## Do not

- Do not add ML, authentication or cloud MQTT.
- Do not delete or reset any database. `/api/reset` stays disabled.
- Do not commit `secrets.h` or any `.db` file.
