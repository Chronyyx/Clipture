import { useId, type ReactNode } from 'react';
import { GothFace, liner, type GothMood, type Makeup } from './GothFace';

export type GothCharacter = 'vesper' | 'raven' | 'nyx' | 'grim' | 'shade';

const skin = '#f4eaee';
const skinEdge = '#2a1a2e';
const dress = '#151018';
const lace = '#3d3045';
const silver = '#d8d0e2';
const edge = { stroke: liner, strokeWidth: 1.6, strokeLinejoin: 'round', strokeLinecap: 'round' } as const;

export function Cross({ x, y, size = 1, color = silver }: { x: number; y: number; size?: number; color?: string }) {
  return (
    <path transform={`translate(${x} ${y}) scale(${size})`} fill={color} stroke={liner} strokeWidth={0.7} strokeLinejoin='round'
      d='M-1 -4.5 H1 V-2 H3 V0 H1 V5.5 H-1 V0 H-3 V-2 H-1Z' />
  );
}

function Shoulders({ children }: { children?: ReactNode }) {
  return (
    <>
      <path d='M12 100 C14 86 30 79 50 79 C70 79 86 86 88 100Z' fill={dress} {...edge} />
      {children}
      <path d='M44 63 L44 79 Q50 83 56 79 L56 63Z' fill={skin} stroke={skinEdge} strokeWidth={1.2} />
      <path d='M44 70 Q50 73 56 70' fill='none' stroke='#d9c3cc' strokeWidth={1.4} />
    </>
  );
}

function Head() {
  return (
    <>
      <ellipse cx={50} cy={46} rx={22.5} ry={24} fill={skin} stroke={skinEdge} strokeWidth={1.4} />
      <ellipse cx={36} cy={57.5} rx={4} ry={2} fill='#c45a7a' opacity={0.16} />
      <ellipse cx={64} cy={57.5} rx={4} ry={2} fill='#c45a7a' opacity={0.16} />
    </>
  );
}

const vesperMakeup: Makeup = { iris: '#c0144e', shadow: '#3b1630', lips: '#1d0d16' };

/** Vesper runs the bar: long black hair, blunt fringe, black lips, a lace
 * headband with a cross. She keeps the recorder. */
function Vesper({ mood }: { mood: GothMood }) {
  const hair = '#120e16';
  return (
    <>
      <path d='M24 44 C22 18 36 10 50 10 C64 10 78 18 76 44 L80 92 Q64 96 59 80 L41 80 Q36 96 20 92Z' fill={hair} {...edge} />
      <Shoulders>
        <path d='M33 84 q2 2.4 4 0 q2 2.4 4 0 q2 2.4 4 0 M55 84 q2 2.4 4 0 q2 2.4 4 0 q2 2.4 4 0' fill='none' stroke={lace} strokeWidth={1.2} />
        <path d='M46 88 L54 92 M54 88 L46 92 M46 93 L54 97 M54 93 L46 97' stroke='#8f4fe0' strokeWidth={1.2} strokeLinecap='round' />
      </Shoulders>
      <path d='M43.5 73 Q50 75.6 56.5 73' fill='none' stroke={liner} strokeWidth={2.8} strokeLinecap='round' />
      <Cross x={50} y={79.5} size={0.85} />
      <Head />
      <path d='M26.5 48 C24 22 37 13 50 13 C63 13 76 22 73.5 48 C73 42 72.5 39 71 37 L29 37 C27.5 39 27 42 26.5 48Z' fill={hair} {...edge} />
      <path d='M26.5 44 Q24.5 66 29 82 L33.5 81 Q30 64 31 44Z M73.5 44 Q75.5 66 71 82 L66.5 81 Q70 64 69 44Z' fill={hair} {...edge} />
      <path d='M35 20 Q50 14 65 20' fill='none' stroke='#3a2f45' strokeWidth={2} strokeLinecap='round' opacity={0.8} />
      <path d='M28 27 Q50 7 72 27' fill='none' stroke='#2b2233' strokeWidth={3.6} strokeLinecap='round' />
      <path d='M30 25.5 q2 -3 4 -2 M36 20.8 q2 -3 4 -1.6 M44 18 q2 -3 4 -1 M52 18 q2 -2 4 1 M60 19.6 q2 -1.6 4 2 M66 23 q2 -0.6 3.4 3' fill='none' stroke='#5a4b66' strokeWidth={1} />
      <Cross x={50} y={13.5} size={0.75} />
      <GothFace left={[41, 50]} right={[59, 50]} mouth={[50, 62.5]} mood={mood} makeup={vesperMakeup} />
    </>
  );
}

