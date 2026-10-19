// bin02 TCP reachability test: can this board open a socket to the broker?
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "C:/Users/test/Documents/ChatGPT/research project/node02-esp8266-firmware/include/secrets.h"

void setup() {
    Serial.begin(115200);
    delay(300);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_AP2_SSID, WIFI_AP2_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
    Serial.printf("\n[TCPTEST] wifi=%d ip=%s gw=%s rssi=%d\n", WiFi.status(),
                  WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
}

void loop() {
    const char* hosts[] = {"192.168.1.3", "192.168.1.1"};
    const int ports[] = {1883, 80};
    for (int i = 0; i < 2; i++) {
        WiFiClient c;
        c.setTimeout(4000);
        uint32_t t = millis();
        bool ok = c.connect(hosts[i], ports[i]);
        Serial.printf("[TCPTEST] %s:%d connect=%s (%lu ms)\n", hosts[i], ports[i], ok ? "OK" : "FAIL", millis() - t);
        c.stop();
    }
    delay(3000);
}
