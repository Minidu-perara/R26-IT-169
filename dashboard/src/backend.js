// Shared by the operator dashboard (/) and the mobile view (/m).
// Served by the backend (http://192.168.1.3:3002): same origin. On the Vite dev
// server (port 5173) the backend is on the same host at :3002.
export const BACKEND = import.meta.env.VITE_BACKEND_URL ||
  (window.location.port === '5173' ? `${window.location.protocol}//${window.location.hostname}:3002` : window.location.origin)

// Throws on a non-2xx reply. The error carries the HTTP status and the parsed
// body so callers can tell a refusal (409, 429) from a timeout (504). Only an
// error reply may lack a JSON body; a malformed 2xx reply still throws.
export async function api(path,body) {
  const response = await fetch(BACKEND+path,body===undefined ? {} : {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
  const value = await response.json().catch(error=>{if(response.ok) throw error; return {}})
  if(!response.ok) {
    const error = Error(value.error || `HTTP ${response.status}`)
    error.status = response.status
    error.body = value
    throw error
  }
  return value
}

export const REPORT_TYPES = [
  {value:'overflow',label:'Overflowing'},
  {value:'damage',label:'Damaged'},
  {value:'smell',label:'Bad smell'},
]
export const reportLabel = type => REPORT_TYPES.find(t=>t.value===type)?.label || type