const ravenMakeup: Makeup = { iris: '#8a4fe8', shadow: '#4a1a5c', lips: '#a3102e' };

/** Raven: twin tails with a violet streak, red lips, a spiked choker. Nosy;
 * peeks over the top of the player. */
function Raven({ mood }: { mood: GothMood }) {
  const hair = '#1c1024';
  return (
    <>
      <g className='gc-tail gc-tail-left'>
        <path d='M29 30 C14 34 7 58 13 86 C15 92 22 92 22 86 C18 64 22 46 33 38Z' fill={hair} {...edge} />
      </g>
      <g className='gc-tail gc-tail-right'>
        <path d='M71 30 C86 34 93 58 87 86 C85 92 78 92 78 86 C82 64 78 46 67 38Z' fill={hair} {...edge} />
        <path d='M80 44 C86 56 86 70 83 82' fill='none' stroke='#8f4fe0' strokeWidth={2.4} strokeLinecap='round' />
      </g>
      <path d='M25 46 C23 18 37 11 50 11 C63 11 77 18 75 46 L74 66 L26 66Z' fill={hair} {...edge} />
      <Shoulders>
        <path d='M20 92 L30 82 M27 98 L40 83 M80 92 L70 82 M73 98 L60 83 M20 86 L34 98 M80 86 L66 98' stroke={lace} strokeWidth={0.9} />
      </Shoulders>
      <path d='M43 72.5 Q50 75.4 57 72.5' fill='none' stroke={liner} strokeWidth={3.4} strokeLinecap='round' />
      <path d='M44.2 74.2 l1.1 2.6 l1.1 -2.4 M48.9 75 l1.1 2.7 l1.1 -2.7 M53.6 74.4 l1.1 2.4 l1.1 -2.6' fill={silver} stroke={liner} strokeWidth={0.5} />
      <Head />
      <path d='M26 50 C23 21 37 13 50 13 C64 13 77 21 74 50 C72 40 66 33 56 33 C50 37 40 41 30 41 C28 44 27 47 26 50Z' fill={hair} {...edge} />
      <path d='M57 14 C62 21 62 28 57 34' fill='none' stroke='#8f4fe0' strokeWidth={3} strokeLinecap='round' />
      <circle cx={29.5} cy={31} r={3.2} fill='#2b2233' {...edge} strokeWidth={1.2} />
      <circle cx={70.5} cy={31} r={3.2} fill='#2b2233' {...edge} strokeWidth={1.2} />
      <Cross x={29.5} y={31} size={0.45} />
      <Cross x={70.5} y={31} size={0.45} />
      <GothFace left={[41, 51]} right={[59, 51]} mouth={[50, 63]} mood={mood} makeup={ravenMakeup} />
      <circle cx={64.5} cy={58} r={0.85} fill={liner} />
    </>
  );
}

const nyxMakeup: Makeup = { iris: '#b9a6ff', shadow: '#5a3a7a', lips: '#4a1238' };

/** Nyx: a silver bob, plum lips and a black rose. Mostly asleep at the foot
 * of Settings. */
