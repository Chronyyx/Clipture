import { AnimatePresence, motion, useAnimate, useReducedMotion } from 'motion/react';
import { useEffect, useRef, useState, type CSSProperties } from 'react';
import { SpookyMascot } from './characters';
import { SpookyBubble } from './scenes';
import { useHalloween } from './useHalloween';

// The night is made of one-off Motion animations started at random intervals:
// no endless loops to keep alive, and nothing starts while the window is hidden.
function useEvery(run: () => void, [min, max]: [number, number], first: number, enabled: boolean) {
  const latest = useRef(run);
  latest.current = run;
  useEffect(() => {
    if (!enabled) return;
    let timer = 0;
    const tick = (delay: number) => {
      timer = window.setTimeout(() => {
        if (!document.hidden) latest.current();
        tick(min + Math.random() * (max - min));
      }, delay);
    };
    tick(first);
    return () => window.clearTimeout(timer);
  }, [enabled, min, max, first]);
}

const random = (min: number, max: number) => min + Math.random() * (max - min);

/** Twinkle one child of the scope at a time: a brief swell of light. */
function useTwinkle(enabled: boolean, interval: [number, number], rest: number) {
  const [scope, animate] = useAnimate<HTMLDivElement>();
  useEvery(() => {
    const children = scope.current?.children;
    if (!children?.length) return;
    const star = children[Math.floor(Math.random() * children.length)] as HTMLElement;
    animate(star, { opacity: [rest, 1, rest], transform: ['scale(0.8)', 'scale(1.5)', 'scale(0.8)'] }, { duration: random(1.4, 2.4), ease: 'easeInOut' });
  }, interval, 600, enabled);
  return scope;
}

const stars: Array<[left: number, top: number, size: number]> = [
  [6, 8, 2], [14, 30, 1.5], [22, 12, 2.5], [31, 42, 1.5], [38, 6, 2], [46, 24, 1.5], [53, 50, 2], [61, 14, 1.5],
  [67, 36, 2.5], [74, 8, 1.5], [81, 28, 2], [88, 46, 1.5], [93, 16, 2], [28, 58, 1.5], [57, 66, 1.5], [85, 62, 2]
];

function Stars({ enabled }: { enabled: boolean }) {
  const scope = useTwinkle(enabled, [450, 1100], 0.3);
  return (
    <div ref={scope} className='hw-stars'>
      {stars.map(([left, top, size], index) => (
        <i key={index} style={{ left: left + '%', top: top + '%', width: size, height: size }} />
      ))}
    </div>
  );
}

// A string of warm lights in gentle swags across the top of the workspace.
const swags = 6;
const bulbs = Array.from({ length: swags * 3 }, (_, index) => {
  const t = ((index % 3) + 1) / 4;
  return { left: ((Math.floor(index / 3) + t) / swags) * 100, top: 2 + 28 * t * (1 - t), cool: index % 5 === 2 };
});
const wire = 'M0 2' + Array.from({ length: swags }, (_, index) => ` Q${index * 100 + 50} 16 ${index * 100 + 100} 2`).join('');

function FairyLights({ enabled }: { enabled: boolean }) {
  const scope = useTwinkle(enabled, [300, 800], 0.55);
  return (
    <div className='hw-lights'>
      <svg viewBox={`0 0 ${swags * 100} 20`} preserveAspectRatio='none'>
        <path d={wire} fill='none' stroke='#5d5178' strokeWidth={1} vectorEffect='non-scaling-stroke' />
      </svg>
      <div ref={scope}>
        {bulbs.map(({ left, top, cool }, index) => (
          <i key={index} className={cool ? 'cool' : undefined} style={{ left: left + '%', top }} />
        ))}
      </div>
    </div>
  );
}

type Mote = { id: number; left: number; size: number; duration: number; drift: number; peak: number; ember: boolean };

