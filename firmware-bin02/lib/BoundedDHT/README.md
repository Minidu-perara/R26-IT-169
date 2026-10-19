# Bounded DHT driver

Derived from the project's installed Adafruit DHT sensor library (version=1.4.7).
Original author notices and MIT license are retained. No unified-sensor wrapper is used.

The original ESP32 crash backtrace reached DHT::expectPulse while interrupts were
disabled. This local copy uses elapsed microseconds (1 ms limit per pulse),
aborts on the first failed bit pulse, and stops collecting a frame after 10 ms
(at most two additional 1 ms pulse waits before the next frame check).
The interrupt lock is released on every return. Watchdogs remain enabled.
Invalid reads keep the upstream NAN/false result and 2-second read spacing.
The firmware reports unavailable temperature/humidity rather than inventing values.

Header renamed to BoundedDHT.h and the upstream lib_deps entries removed to
ensure a future clean build uses this checked-in fix, not a modified cache.
Do not add the upstream DHT library alongside this copy: both define class DHT.

Node02 port: the elapsed-microsecond pulse bounds also apply to ESP8266. Hardware validation remains pending.
