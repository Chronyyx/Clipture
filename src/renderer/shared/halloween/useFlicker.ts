import { useAnimate, useReducedMotion } from 'motion/react';
import { useEffect } from 'react';

const period = 2.4;

/** Candlelight: every few seconds Motion plays a new, irregular opacity
 * sequence on the scoped element, so the flicker never visibly loops. One
 * short WAAPI animation per period; nothing runs while the window is hidden. */
export function useFlicker<T extends Element>(active = true) {
  const [scope, animate] = useAnimate<T>();
  const reduce = useReducedMotion();
  useEffect(() => {
    if (!active || reduce) return;
    let timer = 0;
    const run = () => {
      if (scope.current && !document.hidden) {
        const steps = Array.from({ length: 5 }, () => 0.7 + Math.random() * 0.3);
        animate(scope.current, { opacity: [1, ...steps, 1] }, { duration: period, ease: 'linear' });
      }
      timer = window.setTimeout(run, period * 1000);
    };
    run();
    return () => window.clearTimeout(timer);
  }, [active, reduce]);
  return scope;
}
