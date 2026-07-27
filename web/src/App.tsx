import { BrowserRouter, Routes, Route } from 'react-router-dom';
import { lazy, Suspense } from 'react';
import { AppLayout } from './components/layout/AppLayout';

const Landing = lazy(() => import('./pages/Landing').then((m) => ({ default: m.Landing })));
const Overview = lazy(() => import('./pages/Overview').then((m) => ({ default: m.Overview })));
const FeatureDetail = lazy(() => import('./pages/FeatureDetail').then((m) => ({ default: m.FeatureDetail })));
const FlameGraph = lazy(() => import('./pages/FlameGraph').then((m) => ({ default: m.FlameGraph })));
const PluginManager = lazy(() => import('./pages/PluginManager').then((m) => ({ default: m.PluginManager })));

function PageLoader() {
  return (
    <div className="flex items-center justify-center h-full bg-bg-deep">
      <div className="w-6 h-6 border-2 border-accent/30 border-t-accent rounded-full animate-spin" />
    </div>
  );
}

export function App() {
  return (
    <BrowserRouter>
      <Suspense fallback={<PageLoader />}>
        <Routes>
          <Route index element={<Landing />} />
          <Route element={<AppLayout />}>
            <Route path="dashboard" element={<Overview />} />
            <Route path="feature/:name" element={<FeatureDetail />} />
            <Route path="flamegraph" element={<FlameGraph />} />
            <Route path="plugins" element={<PluginManager />} />
          </Route>
        </Routes>
      </Suspense>
    </BrowserRouter>
  );
}