/** Embers and dust rising slowly from the bottom of the room. */
function Motes({ enabled }: { enabled: boolean }) {
  const [motes, setMotes] = useState<Mote[]>([]);
  const id = useRef(0);
  useEvery(() => setMotes((current) => current.length >= 7 ? current : [...current, {
    id: ++id.current, left: random(2, 98), size: random(1.5, 3.5), duration: random(13, 20),
    drift: random(-40, 40), peak: random(0.35, 0.8), ember: Math.random() < 0.6
  }]), [1600, 3400], 300, enabled);
  return (
    <>
      {motes.map((mote) => (
        <motion.i key={mote.id} className={mote.ember ? 'hw-mote ember' : 'hw-mote'}
          style={{ left: mote.left + '%', width: mote.size, height: mote.size }}
          initial={{ opacity: 0, transform: 'translate(0px, 0px)' }}
          animate={{
            opacity: [0, mote.peak, mote.peak * 0.7, 0],
            transform: ['translate(0px, 0px)', `translate(${mote.drift * 0.4}px, -30vh)`, `translate(${mote.drift}px, -62vh)`]
          }}
          transition={{ duration: mote.duration, ease: 'linear' }}
          onAnimationComplete={() => setMotes((current) => current.filter((other) => other.id !== mote.id))} />
      ))}
    </>
  );
}

/** Two banks of fog, each sliding a few percent and back over half a minute. */
function FogBank({ className, duration, enabled }: { className: string; duration: number; enabled: boolean }) {
  const [out, setOut] = useState(false);
  return (
    <motion.div className={'hw-fog ' + className}
      animate={enabled ? { transform: out ? 'translateX(4%)' : 'translateX(-4%)' } : undefined}
      transition={{ duration, ease: 'easeInOut' }}
      onAnimationComplete={() => setOut((value) => !value)} />
  );
}

type Leaf = { id: number; left: number; duration: number; sway: number; color: string };
const leafColors = ['#8d4a2b', '#a35a2a', '#6e3a2a'];

function Leaves({ enabled }: { enabled: boolean }) {
  const [leaves, setLeaves] = useState<Leaf[]>([]);
  const id = useRef(0);
  useEvery(() => setLeaves((current) => current.length >= 2 ? current : [...current, {
    id: ++id.current, left: random(5, 95), duration: random(17, 24), sway: random(20, 40),
    color: leafColors[Math.floor(Math.random() * leafColors.length)]
  }]), [9000, 17000], 4000, enabled);
  return (
    <>
      {leaves.map((leaf) => (
        <motion.span key={leaf.id} className='hw-leaf' style={{ left: leaf.left + '%' }}
          initial={{ transform: 'translate(0px, -20px) rotate(0deg)' }}
          animate={{
            transform: [
              'translate(0px, -20px) rotate(0deg)', `translate(${leaf.sway}px, 34vh) rotate(120deg)`,
              `translate(${-leaf.sway * 0.6}px, 70vh) rotate(230deg)`, `translate(${leaf.sway * 0.5}px, 106vh) rotate(340deg)`
            ]
          }}
          transition={{ duration: leaf.duration, ease: 'linear' }}
          onAnimationComplete={() => setLeaves((current) => current.filter((other) => other.id !== leaf.id))}>
          <svg viewBox='-10 -10 20 20'>
            <path d='M0 -9 L2 -4 L7 -6 L5 -1 L9 1 L4 3 L5 8 L0 5 L-5 8 L-4 3 L-9 1 L-5 -1 L-7 -6 L-2 -4Z' fill={leaf.color} />
          </svg>
        </motion.span>
      ))}
    </>
  );
}

/** Rarely, a faint ghost drifts past behind the panels. */
function PassingGhost({ enabled }: { enabled: boolean }) {
  const [pass, setPass] = useState<{ id: number; top: number }>();
  useEvery(() => setPass((current) => current ?? { id: Date.now(), top: random(52, 74) }), [80000, 140000], 30000, enabled);
  if (!pass) return null;
  return (
    <motion.span key={pass.id} className='hw-passing-ghost' style={{ top: pass.top + '%' }}
      initial={{ opacity: 0, transform: 'translate(-8vw, 0px)' }}
      animate={{
        opacity: [0, 0.45, 0.45, 0],
        transform: ['translate(-8vw, 0px)', 'translate(28vw, -12px)', 'translate(62vw, 6px)', 'translate(96vw, -8px)']
      }}
      transition={{ duration: 22, ease: 'linear' }}
      onAnimationComplete={() => setPass(undefined)}>
      <SpookyMascot character='boo' mood='sleepy' />
    </motion.span>
  );
}

