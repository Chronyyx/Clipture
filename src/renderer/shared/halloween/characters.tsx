import { ink, SpookyFace, type SpookyMood } from './SpookyFace';

export type SpookyCharacter = 'jack' | 'boo' | 'soot' | 'flap' | 'wick' | 'brewster';

const rim = '#7d6ca8';
const fur = '#1a1128';
const glow = '#ffcf73';
const carve = { fill: glow, stroke: '#8a3a10', strokeWidth: 1, strokeLinejoin: 'round' } as const;
const outline = { stroke: ink, strokeWidth: 1.6, strokeLinejoin: 'round', strokeLinecap: 'round' } as const;
const rimLine = { stroke: rim, strokeWidth: 1.3, strokeLinejoin: 'round', strokeLinecap: 'round' } as const;

function chevron(x: number) {
  return `M${x - 8} 50 L${x} 40 L${x + 8} 50 L${x + 4} 50 L${x} 45 L${x - 4} 50Z`;
}

/** The glowing carving; its light flickers like a candle inside. */
function Carving({ mood }: { mood: SpookyMood }) {
  const nose = <path d='M47 57 L50 52.5 L53 57Z' {...carve} />;
  const grin = <path d='M32 62 Q50 80 68 62 L63 63.5 L60 68 L56 64.5 L50 69 L44 64.5 L40 68 L37 63.5Z' {...carve} />;
  const bigGrin = <path d='M31 61 Q50 86 69 61 Q60 66 50 66 Q40 66 31 61Z' {...carve} />;
  const triangle = (x: number) => <path d={`M${x - 7} 50 L${x} 38 L${x + 7} 50Z`} {...carve} />;
  const cut = (() => {
    switch (mood) {
      case 'sleepy':
        return (
          <>
            <path d='M34 47 q7 5 14 0 M52 47 q7 5 14 0' fill='none' stroke={glow} strokeWidth={3.2} strokeLinecap='round' />
            <ellipse cx={50} cy={66} rx={3.2} ry={2.6} {...carve} />
          </>
        );
      case 'spooked':
        return (
          <>
            <circle cx={41} cy={46} r={6} {...carve} />
            <circle cx={59} cy={46} r={6} {...carve} />
            {nose}
            <ellipse cx={50} cy={68} rx={5} ry={6} {...carve} />
          </>
        );
      case 'cheer':
        return <><path d={chevron(41)} {...carve} /><path d={chevron(59)} {...carve} />{nose}{bigGrin}</>;
      case 'wink':
        return <>{triangle(41)}<path d={chevron(59)} {...carve} />{nose}{bigGrin}</>;
      default:
        return <>{triangle(41)}{triangle(59)}{nose}{grin}</>;
    }
  })();
  return <g className='hw-carve'>{cut}</g>;
}

/** Jack: the jack-o'-lantern. Brave, cheerful, keeps watch over the recorder. */
function Jack({ mood, golden }: { mood: SpookyMood; golden?: boolean }) {
  const [side, middle, shine] = golden ? ['#d9a441', '#e8b955', '#f6dea0'] : ['#bf6128', '#d4712f', '#e8a063'];
  return (
    <>
      <path d='M47 22 q-2 -10 4 -15 l4 3 q-4 4 -3 12z' fill='#5d7a3a' {...outline} strokeWidth={1.4} />
      <g className='hw-leaf'>
        <path d='M54 13 q9 -9 17 -3 q-8 7 -17 3z' fill='#7fa35a' {...outline} strokeWidth={1.4} />
        <path d='M57 11.5 q6 -2.5 11 -1.5' fill='none' stroke={ink} strokeWidth={1.3} strokeLinecap='round' />
      </g>
      <ellipse cx={31} cy={57} rx={23} ry={30} fill={side} {...outline} />
      <ellipse cx={69} cy={57} rx={23} ry={30} fill={side} {...outline} />
      <ellipse cx={50} cy={56} rx={22} ry={33} fill={middle} {...outline} />
      <path d='M22 42 q-5 10 -3 22' fill='none' stroke={shine} strokeWidth={2.6} strokeLinecap='round' opacity={0.5} />
      <ellipse cx={26} cy={63} rx={4.6} ry={2.6} fill='#ff5f6d' opacity={0.18} />
      <ellipse cx={74} cy={63} rx={4.6} ry={2.6} fill='#ff5f6d' opacity={0.18} />
      <Carving mood={mood} />
    </>
  );
}