function Nyx({ mood }: { mood: GothMood }) {
  const hair = '#dcd6e6';
  const shade = '#a79fba';
  return (
    <>
      <path d='M24 46 C22 18 37 10 50 10 C63 10 78 18 76 46 L78 67 Q68 71 62 64 L38 64 Q32 71 22 67Z' fill={hair} {...edge} />
      <Shoulders>
        <path d='M40 79 L44 86 L50 81 L56 86 L60 79' fill={dress} stroke={lace} strokeWidth={1.2} strokeLinejoin='round' />
      </Shoulders>
      <Cross x={50} y={90} size={1.15} />
      <Head />
      <path d='M26 50 C24 22 37 13 50 13 C63 13 76 22 74 50 C72 38 62 30 50 24 C38 30 28 38 26 50Z' fill={hair} {...edge} />
      <path d='M33 38 Q39 28 48 26 M67 38 Q61 28 52 26 M28 56 Q27 62 29 66 M72 56 Q73 62 71 66' fill='none' stroke={shade} strokeWidth={1.3} strokeLinecap='round' />
      <g className='gc-rose'>
        <circle cx={69} cy={23} r={5.6} fill='#3a0a1a' {...edge} strokeWidth={1.2} />
        <path d='M69 23 m-2.6 0 a2.6 2.6 0 1 1 2.6 2.6 a1.6 1.6 0 1 1 -1.4 -2.2' fill='none' stroke='#8e1a3a' strokeWidth={1.1} strokeLinecap='round' />
        <path d='M73.5 27 q4 1 5 4.6 q-3.6 0.4 -5 -2.6z' fill='#1f2a22' stroke={liner} strokeWidth={0.8} />
      </g>
      <GothFace left={[41, 50]} right={[59, 50]} mouth={[50, 62]} mood={mood} makeup={nyxMakeup} />
    </>
  );
}

/** Grim, the bar's bat. Small, rude, delivers news. */
function Grim({ mood }: { mood: GothMood }) {
  const fur = '#1a1220';
  const rim = '#6d5a85';
  const shut = mood === 'sleepy';
  return (
    <>
      <g className='gc-wing gc-wing-left'>
        <path d='M38 38 Q24 20 6 24 Q14 30 12 38 Q20 36 22 44 Q28 40 36 46Z' fill={fur} stroke={rim} strokeWidth={1.3} strokeLinejoin='round' />
      </g>
      <g className='gc-wing gc-wing-right'>
        <path d='M62 38 Q76 20 94 24 Q86 30 88 38 Q80 36 78 44 Q72 40 64 46Z' fill={fur} stroke={rim} strokeWidth={1.3} strokeLinejoin='round' />
      </g>
      <path d='M38 30 L36 14 L46 24Z M62 30 L64 14 L54 24Z' fill={fur} stroke={rim} strokeWidth={1.3} strokeLinejoin='round' />
      <ellipse cx={50} cy={38} rx={15} ry={14} fill={fur} stroke={rim} strokeWidth={1.3} />
      {shut ? (
        <path d='M41 36 q3 2.4 6 0 M53 36 q3 2.4 6 0' fill='none' stroke='#ff2e5e' strokeWidth={1.6} strokeLinecap='round' />
      ) : (
        <g className='gc-blinks'>
          <path d='M40.5 35 L47.5 37 Q46 40 43 39.6 Q40.6 38.6 40.5 35Z M59.5 35 L52.5 37 Q54 40 57 39.6 Q59.4 38.6 59.5 35Z' fill='#ff2e5e' />
        </g>
      )}
      <path d='M46 44 Q50 46.4 54 44' fill='none' stroke={rim} strokeWidth={1.2} strokeLinecap='round' />
      <path d='M47.2 44.6 l0.9 2.4 l0.9 -2.1 M51 44.9 l0.9 2.1 l0.9 -2.4' fill='#fff' />
    </>
  );
}

/** Shade, the café's black cat: shaded fur, amber almond eyes with slit
 * pupils, and paws hooked over whatever she is peeking over. */
