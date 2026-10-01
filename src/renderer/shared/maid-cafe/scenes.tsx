import { AnimatePresence, motion } from 'motion/react';
import { useEffect, useState, type CSSProperties, type ReactNode } from 'react';
import { Heart, Mascot } from './characters';
import type { Mood } from './Face';
import { useMaidCafe } from './useMaidCafe';

const pop = {
  initial: { opacity: 0, scale: 0.6, y: 6 },
  animate: { opacity: 1, scale: 1, y: 0 },
  exit: { opacity: 0, scale: 0.8, y: 4 },
  transition: { type: 'spring', bounce: 0.45, visualDuration: 0.3 }
} as const;

function Bubble({ children, className }: { children: ReactNode; className?: string }) {
  return <motion.span className={'mc-bubble' + (className ? ' ' + className : '')} {...pop}>{children}</motion.span>;
}

function HeartBurst({ count = 5 }: { count?: number }) {
  return (
    <span className='mc-heart-burst' aria-hidden='true'>
      {Array.from({ length: count }, (_, index) => (
        <svg key={index} viewBox='-7 -7 14 14' style={{ '--i': index } as CSSProperties}><Heart x={0} y={0} /></svg>
      ))}
    </span>
  );
}

function Sleeping() {
  return <span className='mc-zzz' aria-hidden='true'><i>z</i><i>z</i><i>Z</i></span>;
}

export type RecorderMood = 'live' | 'warning' | 'waiting' | 'off';

const recorderMoods: Record<RecorderMood, Mood> = { live: 'happy', warning: 'worried', waiting: 'sleepy', off: 'worried' };
const recorderLines: Record<RecorderMood, string> = {
  live: 'Recording! Your moments are safe♡',
  warning: 'Something’s off, still recording!',
  waiting: 'Getting the camera ready…',
  off: 'The recorder isn’t running yet.'
};

/** Mochi perches on the recorder panel, greets you once and mirrors its state. */
export function CafeRecorderPet({ state }: { state: RecorderMood }) {
  const cafe = useMaidCafe();
  const [greeting, setGreeting] = useState(true);
  const [petted, setPetted] = useState(false);
  useEffect(() => {
    const timer = window.setTimeout(() => setGreeting(false), 5200);
    return () => window.clearTimeout(timer);
  }, []);
  if (!cafe) return null;
  const mood: Mood = petted ? 'wink' : recorderMoods[state];
  return (
    <span className={'mc-recorder-pet mc-state-' + state} aria-hidden='true'
      onPointerEnter={() => setPetted(true)} onPointerLeave={() => setPetted(false)}>
      <Mascot character='mochi' mood={mood} />
      {state === 'waiting' && !petted && <Sleeping />}
      {petted && <HeartBurst count={3} />}
      <AnimatePresence>
        {(greeting || petted) && (
          <Bubble key={greeting && !petted ? 'hello' : 'state'} className='mc-bubble-left'>
            {greeting && !petted ? 'Welcome home! ♡' : recorderLines[state]}
          </Bubble>
        )}
      </AnimatePresence>
    </span>
  );
}

/** Azuki peeks over the edge of the preview, and ducks when you come close. */
export function CafePeeker() {
  const cafe = useMaidCafe();
  const [shy, setShy] = useState(false);
  if (!cafe) return null;
  return (
    <div className='mc-peek-anchor' aria-hidden='true'>
      <span className={shy ? 'mc-peeker shy' : 'mc-peeker'} onPointerEnter={() => setShy(true)}
        onPointerLeave={() => window.setTimeout(() => setShy(false), 900)}>
        <Mascot character='azuki' mood={shy ? 'shy' : 'happy'} />
      </span>
    </div>
  );
}

/** Replaces the empty-library art: Mochi serves an empty tray while Pip waits. */
export function CafeEmptyArt({ imported }: { imported: boolean }) {
  if (!useMaidCafe()) return null;
  return (
    <div className='mc-empty-scene' aria-hidden='true'>
      <Mascot character='mochi' mood='happy' className='mc-empty-mochi' />
      <span className='mc-tray' />
      <Mascot character='pip' mood='wink' className='mc-empty-pip' />
      <Bubble className='mc-bubble-side'>{imported ? 'Bring me a folder of videos?' : 'Nothing on the menu yet!'}</Bubble>
    </div>
  );
}

/** Purin naps on the loading library until the clips arrive. */
export function CafeNap() {
  if (!useMaidCafe()) return null;
  return (
    <span className='mc-nap' aria-hidden='true'>
      <Mascot character='purin' mood='sleepy' />
      <Sleeping />
    </span>
  );
}

/** Pip pops into every notice with a little burst of hearts. */
export function CafeNoticePet() {
  if (!useMaidCafe()) return null;
  return (
    <span className='mc-notice-pet' aria-hidden='true'>
      <Mascot character='pip' mood='cheer' />
      <HeartBurst />
    </span>
  );
}

/** Hearts float up from the save button while a clip is being plated. */
export function CafeSaveCheer({ saving }: { saving: boolean }) {
  const cafe = useMaidCafe();
  if (!cafe || !saving) return null;
  return <HeartBurst count={6} />;
}

const railLines: Record<string, string> = {
  recording: 'How long should each clip be, master?',
  video: 'Pretty pictures, coming right up!',
  audio: 'I listen to every track for you♡',
  notifications: 'Ding! I’ll tell you when a clip is served.',
  storage: 'Every clip gets its own little box.',
  appearance: 'You picked the café! Welcome home♡'
};

/** Purin dozes at the foot of the settings list and wakes when you change page. */
export function CafeRailPet({ section }: { section: string }) {
  const cafe = useMaidCafe();
  const [awake, setAwake] = useState(false);
  useEffect(() => {
    setAwake(true);
    const timer = window.setTimeout(() => setAwake(false), 4200);
    return () => window.clearTimeout(timer);
  }, [section]);
  if (!cafe) return null;
  return (
    <span className='mc-rail-pet' aria-hidden='true'>
      <AnimatePresence>
        {awake && railLines[section] && <Bubble key={section} className='mc-bubble-down'>{railLines[section]}</Bubble>}
      </AnimatePresence>
      <Mascot character='purin' mood={awake ? 'happy' : 'sleepy'} />
      {!awake && <Sleeping />}
    </span>
  );
}
