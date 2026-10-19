#pragma once
// Bounded monitoring lease layered over, not written into, the node FSM.
// Include after mqtt, fsm, baselineMode and simulatedBattPct are declared.
struct GatewayLease {
    bool active = false;
    uint32_t startedMs = 0;
    uint32_t durationMs = 0;
    uint32_t intervalMs = 0;
    char requestId[33] = {};
};
static GatewayLease gatewayLease;

static bool gatewayProtected() {
    return baselineMode || simulatedBattPct < BATT_DEAD ||
           fsm.state == State::ALERT || fsm.state == State::CONSERVE;
}
static bool gatewayLive(uint32_t now) {
    return gatewayLease.active && (uint32_t)(now - gatewayLease.startedMs) < gatewayLease.durationMs;
}
static uint32_t gatewayInterval(uint32_t localMs, uint32_t now) {
    return gatewayLive(now) && !gatewayProtected() ? min(localMs, gatewayLease.intervalMs) : localMs;
}
static void gatewayAck(const char* requestId, const char* status) {
    JsonDocument doc;
    const uint32_t now = millis();
    doc["node_id"] = NODE_ID;
    doc["request_id"] = requestId;
    doc["status"] = status;
    doc["ts"] = now;
    doc["mode"] = baselineMode ? "BASELINE" : "ADAPTIVE";
    doc["effective_s"] = (baselineMode ? BASELINE_INTERVAL : gatewayInterval(fsm.txInterval, now)) / 1000;
    doc["policy_s"] = (baselineMode ? BASELINE_INTERVAL : fsm.txInterval) / 1000;
    char message[320];
    size_t size = measureJson(doc);
    const bool fits = !doc.overflowed() && size < sizeof(message);
    if (fits) serializeJson(doc, message, sizeof(message));
    bool sent = fits && mqtt.connected() && mqtt.publish("bin/" NODE_ID "/ack", message, size);
    Serial.printf("[GATEWAY] %s id=%s ack=%s\n", status, requestId, sent ? "published" : "failed");
}
static void gatewayCancel() {
    if (!gatewayLease.active) return;
    gatewayLease.active = false;
    gatewayAck(gatewayLease.requestId, "cancelled");
}
static void gatewayService(uint32_t now) {
    if (gatewayLease.active && !gatewayLive(now)) {
        gatewayLease.active = false;
        gatewayAck(gatewayLease.requestId, "expired");
    }
}
static void gatewayHandle(const byte* payload, unsigned int length) {
    if (length > 192) { Serial.println("[GATEWAY] rejected oversized request"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) { Serial.println("[GATEWAY] rejected malformed JSON"); return; }
    const char* id = doc["request_id"] | "";
    const size_t idLen = strlen(id);
    if (!idLen || idLen > 32) return;
    for (size_t i=0; i<idLen; ++i) {
        const char c = id[i];
        if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_')) return;
    }
    if (!doc["interval_s"].is<uint32_t>() || !doc["ttl_s"].is<uint32_t>()) { gatewayAck(id,"rejected-invalid"); return; }
    const uint32_t interval = doc["interval_s"].as<uint32_t>();
    const uint32_t ttl = doc["ttl_s"].as<uint32_t>();
    if (interval<60 || interval>300 || ttl<60 || ttl>600 || ttl<interval) { gatewayAck(id,"rejected-invalid"); return; }
    if (baselineMode) { gatewayAck(id,"rejected-baseline"); return; }
    if (gatewayProtected()) { gatewayAck(id,"rejected-protected"); return; }
    if (strcmp(gatewayLease.requestId,id)==0) {
        gatewayAck(id,gatewayLive(millis()) ? "duplicate" : "rejected-invalid");
        return; // a duplicate never extends its deadline
    }
    if (gatewayLease.active) gatewayCancel();
    memcpy(gatewayLease.requestId,id,idLen+1);
    gatewayLease.intervalMs=interval*1000UL;
    gatewayLease.durationMs=ttl*1000UL;
    gatewayLease.startedMs=millis();
    gatewayLease.active=true;
    gatewayAck(id,"accepted");
}
static void gatewayTelemetry(JsonDocument& doc, bool isBaseline, float battPct) {
    const uint32_t now=millis();
    const bool active=gatewayLive(now);
    const uint32_t localMs=isBaseline ? BASELINE_INTERVAL : fsm.txInterval;
    const uint32_t effectiveMs=isBaseline ? BASELINE_INTERVAL : gatewayInterval(localMs,now);
    FSMContext next=fsm;
    if (!isBaseline) fsmOnTransmit(next,battPct); // preview only; original FSM is untouched
    doc["control_version"]=1;
    doc["policy_interval"]=localMs/1000;
    doc["interval"]=effectiveMs/1000;
    doc["next_interval"]=(isBaseline ? BASELINE_INTERVAL : gatewayInterval(next.txInterval,now))/1000;
    doc["control_source"]=effectiveMs<localMs ? "gateway" : "local";
    if (active) {
        doc["control_id"]=gatewayLease.requestId;
        doc["control_ttl"]=(gatewayLease.durationMs-(uint32_t)(now-gatewayLease.startedMs)+999)/1000;
    }
}