function Shade({ mood }: { mood: GothMood }) {
  const id = useId().replace(/:/g, '');
  const wide = mood === 'shock';
  const shut = mood === 'sleepy';
  const lidded = mood === 'smirk' || mood === 'bored';
  const eye = (x: number, side: -1 | 1) => {
    // Almond eyes, tilted up toward the outer corner.
    const inner = x - 7.5 * side;
    const outer = x + 7.5 * side;
    const top = wide ? 7 : lidded ? 3.4 : 5.4;
    const shape = `M${inner} 51 C${x - 3 * side} ${51 - top * 1.3} ${x + 4 * side} ${49 - top * 1.3} ${outer} 47.6 C${x + 4 * side} 55.6 ${x - 3 * side} 56 ${inner} 51Z`;
    if (shut) return <path d={`M${inner} 51 Q${x} 55 ${outer} 48`} fill='none' stroke='#6b5a66' strokeWidth={1.4} strokeLinecap='round' />;
    return (
      <g>
        <clipPath id={`${id}-eye${side}`}><path d={shape} /></clipPath>
        <path d={shape} fill={`url(#${id}-iris)`} />
        <g clipPath={`url(#${id}-eye${side})`}>
          <ellipse cx={x} cy={50.6} rx={wide ? 4.2 : 1.3} ry={wide ? 5 : 5.6} fill='#050305' />
          <ellipse cx={x + 2.2} cy={48} rx={1.4} ry={1} fill='#fff' opacity={0.85} />
          {lidded && <path d={`M${inner - 1} 44 H${outer + 1} V${49.2} C${x + 3 * side} 48 ${x - 3 * side} 48.6 ${inner - 1} 49.8Z`} fill='#141015' />}
        </g>
        <path d={shape} fill='none' stroke='#050305' strokeWidth={1.1} />
      </g>
    );
  };
  return (
    <>
      <defs>
        <radialGradient id={`${id}-fur`} cx='0.5' cy='0.42' r='0.62'>
          <stop offset='0' stopColor='#2c242b' />
          <stop offset='0.7' stopColor='#151116' />
          <stop offset='1' stopColor='#0b080b' />
        </radialGradient>
        <linearGradient id={`${id}-ear`} x1='0' y1='0' x2='0' y2='1'>
          <stop offset='0' stopColor='#241a21' />
          <stop offset='1' stopColor='#6a4257' />
        </linearGradient>
        <radialGradient id={`${id}-iris`} cx='0.5' cy='0.55' r='0.6'>
          <stop offset='0' stopColor='#f6d98c' />
          <stop offset='0.6' stopColor='#d9a64a' />
          <stop offset='1' stopColor='#8a5a1e' />
        </radialGradient>
      </defs>
      {/* Ears, with pink-dark inners and a few tufts of fur. */}
      <path d='M21 44 C19 30 19 17 23 5 C32 10 40 19 46 28Z M79 44 C81 30 81 17 77 5 C68 10 60 19 54 28Z' fill={`url(#${id}-fur)`} stroke='#55434f' strokeWidth={1} strokeLinejoin='round' />
      <path d='M25 36 C24 26 24.5 18 26 12 C32 16 36.5 21 40.5 27.5Z M75 36 C76 26 75.5 18 74 12 C68 16 63.5 21 59.5 27.5Z' fill={`url(#${id}-ear)`} />
      <path d='M27 30 l5 -4 M28 26 l4 -2.6 M73 30 l-5 -4 M72 26 l-4 -2.6' stroke='#8a7484' strokeWidth={0.6} strokeLinecap='round' opacity={0.7} />
      {/* Head: wide at the cheeks, with ruffs of fur at either side. */}
      <path d='M50 25 C65 25 77 32 81 45 L85 50 L81.5 53 L85 57 L80 59 C77 68 66 75 50 75 C34 75 23 68 20 59 L15 57 L18.5 53 L15 50 L19 45 C23 32 35 25 50 25Z'
        fill={`url(#${id}-fur)`} stroke='#55434f' strokeWidth={1} strokeLinejoin='round' />
      <path d='M44 31 q1 4 0 7 M50 30 v7.5 M56 31 q-1 4 0 7' stroke='#2f262d' strokeWidth={1.1} strokeLinecap='round' fill='none' />
      <path d='M37 43 Q35 39 31 40 M63 43 Q65 39 69 40' stroke='#2f262d' strokeWidth={1} fill='none' strokeLinecap='round' />
      {eye(37, -1)}
      {eye(63, 1)}
      {/* Muzzle, nose and mouth. */}
      <ellipse cx={44.6} cy={63.4} rx={5.4} ry={4} fill='#1e181d' />
      <ellipse cx={55.4} cy={63.4} rx={5.4} ry={4} fill='#1e181d' />
      <path d='M50 55 C49 57 48 58.4 46.6 59.2 L53.4 59.2 C52 58.4 51 57 50 55Z' fill='#191318' opacity={0.9} />
      <path d='M47.2 59.4 L52.8 59.4 Q52.2 62 50 62.8 Q47.8 62 47.2 59.4Z' fill='#4a3340' />
      <path d='M48.6 60 q1.4 -0.6 2.6 0' stroke='#7a5a6a' strokeWidth={0.6} fill='none' strokeLinecap='round' />
      <path d='M50 62.8 v1.4 M50 64.2 q-1.8 2.2 -4.2 1.2 M50 64.2 q1.8 2.2 4.2 1.2' fill='none' stroke='#050305' strokeWidth={0.9} strokeLinecap='round' />
      {mood === 'shock' && <ellipse cx={50} cy={67.4} rx={1.8} ry={1.4} fill='#4a2a38' />}
      <g fill='none' stroke='#d9ccd4' strokeWidth={0.5} strokeLinecap='round' opacity={0.55}>
        <path d='M42 62 Q28 58 16 60 M42 64 Q28 64 15 67 M42.6 65.6 Q31 68 21 73' />
        <path d='M58 62 Q72 58 84 60 M58 64 Q72 64 85 67 M57.4 65.6 Q69 68 79 73' />
        <path d='M33.6 42 q-2 -4 -1.4 -8 M66.4 42 q2 -4 1.4 -8' />
      </g>
      <g fill='#3a2e38'>
        <circle cx={45.4} cy={63} r={0.45} /><circle cx={43.4} cy={64.2} r={0.45} /><circle cx={54.6} cy={63} r={0.45} /><circle cx={56.6} cy={64.2} r={0.45} />
      </g>
      {/* Paws over the edge, toes parted. */}
      <g className='gc-paws'>
        <path d='M21 80 C21 74 26 71 31 71 C36 71 41 74 41 80 Q31 83 21 80Z M59 80 C59 74 64 71 69 71 C74 71 79 74 79 80 Q69 83 59 80Z'
          fill={`url(#${id}-fur)`} stroke='#55434f' strokeWidth={1} />
        <path d='M27 80 v-3.6 M31 80.6 v-4 M35 80 v-3.6 M65 80 v-3.6 M69 80.6 v-4 M73 80 v-3.6' stroke='#050305' strokeWidth={0.9} strokeLinecap='round' />
      </g>
    </>
  );
}

