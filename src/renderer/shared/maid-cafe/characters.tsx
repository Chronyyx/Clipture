import { Face, type Mood } from './Face';

export type Character = 'mochi' | 'azuki' | 'purin' | 'pip';

const ink = '#5b2a4e';
const line = { stroke: ink, strokeWidth: 2.4, strokeLinejoin: 'round', strokeLinecap: 'round' } as const;

export function Bow({ x, y, scale = 1, color = '#ff7aa8' }: { x: number; y: number; scale?: number; color?: string }) {
  return (
    <g transform={`translate(${x} ${y}) scale(${scale})`} className='mc-bow'>
      <path d='M-1.5 1 L-6 10 L-2.5 9 Z M1.5 1 L6 10 L2.5 9 Z' fill={color} {...line} strokeWidth={1.6} />
      <path d='M0 0 C-4 -7 -12 -7 -11 0 C-12 7 -4 7 0 0Z' fill={color} {...line} strokeWidth={1.8} />
      <path d='M0 0 C4 -7 12 -7 11 0 C12 7 4 7 0 0Z' fill={color} {...line} strokeWidth={1.8} />
      <circle r={2.8} fill={color} {...line} strokeWidth={1.8} />
    </g>
  );
}

export function Heart({ x, y, scale = 1, color = '#ff6f9f' }: { x: number; y: number; scale?: number; color?: string }) {
  return (
    <path transform={`translate(${x} ${y}) scale(${scale})`} fill={color}
      d='M0 3.5 C-5 -0.5 -6 -4 -3.2 -5.5 C-1.6 -6.3 -0.4 -5.4 0 -4.2 C0.4 -5.4 1.6 -6.3 3.2 -5.5 C6 -4 5 -0.5 0 3.5Z' />
  );
}

/** Mochi: the bunny head maid. Cheerful, greets you, keeps the recorder warm. */
function Mochi({ mood }: { mood: Mood }) {
  return (
    <>
      <g className='mc-ear mc-ear-left'>
        <path d='M36 44 C24 34 24 6 34 4 C44 2 46 30 42 44 Z' fill='#fff' {...line} />
        <path d='M36 39 C31 31 31 13 34 11 C38 10 39 27 39 39Z' fill='#ffc2d6' />
      </g>
      <g className='mc-ear mc-ear-right'>
        <path d='M64 44 C76 34 76 6 66 4 C56 2 54 30 58 44 Z' fill='#fff' {...line} />
        <path d='M64 39 C69 31 69 13 66 11 C62 10 61 27 61 39Z' fill='#ffc2d6' />
      </g>
      <path d='M16 70 C16 47 31 37 50 37 C69 37 84 47 84 70 C84 87 69 95 50 95 C31 95 16 87 16 70Z' fill='#fff' />
      <path d='M18.5 80 Q50 71 81.5 80 C78 89 66 95 50 95 C34 95 22 89 18.5 80Z' fill='#ffb3cb' />
      <path d='M36 77 Q50 73 64 77 L65 89 Q50 95 35 89 Z' fill='#fff' {...line} strokeWidth={2} />
      <Heart x={50} y={84} scale={0.7} />
      <path d='M16 70 C16 47 31 37 50 37 C69 37 84 47 84 70 C84 87 69 95 50 95 C31 95 16 87 16 70Z' fill='none' {...line} />
      <path d='M28 46 Q50 35 72 46' fill='none' stroke='#ff9dbd' strokeWidth={3.2} strokeLinecap='round' />
      <path d='M29 45 Q33 37 37.5 42 Q41.5 35 46 40 Q50 33 54 40 Q58.5 35 62.5 42 Q67 37 71 45' fill='#fff' {...line} strokeWidth={2} />
      <Bow x={50} y={71} scale={0.62} />
      <Face left={[40.5, 56]} right={[59.5, 56]} mouth={[50, 63.5]} mood={mood} />
      <ellipse className='mc-paw' cx={27} cy={78} rx={5.5} ry={4.5} fill='#fff' {...line} strokeWidth={2} />
      <ellipse className='mc-paw mc-paw-right' cx={73} cy={78} rx={5.5} ry={4.5} fill='#fff' {...line} strokeWidth={2} />
    </>
  );
}

