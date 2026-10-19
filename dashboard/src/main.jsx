import React,{Suspense,lazy} from 'react'
import ReactDOM from 'react-dom/client'
import './index.css'

// /m and /m/... open the phone view. Each view is its own chunk, so a phone
// does not download the operator charts.
const path = window.location.pathname
const isMobile = path === '/m' || path.startsWith('/m/')
const View = lazy(() => isMobile ? import('./Mobile') : import('./App'))

ReactDOM.createRoot(document.getElementById('root')).render(<Suspense fallback={null}><View /></Suspense>)