const drawings = { vesper: Vesper, raven: Raven, nyx: Nyx, grim: Grim, shade: Shade };
const viewBoxes: Record<GothCharacter, string> = { vesper: '0 0 100 100', raven: '0 0 100 100', nyx: '0 0 100 100', grim: '0 6 100 50', shade: '0 0 100 84' };

export function GothMascot({ character, mood = 'smirk', className }: { character: GothCharacter; mood?: GothMood; className?: string }) {
  const Drawing = drawings[character];
  return (
    <svg className={`gc-mascot gc-${character}${className ? ' ' + className : ''}`} viewBox={viewBoxes[character]}
      aria-hidden='true' focusable='false'>
      <Drawing mood={mood} />
    </svg>
  );
}

/** Three tapers on an iron stand; the flames sway in CSS. */
export function Candelabra({ className }: { className?: string }) {
  const flame = (x: number, y: number) => (
    <g className='gc-flame'>
      <path d={`M${x} ${y} c-3.4 -2.6 -2.6 -7 0 -11 c2.6 4 3.4 8.4 0 11z`} fill='#ffb35c' />
      <path d={`M${x} ${y - 1} c-1.6 -1.4 -1.2 -3.6 0 -5.6 c1.2 2 1.6 4.2 0 5.6z`} fill='#fff1c9' />
    </g>
  );
  return (
    <svg className={'gc-candelabra' + (className ? ' ' + className : '')} viewBox='0 0 60 80' aria-hidden='true' focusable='false'>
      <g fill='none' stroke='#8c7a4a' strokeWidth={2.2} strokeLinecap='round'>
        <path d='M30 76 V40 M14 46 Q14 56 30 56 Q46 56 46 46' />
        <path d='M20 78 Q30 70 40 78' />
      </g>
      <rect x={10.5} y={30} width={7} height={16} rx={1} fill='#ece3d3' />
      <rect x={26.5} y={22} width={7} height={18} rx={1} fill='#ece3d3' />
      <rect x={42.5} y={30} width={7} height={16} rx={1} fill='#ece3d3' />
      <path d='M12 32 v5 M44 32 v3.5 M28 24 v6' stroke='#cfc3ae' strokeWidth={1.6} strokeLinecap='round' />
      {flame(14, 30)}
      {flame(30, 22)}
      {flame(46, 30)}
    </svg>
  );
}
