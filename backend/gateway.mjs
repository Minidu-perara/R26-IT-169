// Deterministic local processing. This is not shared-uplink budget arbitration.
export const validNodeId = value => typeof value === 'string' && /^[A-Za-z0-9_-]{1,32}$/.test(value);
const finite = value => typeof value === 'number' && Number.isFinite(value);

export function describeNode(row, now = Date.now()) {
  const age = finite(row.rx_ts) ? Math.max(0, (now - row.rx_ts) / 1000) : null;
  // Lease expiry and the next AIMD step may lengthen the next interval. Use
  // the local policy bound too, to avoid flagging an expired lease as an outage.
  const expected = Math.max(60, ...[row.interval, row.policy_interval, row.next_interval].filter(finite));
  const staleAfter = expected + Math.max(15, expected * 0.25);
  const stale = age === null || age > staleAfter;
  const fault = !!row.sensor_fault || !finite(row.fill_pct);
  const urgent = row.state === 'ALERT' || (finite(row.fill_pct) && row.fill_pct >= 80);
  const status = stale ? 'STALE' : row.state === 'DEAD' ? 'DEAD' : fault ? 'SENSOR_FAULT' : urgent ? 'URGENT' : 'CURRENT';
  const eligible = !stale && !fault && row.mode === 'ADAPTIVE' &&
    ['STABLE', 'ACTIVE'].includes(row.state) && row.control_version === 1 && expected > 60;
  const reason = stale ? 'No update within the declared policy interval plus grace' :
    row.state === 'DEAD' ? 'Node reports simulated battery shutdown' :
    fault ? 'Fill measurement unavailable or sensor fault' :
    urgent ? 'Node alert or fill at least 80%' :
    row.state === 'ACTIVE' ? 'Active filling needs closer observation' :
    row.state === 'CONSERVE' ? 'Local battery conservation is protected' : 'Routine monitoring';
  const powerComplete = !!row.ina_present &&
    [row.bus_voltage_v,row.current_ma,row.power_mw,row.energy_mwh,row.power_sample_ms].every(finite);
  const powerPathValid = powerComplete && row.bus_voltage_v >= 4.5 && row.bus_voltage_v <= 5.5 &&
    row.current_ma > 1 && row.power_mw > 1;
  const sessionQualified = typeof row.power_session === 'string' && row.power_session.length > 0 &&
    row.power_mode === 'live-connected-10hz';
  const powerStatus = !row.ina_present ? 'UNINSTRUMENTED' :
    !powerPathValid ? 'CHECK_POWER_PATH' : !sessionQualified ? 'SESSION_UNQUALIFIED' : 'MEASURED';
  const powerReason = !row.ina_present ? 'No INA219 measurement in the latest packet' :
    !powerComplete ? 'INA219 packet is incomplete' :
    !powerPathValid ? 'Current or power is near zero, or supply is outside 4.5–5.5 V; check that node power passes through VIN+ and VIN−' :
    !sessionQualified ? 'Electrical path is measured, but this firmware does not identify a reboot-safe power session' :
    'Supply passes through the INA219 shunt and this boot has a measurement session';
  return {...row, age_s: age, stale_after_s: staleAfter, status, priority_reason: reason,
    priority: stale || fault || row.state === 'DEAD' ? 1 : urgent ? 0 : row.state === 'ACTIVE' ? 2 : 3,
    feedback_eligible: eligible, power_path_valid: powerPathValid,
    power_trial_ready: powerPathValid && sessionQualified,
    power_status: powerStatus, power_reason: powerReason};
}

export function gatewaySnapshot(rows, now = Date.now()) {
  const nodes = rows.map(row => describeNode(row, now)).sort((a, b) =>
    a.priority - b.priority || (b.fill_pct ?? -1) - (a.fill_pct ?? -1) || a.node_id.localeCompare(b.node_id));
  const selected = nodes.find(node => node.feedback_eligible);
  return {at: now, nodes, selected_node: selected?.node_id ?? null,
    decision: selected ? `${selected.node_id}: ${selected.priority_reason}; eligible for a bounded 60 s request` :
      'No fresh, capable, nonurgent adaptive node is eligible for closer monitoring',
    processing: 'Local per-bin aggregation, freshness checks and deterministic priority selection',
    limits: 'No validated prediction or shared-uplink budget arbitration; feedback dispatch is explicit, not automatic'};
}

