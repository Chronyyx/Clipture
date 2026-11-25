import { AnimatePresence, motion } from 'motion/react';
import { useEffect, useRef, useState, type CSSProperties, type ReactNode } from 'react';
import { Candelabra, GothMascot } from './characters';
import { emptyLines, greeting, peekerLines, railLines, recorderLines, type GothRecorderState } from './lines';
import { useGothCafe } from './useGothCafe';

const reveal = {
  initial: { opacity: 0, transform: 'translateY(4px) scale(0.96)' },
  animate: { opacity: 1, transform: 'translateY(0px) scale(1)' },
  exit: { opacity: 0, transform: 'translateY(2px) scale(0.98)' },
  transition: { duration: 0.2, ease: 'easeOut' }
} as const;

function Bubble({ children, className }: { children: ReactNode; className: string }) {
  return <motion.span className={'gc-bubble ' + className} {...reveal}>{children}</motion.span>;
}

/** Little silver crosses and red petals drifting up: Goth café's confetti. */
function Relics({ count = 5, loop = false }: { count?: number; loop?: boolean }) {
  return (
    <span className={loop ? 'gc-relics loop' : 'gc-relics'} aria-hidden='true'>
      {Array.from({ length: count }, (_, index) => (
        <i key={index} className={index % 2 ? 'petal' : 'cross'} style={{ '--i': index } as CSSProperties} />
      ))}
    </span>
  );
}

export type { GothRecorderState };


/** A note card above the recorder, with Shade the cat peeking over the
 * recorder's edge beside it. The card greets you, then reports the recorder. */
export function GothRecorderPet({ state }: { state: GothRecorderState }) {
  const goth = useGothCafe();
  const [greet, setGreet] = useState(true);
  const [hover, setHover] = useState(false);
  useEffect(() => {
    const timer = window.setTimeout(() => setGreet(false), 6000);
    return () => window.clearTimeout(timer);
  }, []);
  if (!goth) return null;
  const line = greet ? greeting() : recorderLines[state];
  return (
    <span className={'gc-recorder-pet gc-state-' + state} aria-hidden='true'
      onPointerEnter={() => setHover(true)} onPointerLeave={() => setHover(false)}>
      <span className='gc-note'>
        <span key={line}>{line}</span>
      </span>
      <GothMascot character='shade' mood={hover || state === 'warning' ? 'shock' : 'smirk'} className='gc-note-cat' />
    </span>
  );
}

/** Shade the cat peeks over the top edge of the player and ducks if you stare. */
export function GothPeeker() {
  const goth = useGothCafe();
  const [caught, setCaught] = useState(false);
  const said = useRef(0);
  if (!goth) return null;
  return (
    <div className='gc-peek-anchor' aria-hidden='true'>
      <span className={caught ? 'gc-peeker caught' : 'gc-peeker'}
        onPointerEnter={() => { said.current = (said.current + 1) % peekerLines.length; setCaught(true); }}
        onPointerLeave={() => window.setTimeout(() => setCaught(false), 1200)}>
        <GothMascot character='shade' mood={caught ? 'shock' : 'smirk'} />
      </span>
      <AnimatePresence>
        {caught && <Bubble key={said.current} className='gc-bubble-peek'>{peekerLines[said.current]}</Bubble>}
      </AnimatePresence>
    </div>
  );
}

/** The empty library by candlelight: Vesper minds the clips, Raven the imports. */
export function GothEmptyArt({ imported }: { imported: boolean }) {
  if (!useGothCafe()) return null;
  return (
    <div className='gc-empty-scene' aria-hidden='true'>
      <Candelabra className='gc-empty-candles' />
      <GothMascot character={imported ? 'raven' : 'vesper'} mood='bored' className='gc-empty-girl' />
      <Bubble className='gc-bubble-side'>{imported ? emptyLines.imported : emptyLines.clips}</Bubble>
    </div>
  );
}

/** A candelabra burns while the library loads. */
export function GothLoading() {
  if (!useGothCafe()) return null;
  return <span className='gc-loading' aria-hidden='true'><Candelabra /></span>;
}

/** Grim swoops into every notice. */
export function GothNoticePet() {
  if (!useGothCafe()) return null;
  return (
    <motion.span className='gc-notice-pet' aria-hidden='true'
      initial={{ opacity: 0, transform: 'translate(-10px, -8px) rotate(-14deg)' }}
      animate={{ opacity: 1, transform: 'translate(0px, 0px) rotate(0deg)' }}
      transition={{ type: 'spring', bounce: 0.4, visualDuration: 0.45 }}>
      <GothMascot character='grim' mood='cheer' />
    </motion.span>
  );
}

/** Crosses and petals rise from the save button while a clip is kept. */
export function GothSaveCheer({ saving }: { saving: boolean }) {
  const goth = useGothCafe();
  if (!goth || !saving) return null;
  return <Relics count={6} loop />;
}

/** Nyx dozes at the foot of Settings and mutters when you change page. */
export function GothRailPet({ section }: { section: string }) {
  const goth = useGothCafe();
  const [awake, setAwake] = useState(false);
  useEffect(() => {
    setAwake(true);
    const timer = window.setTimeout(() => setAwake(false), 4200);
    return () => window.clearTimeout(timer);
  }, [section]);
  if (!goth) return null;
  return (
    <span className='gc-rail-pet' aria-hidden='true'>
      <GothMascot character='nyx' mood={awake ? 'bored' : 'sleepy'} />
      <AnimatePresence>
        {awake && railLines[section] && <Bubble key={section} className='gc-bubble-down'>{railLines[section]}</Bubble>}
      </AnimatePresence>
    </span>
  );
}
