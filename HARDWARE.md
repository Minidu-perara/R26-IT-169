# HARDWARE.md — R26-IT-169 bin nodes

Verified 17 Oct 2026 by serial capture, probe sketches (`tools/hw_probe/`) and the live backend.
Logs are in `eval/` (`hwcheck_bin01.log`, `hwprobe_bin01.log`, `hwcheck_bin02*.log`, `hwprobe_bin02.log`, `hwtcp_bin02.log`).

No solar panel or battery is fitted. Battery % is simulated over MQTT (`bin/{node_id}/battery`). Both nodes run from USB 5 V.

## bin01

| Item | Value |
|---|---|
| Board / chip | ESP32 dev board, ESP32-D0WD-V3 rev v3.1, 4 MB flash |
| MAC | 1c:c3:ab:fa:42:6c |
| USB bridge | CP2102 (CP210x), VID:PID 10C4:EA60, adapter serial 0001, **COM6** when plugged in alone |
| Firmware | `smart-bin-pp1/firmware/` (`[env:esp32dev]`, `NODE_ID "bin01"`) |
| Sensor profile | `fill_dht_mq135_ina219` |

Pin map (`firmware/include/config.h`):

| Sensor | Pins |
|---|---|
| HC-SR04 | TRIG 13, ECHO 14 |
| DHT22 | GPIO 4 |
| MQ-135 | GPIO 34 (ADC1) |
| MQ-5 | GPIO 35, **removed** (`ENABLE_MQ5 0`) |
| INA219 | I²C SDA 21, SCL 22, address 0x40 |

INA219 sits on the 5 V USB input (R100 shunt, 32 V / 2 A calibration). Current therefore includes the ESP32, Wi-Fi radio and the MQ-135 heater.

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

Measured 17 Oct 2026 (probe, 60 s, Wi-Fi off): 5.01 V, 29–30 mA; DHT22 29.9 °C, 79.8 %RH with no failed reads; HC-SR04 median 9–10 cm, 0/10 timeouts. Production rows with Wi-Fi: 5.04–5.10 V, 23–74 mA.

## Network and flashing

- Both boards join `Smart-Network` (2.4 GHz, 192.168.1.x). bin01 got .25, bin02 .26. The PC is 192.168.1.3. The Redmi hotspot is AP1 in `secrets.h` and is only used when `Smart-Network` is absent.
- Broker default in both firmware trees is `192.168.1.3`. Both production images were flashed with `-DMQTT_HOST_OVERRIDE=\"192.168.1.3\"`.
- Mosquitto must listen on all interfaces: `eval/mosquitto_lan.conf` (`listener 1883 0.0.0.0`, `allow_anonymous true`). The default config only listens on loopback.
- Windows rule for port 1883 exists (allow, all profiles). The Wi-Fi profile is still classed Public.
- Backend: `cd backend && npm start`, port **3002**. `better-sqlite3` was upgraded to 13.x for Node 24.
- `esptool` v5 often fails to auto-reset these boards ("wrong boot mode"). The PlatformIO upload path works. Retry once, then check the cable.

## Known issues

1. **bin02 fill reads unstable.** Production rows show 12 % → 0 % → 33 % within 30 s while the probe saw a steady ~9–10 cm. A literal 0 % may mean a failed ping is being substituted. Not investigated.
2. **bin01 DHT22** dropped 2 of 8 reads in the probe. Check the data-line pull-up and connections.
3. **bin01 HC-SR04 and bin02 HC-SR04** read about 10 cm, so the bin is not at the empty 40 cm reference during these checks.
4. **bin02 MQTT** failed with `rc=-2` on the first production flash and connected on the second flash. Cause not found. TCP to 192.168.1.3:1883 opened in 5–23 ms from the same board.
5. **bin01 MQ-135** raw is about 460, below the ~575 idle figure in `config.h`. Warm-up and ambient conditions differ. Not a fault.
6. ~~bin02 firmware lives outside this repo.~~ Fixed 19 Oct: it is now in `firmware-bin02/`.
7. **bin02 `timer` transmissions** appeared about every 62 s while the broker was down, not the 300 s STABLE interval. Not investigated.
8. **Active run label** is still `pp2-live-20260901`, so new rows carry it. Set a new label with `POST /api/run` before an evaluation run.
9. DB backup before the sqlite upgrade: `backend/_db_backup_20261017/`.
