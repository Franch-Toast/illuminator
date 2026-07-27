import { useEffect } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import {
  LayoutDashboard,
  Cpu,
  MemoryStick,
  HardDrive,
  Network,
  Clock,
  Flame,
  Puzzle,
  PanelLeftClose,
  PanelLeftOpen,
  Activity,
} from 'lucide-react';
import clsx from 'clsx';
import { useUIStore } from '../../stores/ui-store';

interface NavItem {
  icon: React.ReactNode;
  label: string;
  path: string;
}

const mainNav: NavItem[] = [
  { icon: <LayoutDashboard size={18} />, label: 'Overview', path: '/dashboard' },
];

const featureNav: NavItem[] = [
  { icon: <Cpu size={18} />, label: 'CPU', path: '/cpu' },
  { icon: <MemoryStick size={18} />, label: 'Memory', path: '/feature/memory_usage' },
  { icon: <HardDrive size={18} />, label: 'Disk I/O', path: '/feature/disk_io' },
  { icon: <Network size={18} />, label: 'Network', path: '/feature/network_io' },
  { icon: <Clock size={18} />, label: 'Scheduler', path: '/feature/scheduler_latency' },
];

const toolNav: NavItem[] = [
  { icon: <Flame size={18} />, label: 'Flame Graph', path: '/flamegraph' },
  { icon: <Puzzle size={18} />, label: 'Plugins', path: '/plugins' },
];

export function Sidebar() {
  const collapsed = useUIStore((s) => s.sidebarCollapsed);
  const toggle = useUIStore((s) => s.toggleSidebar);
  const setCollapsed = useUIStore((s) => s.setSidebarCollapsed);
  const location = useLocation();
  const navigate = useNavigate();

  useEffect(() => {
    const mq = window.matchMedia('(max-width: 900px)');
    const handler = (e: MediaQueryListEvent) => setCollapsed(e.matches);
    mq.addEventListener('change', handler);
    if (mq.matches) setCollapsed(true);
    return () => mq.removeEventListener('change', handler);
  }, [setCollapsed]);

  const isActive = (path: string) => {
    if (path === '/dashboard') return location.pathname === '/dashboard';
    return location.pathname.startsWith(path);
  };

  return (
    <aside
      className={clsx(
        'h-full flex flex-col border-r border-border bg-surface transition-[width] duration-200 flex-shrink-0',
        collapsed ? 'w-16' : 'w-[260px]'
      )}
    >
      {/* Logo */}
      <div className="h-14 flex items-center gap-2.5 px-4 border-b border-border flex-shrink-0">
        <Activity size={22} className="text-accent flex-shrink-0 animate-slide-in-left" />
        {!collapsed && (
          <span className="text-sm font-bold tracking-tight text-text-primary whitespace-nowrap">
            Illuminator
          </span>
        )}
      </div>

      {/* Navigation */}
      <nav className="flex-1 overflow-y-auto py-4 space-y-1">
        <NavGroup label="Dashboard" collapsed={collapsed}>
          {mainNav.map((item) => (
            <NavLink key={item.path} item={item} active={isActive(item.path)} collapsed={collapsed} onClick={() => navigate(item.path)} />
          ))}
        </NavGroup>

        <NavGroup label="Features" collapsed={collapsed}>
          {featureNav.map((item) => (
            <NavLink key={item.path} item={item} active={isActive(item.path)} collapsed={collapsed} onClick={() => navigate(item.path)} />
          ))}
        </NavGroup>

        <NavGroup label="Tools" collapsed={collapsed}>
          {toolNav.map((item) => (
            <NavLink key={item.path} item={item} active={isActive(item.path)} collapsed={collapsed} onClick={() => navigate(item.path)} />
          ))}
        </NavGroup>
      </nav>

      {/* Collapse toggle */}
      <button
        className="h-11 flex items-center gap-2 px-4 border-t border-border text-text-muted hover:text-text-secondary transition-colors cursor-pointer"
        onClick={toggle}
      >
        {collapsed ? <PanelLeftOpen size={18} /> : <PanelLeftClose size={18} />}
        {!collapsed && <span className="text-xs">Collapse</span>}
      </button>
    </aside>
  );
}

function NavGroup({
  label,
  collapsed,
  children,
}: {
  label: string;
  collapsed: boolean;
  children: React.ReactNode;
}) {
  return (
    <div className="mb-3">
      {collapsed ? (
        <div className="mx-3 my-3 border-t border-border" />
      ) : (
        <div className="px-5 mb-2 pt-1 text-[11px] font-semibold uppercase tracking-widest text-text-muted">
          {label}
        </div>
      )}
      {children}
    </div>
  );
}

function NavLink({
  item,
  active,
  collapsed,
  onClick,
}: {
  item: NavItem;
  active: boolean;
  collapsed: boolean;
  onClick: () => void;
}) {
  return (
    <button
      onClick={onClick}
      className={clsx(
        'w-full flex items-center gap-3 h-12 transition-colors cursor-pointer relative',
        collapsed ? 'justify-center mx-auto px-0' : 'px-5',
        active
          ? 'text-accent bg-accent/8'
          : 'text-text-secondary hover:text-text-primary hover:bg-surface-2'
      )}
    >
      {active && !collapsed && (
        <span className="absolute left-0 top-1/2 -translate-y-1/2 w-[3px] h-6 bg-accent rounded-r" />
      )}
      <span className="flex-shrink-0">{item.icon}</span>
      {!collapsed && (
        <span className="text-sm truncate">{item.label}</span>
      )}
    </button>
  );
}
