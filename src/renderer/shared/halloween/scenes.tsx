import { AnimatePresence, motion } from 'motion/react';
import { useEffect, useRef, useState, type CSSProperties, type ReactNode } from 'react';
import { SpookyMascot } from './characters';
import type { SpookyMood } from './SpookyFace';
import { useFlicker } from './useFlicker';
import { useHalloween } from './useHalloween';

const reveal = {
  initial: { opacity: 0, transform: 'translateY(3px)' },
  animate: { opacity: 1, transform: 'translateY(0px)' },
  exit: { opacity: 0, transform: 'translateY(2px)' },
  transition: { duration: 0.22, ease: 'easeOut' }
} as const;

/** A whisper of dark glass: quiet, small, never over controls. */
export function SpookyBubble({ children, className }: { children: ReactNode; className: string }) {
  return <motion.span className={'hw-bubble ' + className} {...reveal}>{children}</motion.span>;
}

/** A few embers drifting up from something good happening. */
export function EmberBurst({ count = 5, loop = false }: { count?: number; loop?: boolean }) {
  return (
    <span className={loop ? 'hw-embers loop' : 'hw-embers'} aria-hidden='true'>
      {Array.from({ length: count }, (_, index) => <i key={index} style={{ '--i': index } as CSSProperties} />)}
    </span>
  );
}

function CandleGlow({ className }: { className: string }) {
  const scope = useFlicker<HTMLSpanElement>();
  return <span ref={scope} className={className} aria-hidden='true' />;
}

export type RecorderSpookState = 'live' | 'warning' | 'waiting' | 'off';

const recorderMoods: Record<RecorderSpookState, SpookyMood> = { live: 'happy', warning: 'spooked', waiting: 'sleepy', off: 'sleepy' };
const recorderLines: Record<RecorderSpookState, string> = {
  live: 'Keeping watch.',
  warning: 'Something stirs…',
  waiting: 'Lighting the lantern…',
  off: 'All is quiet.'
};

function greeting(now = new Date()) {
  if (now.getMonth() === 9 && now.getDate() === 31) return 'Happy Halloween.';
  if (now.getHours() < 4) return 'The witching hour.';
  return 'Good evening.';
}

/** Jack, a small lantern-pumpkin on the recorder panel. Five clicks turn him gold. */
export function SpookyRecorderPet({ state }: { state: RecorderSpookState }) {
  const spooky = useHalloween();
  const [greet, setGreet] = useState(true);
  const [hover, setHover] = useState(false);
  const [golden, setGolden] = useState(false);
  const clicks = useRef(0);
  useEffect(() => {
    const timer = window.setTimeout(() => setGreet(false), 5200);
    return () => window.clearTimeout(timer);
  }, []);
  useEffect(() => {
    if (!golden) return;
    const timer = window.setTimeout(() => setGolden(false), 9000);
    return () => window.clearTimeout(timer);
  }, [golden]);
  if (!spooky) return null;
  const line = golden ? 'A golden pumpkin…' : greet && !hover ? greeting() : recorderLines[state];
  const mood: SpookyMood = golden ? 'cheer' : hover ? 'wink' : recorderMoods[state];
  return (
    <span className={'hw-recorder-pet hw-state-' + state + (golden ? ' golden' : '')} aria-hidden='true'
      onPointerEnter={() => setHover(true)} onPointerLeave={() => setHover(false)}
      onClick={() => { if (++clicks.current % 5 === 0) setGolden(true); }}>
      <CandleGlow className='hw-glow' />
      <SpookyMascot character='jack' mood={mood} golden={golden} />
      {golden && <EmberBurst count={6} loop />}
      <AnimatePresence>
        {(greet || hover || golden) && <SpookyBubble key={line} className='hw-bubble-left'>{line}</SpookyBubble>}
      </AnimatePresence>
    </span>
  );
}

/** Soot, a small black cat on the preview's top edge, tail over the side. */
export function SpookyPerch() {
  const spooky = useHalloween();
  const [petted, setPetted] = useState(false);
  if (!spooky) return null;
  return (
    <div className='hw-perch-anchor' aria-hidden='true'>
      <span className={petted ? 'hw-perch petted' : 'hw-perch'}
        onPointerEnter={() => setPetted(true)} onPointerLeave={() => setPetted(false)}>
        <SpookyMascot character='soot' mood={petted ? 'cheer' : 'happy'} />
        <AnimatePresence>{petted && <SpookyBubble key='mrrp' className='hw-bubble-left'>mrrp.</SpookyBubble>}</AnimatePresence>
      </span>
    </div>
  );
}

/** Boo, carrying a lantern through the empty library. */
export function SpookyEmptyArt({ imported }: { imported: boolean }) {
  if (!useHalloween()) return null;
  return (
    <div className='hw-empty-scene' aria-hidden='true'>
      {/* An endless drift is a CSS loop (compositor-only); Motion handles the
          one-off, interruptible motion elsewhere. */}
      <span className='hw-empty-boo'>
        <SpookyMascot character='boo' mood='shy' lantern />
      </span>
      <SpookyBubble className='hw-bubble-under'>{imported ? 'Bring me a folder of videos.' : 'Nothing here yet… only us.'}</SpookyBubble>
    </div>
  );
}

/** Brewster simmers quietly while the library loads. */
export function SpookyBrew() {
  if (!useHalloween()) return null;
  return (
    <span className='hw-brew' aria-hidden='true'>
      <span className='hw-brew-steam'><i /><i /><i /></span>
      <SpookyMascot character='brewster' mood='sleepy' />
    </span>
  );
}

/** Boo appears beside every notice, trailing a few embers. */
export function SpookyNoticePet() {
  if (!useHalloween()) return null;
  return (
    <motion.span className='hw-notice-pet' aria-hidden='true'
      initial={{ opacity: 0, transform: 'translateY(6px)' }} animate={{ opacity: 1, transform: 'translateY(0px)' }}
      transition={{ duration: 0.5, ease: 'easeOut' }}>
      <SpookyMascot character='boo' mood='happy' />
      <EmberBurst count={4} />
    </motion.span>
  );
}

/** Embers rise from the save button while a clip is kept. */
export function SpookySaveCheer({ saving }: { saving: boolean }) {
  const spooky = useHalloween();
  if (!spooky || !saving) return null;
  return <EmberBurst count={5} loop />;
}

const railLines: Record<string, string> = {
  recording: 'How far back shall I remember?',
  video: 'The picture, by candlelight.',
  audio: 'I hear whispers on every track.',
  notifications: 'I’ll ring a bell when a clip is kept.',
  storage: 'Where the clips rest.',
  appearance: 'Welcome to the night.'
};

/** Wick, a small candle at the foot of Settings; changing page wakes the flame. */
export function SpookyRailPet({ section }: { section: string }) {
  const spooky = useHalloween();
  const [awake, setAwake] = useState(false);
  useEffect(() => {
    setAwake(true);
    const timer = window.setTimeout(() => setAwake(false), 4200);
    return () => window.clearTimeout(timer);
  }, [section]);
  if (!spooky) return null;
  return (
    <span className='hw-rail-pet' aria-hidden='true'>
      <CandleGlow className={awake ? 'hw-glow' : 'hw-glow dim'} />
      <SpookyMascot character='wick' mood={awake ? 'happy' : 'sleepy'} />
      <AnimatePresence>
        {awake && railLines[section] && <SpookyBubble key={section} className='hw-bubble-down'>{railLines[section]}</SpookyBubble>}
      </AnimatePresence>
    </span>
  );
}
