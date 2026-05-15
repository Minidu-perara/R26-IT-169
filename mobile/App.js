import { useEffect, useState } from 'react';
import { View, Text, StyleSheet, ScrollView } from 'react-native';

const BACKEND = 'http://192.168.1.100:3001'; // update to your LAN IP

export default function App() {
  const [readings, setReadings] = useState([]);

  useEffect(() => {
    const poll = () =>
      fetch(`${BACKEND}/api/readings?limit=10`)
        .then(r => r.json())
        .then(rows => setReadings(rows.reverse()))
        .catch(() => {});
    poll();
    const id = setInterval(poll, 10000);
    return () => clearInterval(id);
  }, []);

  const latest = readings[0];

  return (
    <ScrollView style={styles.container}>
      <Text style={styles.title}>Smart Bin R26-IT-169</Text>
      {latest ? (
        <View style={styles.card}>
          <Text style={styles.state}>{latest.state}</Text>
          <Text style={styles.stat}>Fill:    {latest.fill_pct} %</Text>
          <Text style={styles.stat}>Battery: {parseFloat(latest.batt_pct).toFixed(0)} %</Text>
          <Text style={styles.stat}>Temp:    {latest.temp_c} °C</Text>
          <Text style={styles.stat}>Gas:     {latest.gas_raw} ADC</Text>
          <Text style={styles.stat}>Mode:    {latest.mode}</Text>
          <Text style={styles.stat}>TX Adaptive: {latest.tx_adaptive}</Text>
          <Text style={styles.stat}>TX Baseline: {latest.tx_baseline}</Text>
        </View>
      ) : (
        <Text style={styles.stat}>Waiting for data…</Text>
      )}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: '#111827', padding: 20 },
  title:     { color: '#fff', fontSize: 20, fontWeight: 'bold', marginBottom: 16 },
  card:      { backgroundColor: '#1f2937', borderRadius: 12, padding: 16, marginBottom: 12 },
  state:     { color: '#60a5fa', fontSize: 18, fontWeight: 'bold', marginBottom: 8 },
  stat:      { color: '#d1d5db', fontSize: 15, marginBottom: 4 },
});
