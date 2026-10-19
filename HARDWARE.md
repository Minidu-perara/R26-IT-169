# HARDWARE.md — R26-IT-169 bin nodes

Verified 17 Oct 2026 by serial capture, probe sketches (`tools/hw_probe/`) and the live backend.
Logs are in `eval/` (`hwcheck_bin01.log`, `hwprobe_bin01.log`, `hwcheck_bin02*.log`, `hwprobe_bin02.log`, `hwtcp_bin02.log`).

No solar panel is fitted. Battery % is simulated over MQTT (`bin/{node_id}/battery`). Since 20 Oct each node runs from a power bank, and the INA219 sits on the power bank's supply path. Laptop USB power (flashing, serial) bypasses the INA219, so it reads about 0 mA then: run every evaluation on the power bank with USB unplugged.

Bin depth is per node (`BIN_DEPTH_CM` in each `include/config.h`): set it 1.5–2 cm shorter than the measured empty-bin distance, so echo jitter at empty clamps to 0 % instead of tripping the 10 %/hr fill-rate ALERT. Fill = (BIN_DEPTH_CM − d) / (BIN_DEPTH_CM − 2) × 100.

## bin01

| Item | Value |
|---|---|
| Board / chip | ESP32 dev board, ESP32-D0WD-V3 rev v3.1, 4 MB flash |
| MAC | 1c:c3:ab:fa:42:6c |
| USB bridge | CP2102 (CP210x), VID:PID 10C4:EA60, adapter serial 0001, **COM3** on the DEV laptop (COM6 on the old laptop) |
| Firmware | `smart-bin-pp1/firmware/` (`[env:esp32dev]`, `NODE_ID "bin01"`) |
| Sensor profile | `fill_dht_mq135_ina219` |
| Bin depth | `BIN_DEPTH_CM 14.0` (empty bin reads 15.8–16.8 cm, 20 Oct). 60 % = 6.8 cm, 80 % = 4.4 cm; demo targets 5.5 cm (71 %, ACTIVE) and 3.0 cm (92 %, ALERT) |

Pin map (`firmware/include/config.h`):

| Sensor | Pins |
|---|---|
| HC-SR04 | TRIG 13, ECHO 14 |
| DHT22 | GPIO 4 |
| MQ-135 | GPIO 34 (ADC1) |
| MQ-5 | GPIO 35, **removed** (`ENABLE_MQ5 0`) |
| INA219 | I²C SDA 21, SCL 22, address 0x40 |

INA219 sits on the power bank's 5 V supply path (R100 shunt, 32 V / 2 A calibration). Current therefore includes the ESP32, Wi-Fi radio and the MQ-135 heater. On laptop USB power it is bypassed and reads about 0 mA (20 Oct: 4.24 V, −0.3 to 0.1 mA).

Measured 20 Oct 2026, reassembled, on laptop USB (probe sketch, 35 s, `eval/hwcheck_bin01_20oct_empty.log`; full check in `eval/hwcheck_bin01_20oct.log`): HC-SR04 over the empty bin 15.8–16.8 cm with 0/10 timeouts; DHT22 31.1 °C, 76.4 %RH; MQ-135 raw 1088–1113; INA219 found at 0x40.

Measured 17 Oct 2026 (probe, 60 s):

| Sensor | Value |
|---|---|
| INA219 | 4.86 V, 165 mA (production rows: 4.81 V, 187 mA) |
| DHT22 | 30.1 °C, 80.1 %RH (2 of the last 8 probe reads failed) |
| HC-SR04 | median about 10 cm, 0–1 timeouts per 10 (a target was in front of it, not the empty ~40 cm) |
| MQ-135 | raw 454–479 (spec idle ~575) |

## bin02

