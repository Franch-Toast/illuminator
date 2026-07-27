import { type ReactNode } from 'react';
import clsx from 'clsx';

interface ShinyTextProps {
  children: ReactNode;
  className?: string;
  shimmer?: boolean;
}

export function ShinyText({ children, className, shimmer = true }: ShinyTextProps) {
  return (
    <span
      className={clsx(className, shimmer && 'bg-clip-text')}
      style={
        shimmer
          ? {
              backgroundImage:
                'linear-gradient(90deg, var(--color-text-primary) 35%, var(--color-accent-light) 50%, var(--color-text-primary) 65%)',
              backgroundSize: '200% auto',
              WebkitBackgroundClip: 'text',
              WebkitTextFillColor: 'transparent',
              animation: 'shimmer 3s linear infinite',
            }
          : undefined
      }
    >
      {children}
    </span>
  );
}