export function normalizeReading(topicNode, value, now, runLabel) {
  if (!validNodeId(topicNode) || !value || typeof value !== 'object' || Array.isArray(value) ||
      value.node_id !== topicNode || !['BASELINE', 'ADAPTIVE'].includes(value.mode) ||
      !['STABLE', 'ACTIVE', 'ALERT', 'CONSERVE', 'DEAD'].includes(value.state)) throw Error('Invalid node/topic, mode or state');
  const row = {node_id: topicNode, state: value.state, mode: value.mode,
    reason: typeof value.reason === 'string' ? value.reason.slice(0,64) : null,
    tx_trigger: typeof value.tx_trigger === 'string' ? value.tx_trigger.slice(0,64) :
      (typeof value.reason === 'string' ? value.reason.slice(0,64) : null),
    state_cause: typeof value.state_cause === 'string' ? value.state_cause.slice(0,64) : null,
    sensor_profile: typeof value.sensor_profile === 'string' ? value.sensor_profile.slice(0,32) : 'legacy-unspecified',
    power_session: typeof value.power_session === 'string' ? value.power_session.slice(0,32) : null,
    power_profile: typeof value.power_profile === 'string' ? value.power_profile.slice(0,32) : null,
    power_mode: typeof value.power_mode === 'string' ? value.power_mode.slice(0,32) : null,
    control_id: typeof value.control_id === 'string' ? value.control_id.slice(0,32) : null,
    control_source: typeof value.control_source === 'string' ? value.control_source.slice(0,24) : 'local',
    sensor_fault: value.sensor_fault ? 1 : 0, ina_present: value.ina_present ? 1 : 0,
    rx_ts: now, run_label: runLabel};
  for (const field of ['ts','fill_pct','temp_c','humidity','gas_135','gas_5','batt_pct','tx_count',
    'interval','deadband','dfdt','tx_dropped','policy_interval','next_interval','control_ttl','control_version',
    'bus_voltage_v','current_ma','power_mw','energy_mwh','power_sample_ms']) {
    row[field] = finite(value[field]) ? value[field] : null;
  }
  for (const field of ['fill_pct','humidity','batt_pct']) {
    if (row[field] !== null && (row[field] < 0 || row[field] > 100)) throw Error(`Invalid ${field}`);
  }
  if (row.ts === null || row.ts < 0) throw Error('Missing device uptime');
  if (row.state !== 'DEAD' && (!finite(row.interval) || row.interval <= 0 || row.interval > 1800)) throw Error('Invalid reporting interval');
  if (row.bus_voltage_v !== null && (row.bus_voltage_v < 0 || row.bus_voltage_v > 26)) throw Error('Invalid INA219 bus voltage');
  if (row.current_ma !== null && Math.abs(row.current_ma) > 3200) throw Error('Invalid INA219 current');
  if (row.power_mw !== null && (row.power_mw < 0 || row.power_mw > 100000)) throw Error('Invalid INA219 power');
  if (row.energy_mwh !== null && (row.energy_mwh < 0 || row.energy_mwh > 10000000)) throw Error('Invalid INA219 energy');
  if (row.power_sample_ms !== null && row.power_sample_ms < 0) throw Error('Invalid INA219 sample uptime');
  if (row.ina_present && [row.bus_voltage_v,row.current_ma,row.power_mw,row.energy_mwh,row.power_sample_ms].some(v=>!finite(v))) {
    throw Error('INA219 marked present without a complete measurement');
  }
  return row;
}

export function validLease(body) {
  return Number.isInteger(body?.interval_s) && body.interval_s >= 60 && body.interval_s <= 300 &&
    Number.isInteger(body?.ttl_s) && body.ttl_s >= 60 && body.ttl_s <= 600 && body.ttl_s >= body.interval_s;
}