| Item | Value |
|---|---|
| Board / chip | ESP8266 ESP-12E (NodeMCU-style), ESP8266EX, 4 MB flash |
| MAC | 84:0d:8e:b4:0a:42 |
| USB bridge | CP210x, VID:PID 10C4:EA60, adapter serial 0001 (same serial as bin01's; identify by MAC) |
| Firmware | `firmware-bin02/` in this repo (`[env:node02_esp8266]`, `NODE_ID "bin02"`). Copied in on 19 Oct 2026. |
| Sensor profile | `fill_dht_ina219` |

Pin map (node02 `include/config.h`):

| Sensor | Pins |
|---|---|
| HC-SR04 | TRIG 13, ECHO 14 |
| DHT22 | GPIO 4 |
| INA219 | I²C SDA 12, SCL 5, address 0x40 |
| MQ-135 / MQ-5 | not fitted |

None of these is a boot-strapping pin (GPIO 0, 2, 15). INA219 sits on the 5 V USB input.

Measured 17 Oct 2026 (probe, 60 s, Wi-Fi off): 5.01 V, 29–30 mA; DHT22 29.9 °C, 79.8 %RH; HC-SR04 10-ping medians mostly 9–12 cm, but some cycles read 50.7, 50.1, 31.5 and 24.9 cm with 0/10 timeouts (`hwprobe_bin02.log` lines 51, 87, 107, 147), so the target was not always in the beam. Production rows with Wi-Fi: 5.04–5.10 V, 23–74 mA.

## Network and flashing

- Both boards join `Smart-Network` (2.4 GHz, 192.168.1.x). bin01 got .25, bin02 .26. The PC is 192.168.1.3. The Redmi hotspot is AP1 in `secrets.h` and is only used when `Smart-Network` is absent.
- Broker default in both firmware trees is `192.168.1.3`. Both production images were flashed with `-DMQTT_HOST_OVERRIDE=\"192.168.1.3\"`.
- Mosquitto must listen on all interfaces: `eval/mosquitto_lan.conf` (`listener 1883 0.0.0.0`, `allow_anonymous true`). The default config only listens on loopback.
- Windows rule for port 1883 exists (allow, all profiles). The Wi-Fi profile is still classed Public.
- Backend: `cd backend && npm start`, port **3002**. `better-sqlite3` was upgraded to 13.x for Node 24.
- `esptool` v5 often fails to auto-reset these boards ("wrong boot mode"). The PlatformIO upload path works. Retry once, then check the cable.

## Known issues

1. **bin02 fill reads unstable: real echoes, not a substituted 0.** Production rows show 12 % → 0 % → 33 % within 30 s. Checked 20 Oct: the 0 % rows are valid echoes from 40 cm or further (the room behind the bench, about 50 cm), which `readFillPct()` clamps to 0 %. A failed ping does not produce 0 %: `firmware-bin02` holds the last good value (null before the first valid echo) and sets `sensor_fault` after 5 consecutive faults, like bin01. Evidence: `hwcheck_bin02_prod2.log` lines 11–12 (the 0 % packet was sent as a deadband change of 12.2 % from the held 12.25 %); on 17 Oct, 123 of 311 bin02 rows read 0 % and none was null or flagged `sensor_fault`; the probe also saw 50 cm medians (see above). Fix: keep the target squarely in the beam, and set `BIN_DEPTH_CM` 1.5–2 cm shorter than the measured empty distance (see the top of this file). Any reading shorter than the depth is fill, and even a 0.3 cm jitter there is a fill rate far above the 10 %/hr ALERT threshold.
2. **bin01 DHT22** dropped 2 of 8 reads in the probe. Check the data-line pull-up and connections.
3. **bin01 HC-SR04 and bin02 HC-SR04** read about 10 cm, so the bin is not at the empty 40 cm reference during these checks.
4. **bin02 MQTT** failed with `rc=-2` on the first production flash and connected on the second flash. Cause not found. TCP to 192.168.1.3:1883 opened in 5–23 ms from the same board.
5. **bin01 MQ-135** raw is about 460, below the ~575 idle figure in `config.h`. Warm-up and ambient conditions differ. Not a fault.
6. ~~bin02 firmware lives outside this repo.~~ Fixed 19 Oct: it is now in `firmware-bin02/`.
7. **bin02 `timer` transmissions about every 62 s: explained, not a fault.** The 9–10 cm target is about 79–82 % fill, on the 80 % ALERT threshold, so bin02 entered ALERT on its first tick (`hwcheck_bin02.log`: the first attempt at 41.8 s was `urgent-transition`, the next `timer` at 102.5 s). ALERT reports every 60 s (`AoI_ALERT`); one loop pass adds the extra 1–2 s. With the bin empty it would use the STABLE timer.
8. **Active run label** is still `pp2-live-20260901`, so new rows carry it. Set a new label with `POST /api/run` before an evaluation run.
9. DB backup before the sqlite upgrade: `backend/_db_backup_20261017/`.
