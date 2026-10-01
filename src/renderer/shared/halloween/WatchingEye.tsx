import { motion, useMotionValue, useReducedMotion, useSpring } from 'motion/react';
import { useEffect, useRef } from 'react';
import { useHalloween } from './useHalloween';

const reach = 2.6;

/** A bloodshot eye that stands in for the dot of the "i" and watches the
 * cursor. The pupil is driven by spring motion values, so following the
 * pointer never re-renders React. */
function Eye() {
  const ref = useRef<SVGSVGElement>(null);
  const reduce = useReducedMotion();
  const targetX = useMotionValue(0);
  const targetY = useMotionValue(0);
  const x = useSpring(targetX, { stiffness: 260, damping: 22 });
  const y = useSpring(targetY, { stiffness: 260, damping: 22 });

  useEffect(() => {
    if (reduce) return;
    const follow = (event: PointerEvent) => {
      const box = ref.current?.getBoundingClientRect();
      if (!box) return;
      const dx = event.clientX - (box.left + box.width / 2);
      const dy = event.clientY - (box.top + box.height / 2);
      const distance = Math.hypot(dx, dy) || 1;
      const pull = Math.min(1, distance / 120);
      targetX.set((dx / distance) * reach * pull);
      targetY.set((dy / distance) * reach * 0.8 * pull);
    };
    window.addEventListener('pointermove', follow, { passive: true });
    return () => window.removeEventListener('pointermove', follow);
  }, [reduce]);

  return (
    <svg ref={ref} className='hw-eye' viewBox='-10 -8 20 16' aria-hidden='true' focusable='false'>
      <defs>
        <radialGradient id='hw-eye-sclera' r='0.62'>
          <stop offset='0.45' stopColor='#f6eee6' />
          <stop offset='0.85' stopColor='#f0c9c0' />
          <stop offset='1' stopColor='#d9807a' />
        </radialGradient>
        <clipPath id='hw-eye-lids'>
          <path d='M-9 0 Q0 -8.4 9 0 Q0 8.4 -9 0Z' />
        </clipPath>
      </defs>
      <g className='hw-eye-ball'>
        <path d='M-9 0 Q0 -8.4 9 0 Q0 8.4 -9 0Z' fill='url(#hw-eye-sclera)' />
        <g clipPath='url(#hw-eye-lids)' fill='none' stroke='#c2413d' strokeLinecap='round'>
          <path d='M-9 -1 q2.4 0.4 3.6 1.8 q0.8 0.9 2 0.7' strokeWidth={0.6} />
          <path d='M-8 2.6 q2.6 -0.6 4 -2.2' strokeWidth={0.5} />
          <path d='M9 -0.6 q-2.2 0.8 -3.4 2.4 q-0.6 0.8 -1.8 0.9' strokeWidth={0.6} />
          <path d='M8.2 2.2 q-2 -0.2 -3.2 -1.6 M6.4 -3.2 q-1.4 1.2 -1.6 2.6' strokeWidth={0.45} />
          <path d='M-6.6 -3.4 q1 1.4 0.6 2.8' strokeWidth={0.45} />
          <motion.g style={{ x, y }}>
            <circle r={2.9} fill='#b8762f' />
            <circle r={2.9} fill='none' stroke='#6b3c14' strokeWidth={0.5} />
            <circle r={1.35} fill='#120b10' stroke='none' />
            <circle cx={1} cy={-1.1} r={0.6} fill='#fff' stroke='none' opacity={0.85} />
          </motion.g>
        </g>
        <path d='M-9.2 0.1 Q0 -9.2 9.2 0.1' fill='none' stroke='#2a1a24' strokeWidth={1.1} strokeLinecap='round' />
        <path d='M-9 0 Q0 8.4 9 0' fill='none' stroke='#8f3a3a' strokeWidth={0.5} opacity={0.7} />
      </g>
    </svg>
  );
}

export function WatchingEye() {
  return useHalloween() ? <Eye /> : null;
}