/** Azuki: the shy cocoa kitten. Mostly seen peeking over the edge of things. */
function Azuki({ mood }: { mood: Mood }) {
  return (
    <>
      <g className='mc-ear mc-ear-left'>
        <path d='M24 40 L20 8 Q22 5 26 8 L46 26 Z' fill='#d49a86' {...line} />
        <path d='M27 33 L25 15 L39 27Z' fill='#ffc2d6' />
      </g>
      <g className='mc-ear mc-ear-right'>
        <path d='M76 40 L80 8 Q78 5 74 8 L54 26 Z' fill='#d49a86' {...line} />
        <path d='M73 33 L75 15 L61 27Z' fill='#ffc2d6' />
      </g>
      <ellipse cx={50} cy={51} rx={31} ry={26} fill='#d49a86' {...line} />
      <ellipse cx={50} cy={61} rx={12} ry={7.5} fill='#f3d3c4' />
      <path d='M44 28 q2 4.5 0 8.5 M50 26.5 v9.5 M56 28 q-2 4.5 0 8.5' fill='none' stroke='#a86c5a' strokeWidth={2} strokeLinecap='round' />
      <g fill='none' stroke={ink} strokeWidth={1.5} strokeLinecap='round'>
        <path d='M23 55 l-11 -2.5 M23 60 l-11 2' />
        <path d='M77 55 l11 -2.5 M77 60 l11 2' />
      </g>
      <Bow x={27} y={20} scale={0.95} />
      <Face left={[39, 50]} right={[61, 50]} mouth={[50, 59.5]} mood={mood} />
      <g className='mc-paws'>
        <ellipse cx={31} cy={77} rx={9} ry={6} fill='#d49a86' {...line} />
        <ellipse cx={69} cy={77} rx={9} ry={6} fill='#d49a86' {...line} />
        <path d='M28 78 v-3 M33 78 v-3 M66 78 v-3 M71 78 v-3' stroke={ink} strokeWidth={1.4} strokeLinecap='round' />
      </g>
    </>
  );
}

/** Purin: the custard pudding in a maid cap. Always a little sleepy. */
function Purin({ mood }: { mood: Mood }) {
  return (
    <>
      <ellipse cx={50} cy={90} rx={40} ry={7} fill='#fff' {...line} />
      <ellipse cx={50} cy={90} rx={33} ry={4.4} fill='none' stroke='#ffb3cb' strokeWidth={1.6} strokeDasharray='2 3' />
      <g className='mc-wobble'>
        <path d='M24 88 L31 44 Q50 36 69 44 L76 88 Q50 95 24 88Z' fill='#ffe08a' {...line} />
        <path d='M31 44 Q50 36 69 44 L68 52 Q65 58 61 52 Q56 60 51 52 Q46 59 42 52 Q37 58 33 52 Z' fill='#c47a45' {...line} strokeWidth={2} />
        <path d='M35.5 61 Q33.5 72 34.5 82' fill='none' stroke='#fff6d6' strokeWidth={3} strokeLinecap='round' />
        <ellipse cx={50} cy={40.5} rx={14} ry={4} fill='#fff' {...line} strokeWidth={2} />
        <path d='M36 41 Q37.5 33 42 36.5 Q45 29.5 50 34 Q55 29.5 58 36.5 Q62.5 33 64 41' fill='#fff' {...line} strokeWidth={2} />
        <Bow x={50} y={33} scale={0.55} />
        <Face left={[41.5, 68]} right={[58.5, 68]} mouth={[50, 75.5]} mood={mood} />
      </g>
    </>
  );
}

/** Pip: the star chick. Excitable; turns up to celebrate every save. */
function Pip({ mood }: { mood: Mood }) {
  return (
    <>
      <path d='M51 12 q-3 -5 1 -8' fill='none' {...line} strokeWidth={2} />
      <Heart x={53.5} y={5} scale={0.62} />
      <path d='M50 12 L61.2 36.6 L88 39.6 L68.1 57.9 L73.5 84.4 L50 71 L26.5 84.4 L31.9 57.9 L12 39.6 L38.8 36.6 Z'
        fill='#ffd978' {...line} strokeWidth={2.8} />
      <path d='M36 44 Q39 38 44 37' fill='none' stroke='#fff6d6' strokeWidth={3} strokeLinecap='round' />
      <Face left={[43, 51]} right={[57, 51]} mouth={[50, 58.5]} mood={mood} />
    </>
  );
}

const drawings = { mochi: Mochi, azuki: Azuki, purin: Purin, pip: Pip };
const viewBoxes: Record<Character, string> = { mochi: '0 0 100 100', azuki: '0 0 100 84', purin: '0 0 100 100', pip: '0 0 100 90' };

export function Mascot({ character, mood = 'happy', className }: { character: Character; mood?: Mood; className?: string }) {
  const Drawing = drawings[character];
  return (
    <svg className={`mc-mascot mc-${character}${className ? ' ' + className : ''}`} viewBox={viewBoxes[character]}
      aria-hidden='true' focusable='false'>
      <Drawing mood={mood} />
    </svg>
  );
}
