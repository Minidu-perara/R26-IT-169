# bin02 — ESP8266 / ESP-12E, HC-SR04 and DHT22

Prepared 31 August 2026; physically uploaded and connected on 1 September. The supplied photo shows a NodeMCU-style ESP-12E USB development board. COM6 was positively identified as ESP8266EX, MAC `84:0d:8e:b4:0a:42`. After a verified original-flash backup, this project firmware was uploaded and produced independently stored bin02 telemetry at the laptop gateway. BASELINE reports arrived 60.243 seconds apart; ADAPTIVE restoration was confirmed. The user has confirmed the sensors were connected and HC-SR04 was covered/uncovered with hand movements, explaining the changing readings. Supply rails, precise wiring verification, fixed-target stability and calibration remain pending; this is not a completed accuracy or energy evaluation. Insulate the exposed resistor joints with power disconnected. See [current bin02 status](../NODE02_STATUS.md).

## Selected hardware

- Existing NodeMCU ESP8266 board; Micro-USB data cable.
- HC-SR04 ultrasonic sensor and **DHT22** temperature/humidity sensor. If the purchased DHT is a DHT11, the firmware sensor type must be changed before testing.
- Breadboard, jumper wires, and an Echo voltage divider or a suitable level shifter.
- For a bare DHT sensor, a data pull-up resistor unless the purchased breakout already includes it.
- No gas sensors and no extra ADC are required for this selected profile.
- Power the development board through Micro-USB from a regulated USB supply/power bank for PP2. A raw cell must not be connected until its voltage, protection and conversion circuitry are established.

## Firmware pin map

These NodeMCU label mappings were checked against the installed Arduino core's `variants/nodemcu/pins_arduino.h`.

| Signal | GPIO | NodeMCU marking |
|---|---:|---|
| HC-SR04 TRIG | 13 | D7 |
| HC-SR04 ECHO, after voltage reduction | 14 | D5 |
| DHT22 DATA | 4 | D2 |

Use a common ground. The ESP GPIOs use 3.3 V logic. Do not connect the HC-SR04's 5 V Echo directly to D5. With the user's available resistor types, use 2.2 kΩ from Echo to the D5 junction, then 2.2 kΩ plus 1 kΩ in series from the junction to ground. This reduces nominal 5 V to approximately 2.96 V. It needs two red-red-red-gold resistors and one brown-black-red-gold resistor. Do not substitute a single 2.2 kΩ for the ground resistor in the earlier 1 kΩ / 2 kΩ suggestion; that gives less voltage margin. Verify values and connections before power-up.

The HC-SR04 needs a suitable 5 V supply. Do not run it from the board's 3V3 rail. Confirm the actual board's available 5 V supply pin before connecting it: clone boards differ, and the photo alone does not establish the power circuit. Avoid unintentionally joining two power supplies or backfeeding USB. DHT wiring must follow the markings of the actual bare sensor or breakout, not an assumed module pin order.

For the DHT22, start with 3.3 V power and a 10 kΩ data pull-up to 3.3 V where required. Do not pull an ESP input up to 5 V. See [Adafruit's DHT wiring guide](https://learn.adafruit.com/dht/connecting-to-a-dhtxx-sensor) and [Espressif's GPIO voltage guidance](https://docs.espressif.com/projects/esp-faq/en/latest/hardware-related/hardware-design.html#what-is-the-voltage-tolerance-of-gpios-of-esp-chips).

## Software behavior

`bin02` publishes on `bin/bin02/data`. Gas readings are JSON null, not fabricated zero measurements. The shared FSM file is byte-identical to node01. Gas branches are disabled internally because those sensors are not fitted. This profile does not implement RTC persistence or deep sleep.

The router SSID is `Smart-Network ` with one trailing space. MQTT points to laptop `192.168.1.3:1883`. Credentials are private in ignored `include/secrets.h`; do not include that file in slides, public repositories or shared archives.

Gateway leases use the same bounded protocol as node01. They never extend the local interval, do not affect BASELINE, and do not shorten ALERT or CONSERVE policy intervals. Gateway acknowledgments use a separate topic and are excluded from sensor-message counts.

## Build and upload

Build from this directory:

```powershell
& 'C:\Users\test\.platformio\penv\Scripts\python.exe' -m platformio run -e node02_esp8266
```

Identify the attached chip before every upload; the COM number is not a board identity. **COM6 was the ESP32 node01 on 31 August, but on 1 September it was the ESP8266EX, MAC `84:0d:8e:b4:0a:42`, and the verified bin02 build was uploaded there.** Node01's MAC is `1c:c3:ab:fa:42:6c`.

After assembly: check supply voltages and common ground; verify real fill/DHT readings; calibrate empty/full sensor distances for the actual bin; confirm `bin02` in the dashboard; verify controls affect only bin02; then run baseline/adaptive comparisons separately within each board. Do not compare ESP32 baseline power against ESP8266 adaptive power as though only the algorithm changed.