/** Boo: a shy little ghost who carries a lantern through empty rooms. */
function Boo({ mood, lantern }: { mood: SpookyMood; lantern?: boolean }) {
  return (
    <>
      <path d='M24 86 V46 C24 25 36 12 50 12 C64 12 76 25 76 46 V86 q-4.3 -6 -8.7 0 q-4.3 6 -8.7 0 q-4.3 -6 -8.6 0 q-4.3 6 -8.7 0 q-4.3 -6 -8.7 0 q-4.3 6 -8.6 0 Z'
        fill='#e9e4f7' {...outline} />
      <path d='M66 28 q7 11 5 32' fill='none' stroke='#cfc6e6' strokeWidth={3} strokeLinecap='round' />
      <ellipse cx={21} cy={58} rx={5.5} ry={4.2} fill='#e9e4f7' {...outline} strokeWidth={2} />
      <ellipse cx={79} cy={58} rx={5.5} ry={4.2} fill='#e9e4f7' {...outline} strokeWidth={2} />
      <SpookyFace left={[41, 44]} right={[59, 44]} mouth={[50, 53]} mood={mood} />
      {lantern && (
        <g transform='translate(86 60)'>
          <circle className='hw-lantern-glow' cy={10} r={15} fill={glow} opacity={0.22} />
          <path d='M-5 1 q5 -9 10 0' fill='none' {...outline} strokeWidth={1.8} />
          <rect x={-6} y={1} width={12} height={15} rx={3} fill={glow} {...outline} strokeWidth={1.8} />
          <path d='M-6 6 h12 M-6 11 h12' stroke='#e0a52c' strokeWidth={1.2} />
        </g>
      )}
    </>
  );
}

/** Soot: the witch's cat. Aloof, sits on the edge of things, tail swishing. */
function Soot({ mood }: { mood: SpookyMood }) {
  const happy = mood === 'cheer';
  const eye = (x: number) => mood === 'sleepy' || happy
    ? <path d={mood === 'sleepy' ? `M${x - 6} 37 q6 4 12 0` : `M${x - 6} 38 q6 -6 12 0`} fill='none' stroke='#e6cf7a' strokeWidth={2.4} strokeLinecap='round' />
    : (
      <g>
        <path d={`M${x - 6} 36 q6 -7.5 12 0 q-6 6.5 -12 0Z`} fill='#e6cf7a' />
        <ellipse cx={x} cy={36} rx={1.3} ry={3.4} fill={ink} />
      </g>
    );
  return (
    <>
      <path className='hw-tail' d='M80 64 C96 70 99 86 91 97 C89 100 84 99 85 96 C92 85 90 75 78 70Z' fill={fur} {...rimLine} />
      <path d='M28 70 C26 52 38 44 55 44 C72 44 84 52 82 70 Z' fill={fur} {...rimLine} />
      <ellipse cx={45} cy={71} rx={7} ry={5} fill={fur} {...rimLine} />
      <ellipse cx={65} cy={71} rx={7} ry={5} fill={fur} {...rimLine} />
      <path d='M38 27 L36 8 L50 20 Z M72 27 L74 8 L60 20Z' fill={fur} {...rimLine} />
      <path d='M40 22 L39 13 L46 19Z M70 22 L71 13 L64 19Z' fill='#6b3f8f' />
      <circle cx={55} cy={36} r={20} fill={fur} {...rimLine} />
      <g className={mood === 'sleepy' || happy ? '' : 'hw-slow-blink'}>{eye(46)}{eye(64)}</g>
      <path d='M53.5 43 h3 l-1.5 2z' fill='#ff9ab0' />
      <path d='M51 47 q2 2 4 0 q2 2 4 0' fill='none' stroke={rim} strokeWidth={1.4} strokeLinecap='round' />
      <path d='M35 43 l-10 -2 M35 47 l-10 1 M75 43 l10 -2 M75 47 l10 1' stroke={rim} strokeWidth={1.1} strokeLinecap='round' opacity={0.8} />
    </>
  );
}

/** Flap: a round little bat who would rather be asleep. */
function Flap({ mood }: { mood: SpookyMood }) {
  return (
    <>
      <path className='hw-wing hw-wing-left' d='M38 36 C30 20 14 18 4 26 C10 28 12 32 11 37 C16 33 21 34 23 40 C27 35 32 36 34 42Z' fill='#2b2145' {...rimLine} />
      <path className='hw-wing hw-wing-right' d='M62 36 C70 20 86 18 96 26 C90 28 88 32 89 37 C84 33 79 34 77 40 C73 35 68 36 66 42Z' fill='#2b2145' {...rimLine} />
      <path d='M40 29 L38 16 L47 25Z M60 29 L62 16 L53 25Z' fill='#2b2145' {...rimLine} />
      <circle cx={50} cy={38} r={14} fill='#2b2145' {...rimLine} />
      <ellipse cx={50} cy={45} rx={7} ry={5.5} fill='#3d2f63' />
      <path d='M46 52 v4 M54 52 v4' stroke={rim} strokeWidth={2} strokeLinecap='round' />
      <SpookyFace left={[44.5, 36]} right={[55.5, 36]} mouth={[50, 42.5]} mood={mood} color='#f3ead8' blush='#ff7fa0' />
      <path d='M48 43.5 l1.3 3 l1.3 -3z' fill='#fff' />
    </>
  );
}