function BatSilhouette() {
  return (
    <svg viewBox='0 0 40 18'>
      <path d='M20 6 L18.6 3 L19.4 6.2 Q16 3 9 2 Q12 5 11 8 Q7 6.5 2 8 Q7 10 8 13 Q11 10 14 12 Q16 9 20 13 Q24 9 26 12 Q29 10 32 13 Q33 10 38 8 Q33 6.5 29 8 Q28 5 31 2 Q24 3 20.6 6.2 L21.4 3Z' />
    </svg>
  );
}

type Flight = { id: number; top: number; size: number; flock: number; reverse: boolean };

/** Now and then a few small bats cross the sky. */
function BatFlights({ enabled }: { enabled: boolean }) {
  const [flight, setFlight] = useState<Flight>();
  useEvery(() => setFlight((current) => current ?? {
    id: Date.now(), top: random(4, 16), size: random(14, 20), flock: 1 + Math.floor(Math.random() * 3), reverse: Math.random() < 0.4
  }), [28000, 55000], 9000, enabled);
  if (!flight) return null;
  // One transform string, so Motion can hand the flight to WAAPI.
  const xs = flight.reverse ? [108, 78, 48, 18, -12] : [-12, 18, 48, 78, 108];
  const ys = [0, 18, -6, 12, 0];
  return (
    <motion.div key={flight.id} className={flight.reverse ? 'hw-flight reverse' : 'hw-flight'} style={{ top: flight.top + 'vh' }}
      initial={{ transform: `translate(${xs[0]}vw, 0px)` }}
      animate={{ transform: xs.map((x, index) => `translate(${x}vw, ${ys[index]}px)`) }}
      transition={{ duration: 9, ease: 'easeInOut' }}
      onAnimationComplete={() => setFlight(undefined)}>
      {Array.from({ length: flight.flock }, (_, index) => (
        <span key={index} className='hw-bat' style={{ width: flight.size - index * 3, '--i': index } as CSSProperties}>
          <BatSilhouette />
        </span>
      ))}
    </motion.div>
  );
}

/** A witch on her broom, a silhouette crossing the sidebar's moon. */
function Witch() {
  return (
    <span className='hw-witch'>
      <svg viewBox='0 0 60 28'>
        <path d='M3 22.5 L45 15.5' stroke='#0a0712' strokeWidth={1.6} strokeLinecap='round' />
        <path d='M43.5 13.5 L57 8 L55 13 L59.5 15 L54.5 17 L57.5 22 L44.5 18.5Z' />
        <path d='M17.5 20 L31 18 L28.5 9 Z' />
        <path d='M28.5 10 Q39 10.5 43 16.5 Q35 14.5 29 15Z' />
        <circle cx={27.8} cy={8.2} r={2.5} />
        <path d='M23.5 7 L33.5 5.6 L30.6 5 L35 -2.5 L28 4.4 Z' />
        <path d='M20 19.5 L13.5 21.8 L14.3 23.2 L21 21 Z' />
        <path d='M23 17.5 L19 12.5 L20.4 12 L24.6 16.6 Z' />
      </svg>
    </span>
  );
}

/** Flap, a small bat asleep on the light string. */
function HangingBat() {
  const [awake, setAwake] = useState(false);
  return (
    <span className={awake ? 'hw-hanging awake' : 'hw-hanging'}
      onPointerEnter={() => setAwake(true)} onPointerLeave={() => window.setTimeout(() => setAwake(false), 1400)}>
      <span className='hw-thread' />
      <SpookyMascot character='flap' mood={awake ? 'spooked' : 'sleepy'} />
      <AnimatePresence>{awake && <SpookyBubble key='eek' className='hw-bubble-down'>Oh… it’s you.</SpookyBubble>}</AnimatePresence>
    </span>
  );
}

export function HalloweenAmbience() {
  const spooky = useHalloween();
  const reduce = useReducedMotion();
  if (!spooky) return null;
  const alive = !reduce;
  return (
    <>
      <div className='hw-ambient-back' aria-hidden='true'>
        <Stars enabled={alive} />
        <FogBank className='far' duration={34} enabled={alive} />
        <FogBank className='near' duration={46} enabled={alive} />
        {alive && <Motes enabled={alive} />}
        {alive && <Leaves enabled={alive} />}
        {alive && <PassingGhost enabled={alive} />}
      </div>
      <div className='hw-ambient-front' aria-hidden='true'>
        <FairyLights enabled={alive} />
        <Witch />
        <HangingBat />
        {alive && <BatFlights enabled={alive} />}
      </div>
    </>
  );
}
