import { useNavigate } from 'react-router-dom';
import { Activity, ArrowRight, ExternalLink, Zap, Shield, Radio } from 'lucide-react';

const highlights = [
  { icon: <Zap size={18} />, label: 'Zero-intrusion eBPF' },
  { icon: <Shield size={18} />, label: '<1% overhead' },
  { icon: <Radio size={18} />, label: 'Real-time SSE' },
];

const metrics = [
  { label: 'CPU Usage', value: '78%', sub: '16 cores', color: '#6366f1' },
  { label: 'Memory', value: '4.2 GiB', sub: '/ 32 GiB', color: '#22c55e' },
  { label: 'Disk I/O', value: '120 MB/s', sub: 'nvme0n1', color: '#f59e0b' },
  { label: 'P99 Latency', value: '2.3 ms', sub: 'last 60s', color: '#ef4444' },
];


export function Landing() {
  const navigate = useNavigate();

  return (
    <div className="min-h-screen bg-bg-deep text-text-primary flex flex-col relative overflow-hidden">
      {/* Animated background */}
      <div className="absolute inset-0 pointer-events-none" aria-hidden>
        {/* Gentle blurred color washes — soft and barely visible */}
        <div className="absolute w-full h-full">
          <div className="absolute top-[5%] right-[10%] w-[50vw] h-[50vh] rounded-full animate-float"
            style={{ background: 'radial-gradient(circle, rgba(129,140,248,0.18) 0%, transparent 65%)', filter: 'blur(80px)' }} />
          <div className="absolute bottom-[15%] left-[5%] w-[45vw] h-[45vh] rounded-full animate-float"
            style={{ background: 'radial-gradient(circle, rgba(99,102,241,0.14) 0%, transparent 65%)', filter: 'blur(90px)', animationDelay: '6s' }} />
          <div className="absolute top-[45%] left-[35%] w-[35vw] h-[35vh] rounded-full animate-float"
            style={{ background: 'radial-gradient(circle, rgba(167,139,250,0.10) 0%, transparent 65%)', filter: 'blur(80px)', animationDelay: '12s' }} />
        </div>
        {/* Subtle grid pattern */}
        <div className="absolute inset-0 opacity-[0.03]" style={{
          backgroundImage: 'linear-gradient(rgba(99,102,241,0.4) 1px, transparent 1px), linear-gradient(90deg, rgba(99,102,241,0.4) 1px, transparent 1px)',
          backgroundSize: '72px 72px',
        }} />
      </div>

      {/* Nav */}
      <nav className="h-16 flex items-center justify-between px-8 border-b border-border/50 bg-bg-deep/60 backdrop-blur-xl flex-shrink-0 relative z-20">
        <div className="flex items-center gap-3">
          <Activity size={24} className="text-accent" />
          <span className="text-base font-bold tracking-tight">Illuminator</span>
        </div>
        <div className="flex items-center gap-4">
          <a
            href="https://github.com"
            target="_blank"
            rel="noopener noreferrer"
            className="flex items-center gap-2 px-4 py-2 text-sm text-text-secondary hover:text-text-primary transition-colors"
          >
            <ExternalLink size={15} />
            Source
          </a>
          <button
            onClick={() => navigate('/dashboard')}
            className="flex items-center gap-2 px-5 py-2.5 border border-accent/40 hover:border-accent/70 text-accent-light hover:text-white text-sm font-semibold rounded-radius-sm transition-all hover:bg-accent/10 cursor-pointer"
          >
            Launch Dashboard
            <ArrowRight size={15} />
          </button>
        </div>
      </nav>

      {/* Hero */}
      <main className="flex-1 flex flex-col items-center justify-center px-8 relative z-10">
        <div className="text-center max-w-3xl mx-auto space-y-12">
          {/* Badge */}
          <div className="animate-fade-in-up">
            <span className="inline-flex items-center gap-2.5 px-5 py-2 rounded-full border border-accent/20 bg-accent/5 text-sm text-accent-light">
              <span
                className="w-2 h-2 rounded-full bg-status-active"
                style={{ animation: 'pulse-glow 2s ease-in-out infinite', color: '#22c55e' }}
              />
              Built on eBPF
            </span>
          </div>

          {/* Title */}
          <h1 className="text-5xl sm:text-6xl lg:text-7xl font-bold leading-[1.1] tracking-tight animate-fade-in-up" style={{ animationDelay: '80ms' }}>
            Deep Performance
            <br />
            <span className="bg-gradient-to-r from-accent-light via-accent to-indigo-400 bg-clip-text text-transparent">
              Analysis Platform
            </span>
          </h1>

          {/* Description */}
          <p className="text-lg sm:text-xl text-text-secondary max-w-xl mx-auto leading-relaxed animate-fade-in-up" style={{ animationDelay: '160ms' }}>
            Real-time system observability with plugin-based architecture.
            <br className="hidden sm:block" />
            Record, replay, and analyze at every layer.
          </p>

          {/* CTA */}
          <div className="flex flex-col items-center gap-5 animate-fade-in-up" style={{ animationDelay: '240ms' }}>
            <button
              onClick={() => navigate('/dashboard')}
              className="group/cta flex items-center gap-2.5 px-10 py-4.5 border border-accent/50 hover:border-accent text-accent-light hover:text-white text-lg font-semibold rounded-radius-md transition-all hover:bg-accent/10 hover:shadow-[0_0_48px_rgba(99,102,241,0.15)] cursor-pointer backdrop-blur-sm"
            >
              Open Dashboard
              <ArrowRight size={20} className="transition-transform group-hover/cta:translate-x-1" />
            </button>
            <code className="px-5 py-3 bg-surface/80 border border-border rounded-radius-sm text-sm font-mono text-text-muted backdrop-blur-sm">
              $ ./illuminator --serve --port 9090
            </code>
          </div>

          {/* Highlights */}
          <div className="flex flex-wrap items-center justify-center gap-8 sm:gap-12 animate-fade-in-up" style={{ animationDelay: '320ms' }}>
            {highlights.map((h) => (
              <div key={h.label} className="flex items-center gap-2.5 text-sm text-text-secondary">
                <span className="text-accent">{h.icon}</span>
                {h.label}
              </div>
            ))}
          </div>
        </div>

        {/* Live metrics bar */}
        <div className="relative z-10 mt-auto mb-10 w-full max-w-4xl mx-auto animate-fade-in-up" style={{ animationDelay: '400ms' }}>
          <div className="grid grid-cols-2 lg:grid-cols-4 gap-4">
            {metrics.map((m) => (
              <div
                key={m.label}
                className="flex items-center gap-4 px-6 py-5 rounded-radius-md bg-surface/50 border border-border/50 backdrop-blur-md hover:border-accent/20 transition-colors"
              >
                <div className="w-1.5 h-10 rounded-full opacity-70" style={{ backgroundColor: m.color }} />
                <div>
                  <div className="text-[11px] font-medium uppercase tracking-widest text-text-muted mb-1">{m.label}</div>
                  <div className="text-xl font-bold font-mono leading-tight">{m.value}</div>
                  <div className="text-[11px] text-text-muted mt-0.5">{m.sub}</div>
                </div>
              </div>
            ))}
          </div>
        </div>
      </main>

      {/* Footer */}
      <footer className="py-5 px-8 border-t border-border/50 text-center text-xs text-text-muted flex-shrink-0 relative z-10">
        Illuminator &middot; Deep Performance Analysis Platform
      </footer>
    </div>
  );
}