/** Wick: a candle who dozes between pages; the flame grows when it wakes. */
function Wick({ mood }: { mood: SpookyMood }) {
  return (
    <>
      <circle className='hw-flame-glow' cx={40} cy={28} r={20} fill='#ffb14a' opacity={0.16} />
      <ellipse cx={40} cy={101} rx={30} ry={6} fill='#6d5a8f' {...outline} strokeWidth={2} />
      <path d='M69 99 q10 -6 3 -13' fill='none' {...outline} strokeWidth={2} />
      <rect x={24} y={46} width={32} height={55} rx={6} fill='#e9dfc9' {...outline} />
      <path d='M24 54 V52 Q24 46 30 46 H50 Q56 46 56 52 V58 Q53 64 51 58 Q49 54 47 60 Q44 70 41 60 Q39 54 36 58 Q33 62 31 56 Q28 52 24 54Z'
        fill='#fffaf0' {...outline} strokeWidth={1.8} />
      <path d='M40 46 v-6' stroke={ink} strokeWidth={2} strokeLinecap='round' />
      <g className={mood === 'sleepy' ? 'hw-flame dim' : 'hw-flame'}>
        <path d='M40 41 C30 35 32 23 40 9 C48 23 50 35 40 41Z' fill='#ff9a2e' />
        <path d='M40 39 C35 35 36 28 40 21 C44 28 45 35 40 39Z' fill='#ffe27a' />
      </g>
      <SpookyFace left={[33.5, 76]} right={[46.5, 76]} mouth={[40, 84]} mood={mood} />
    </>
  );
}

/** Brewster: the cauldron. Bubbles away while your clips are brewing. */
function Brewster({ mood }: { mood: SpookyMood }) {
  return (
    <>
      <g className='hw-fire'>
        <path d='M42 90 q-5 -8 1 -15 q1 6 5 6 q2 -7 -1 -13 q10 9 4 22z' fill='#ff8a2a' />
        <path d='M60 90 q-4 -7 1 -12 q1 5 4 5 q2 -5 0 -10 q8 8 3 17z' fill='#ffb14a' />
      </g>
      <path d='M30 78 l-5 10 M80 78 l5 10' stroke={rim} strokeWidth={3} strokeLinecap='round' />
      <path d='M16 40 Q14 80 55 82 Q96 80 94 40 Z' fill='#2a2140' {...rimLine} strokeWidth={2.4} />
      <path d='M10 44 q-6 6 0 12 M100 44 q6 6 0 12' fill='none' stroke={rim} strokeWidth={2.4} strokeLinecap='round' />
      <ellipse cx={55} cy={40} rx={42} ry={9} fill='#3a2e5a' {...rimLine} />
      <ellipse cx={55} cy={40} rx={36} ry={6} fill='#6f9f5e' />
      <g className='hw-brew-bubbles' fill='#a8cf96' stroke='#4f7d3e' strokeWidth={0.9}>
        <circle cx={44} cy={38} r={3.6} />
        <circle cx={61} cy={36} r={2.8} />
        <circle cx={71} cy={39} r={3.2} />
      </g>
      <SpookyFace left={[45, 60]} right={[65, 60]} mouth={[55, 68]} mood={mood} color='#f3ead8' />
    </>
  );
}

const viewBoxes: Record<SpookyCharacter, string> = {
  jack: '0 0 100 92',
  boo: '0 0 106 96',
  soot: '0 0 110 100',
  flap: '0 10 100 50',
  wick: '0 4 80 106',
  brewster: '0 26 110 66'
};

export function SpookyMascot({ character, mood = 'happy', className, golden, lantern }: {
  character: SpookyCharacter; mood?: SpookyMood; className?: string; golden?: boolean; lantern?: boolean;
}) {
  const drawing = {
    jack: <Jack mood={mood} golden={golden} />,
    boo: <Boo mood={mood} lantern={lantern} />,
    soot: <Soot mood={mood} />,
    flap: <Flap mood={mood} />,
    wick: <Wick mood={mood} />,
    brewster: <Brewster mood={mood} />
  }[character];
  return (
    <svg className={`hw-mascot hw-${character}${className ? ' ' + className : ''}`} viewBox={viewBoxes[character]}
      aria-hidden='true' focusable='false'>
      {drawing}
    </svg>
  );
}
