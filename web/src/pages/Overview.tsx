import { useNavigate } from 'react-router-dom';
import { goldenSignals } from '../mock/data';
import { useFeatureStore } from '../stores/feature-store';
import { StatCard } from '../components/cards/StatCard';
import { FeatureCard } from '../components/cards/FeatureCard';
import { ShinyText } from '../components/effects/ShinyText';

export function Overview() {
  const features = useFeatureStore((s) => s.features);
  const navigate = useNavigate();
  const activeCount = features.filter((f) => f.status === 'active').length;

  return (
    <div className="page-container space-y-10">
      {/* Header */}
      <div className="animate-fade-in-up">
        <h1 className="text-2xl sm:text-3xl font-bold tracking-tight">
          <ShinyText>System Overview</ShinyText>
        </h1>
        <p className="text-sm sm:text-base text-text-secondary mt-2">
          Golden Signals at a glance &mdash; real-time system health
        </p>
      </div>

      {/* Golden Signals Grid */}
      <section>
        <div className="grid grid-cols-1 sm:grid-cols-2 xl:grid-cols-4 gap-5">
          {goldenSignals.map((signal, i) => (
            <StatCard key={signal.label} signal={signal} index={i} />
          ))}
        </div>
      </section>

      {/* Feature Status */}
      <section className="space-y-5">
        <div className="flex items-center justify-between animate-fade-in-up" style={{ animationDelay: '300ms' }}>
          <div>
            <h2 className="text-xl font-semibold">Feature Status</h2>
            <p className="text-sm text-text-muted mt-1">
              Auto-discovered &middot; {activeCount} active &middot; {features.length} total
            </p>
          </div>
        </div>

        <div className="grid grid-cols-1 md:grid-cols-2 2xl:grid-cols-3 gap-5">
          {features.map((feature, i) => (
            <FeatureCard
              key={feature.name}
              feature={feature}
              index={i + goldenSignals.length}
              onNavigate={(name) => navigate(`/feature/${name}`)}
            />
          ))}
        </div>
      </section>
    </div>
  );
}
