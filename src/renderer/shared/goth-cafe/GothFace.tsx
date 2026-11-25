import { useId } from 'react';

export type GothMood = 'smirk' | 'cheer' | 'sleepy' | 'shock' | 'wink' | 'bored';

export const liner = '#0b070d';
const lineEnd = { fill: 'none', stroke: liner, strokeLinecap: 'round', strokeLinejoin: 'round' } as const;

type Point = readonly [number, number];
export type Makeup = { iris: string; shadow: string; lips: string };

type EyeKind = 'open' | 'lidded' | 'wide' | 'closed';

/** A lined almond eye with a winged flick at the outer corner. `side` is -1
 * for the eye on the left of the face, so the wing points outward. */
function Eye({ at: [x, y], kind, side, makeup }: { at: Point; kind: EyeKind; side: -1 | 1; makeup: Makeup }) {
  const clip = useId();
  const outer = x + 5.4 * side;
  const wing = `M${outer} ${y - 0.4} q${2.6 * side} -0.6 ${4.6 * side} -3.6`;
  const lashes = `M${x + 3.4 * side} ${y - 2.6} l${1.2 * side} -2 M${x + 1.2 * side} ${y - 3.4} l${0.6 * side} -2.1`;
  const smoke = <ellipse cx={x + 0.6 * side} cy={y - 1.6} rx={8} ry={5.4} fill={makeup.shadow} opacity={0.6} />;

  if (kind === 'closed') {
    return (
      <g>
        {smoke}
        <path d={`M${x - 5.4} ${y} Q${x} ${y + 3.4} ${x + 5.4} ${y}`} {...lineEnd} strokeWidth={2.2} />
        <path d={wing} {...lineEnd} strokeWidth={1.8} />
        <path d={`M${x + 2.6 * side} ${y + 1.6} l${0.8 * side} 2 M${x} ${y + 2} v2.1 M${x - 2.6 * side} ${y + 1.6} l${-0.6 * side} 1.9`} {...lineEnd} strokeWidth={1.1} />
      </g>
    );
  }

  const top = kind === 'wide' ? 5.6 : kind === 'lidded' ? 2.4 : 4.4;
  const shape = `M${x - 5.4} ${y} Q${x} ${y - 2 * top} ${x + 5.4} ${y} Q${x} ${y + 4.2} ${x - 5.4} ${y}Z`;
  const irisY = kind === 'lidded' ? y + 0.9 : y + 0.2;
  return (
    <g>
      {smoke}
      <clipPath id={clip}><path d={shape} /></clipPath>
      <path d={shape} fill='#fbf7f9' />
      <g clipPath={`url(#${clip})`}>
        <circle cx={x} cy={irisY} r={kind === 'wide' ? 2.4 : 3.1} fill={makeup.iris} />
        <circle cx={x} cy={irisY} r={1.3} fill={liner} />
        <circle cx={x + 1.1} cy={irisY - 1.1} r={0.85} fill='#fff' />
      </g>
      <path d={`M${x - 5.6} ${y + 0.3} Q${x} ${y - 2 * top} ${x + 5.6} ${y + 0.1}`} {...lineEnd} strokeWidth={2.4} />
      <path d={`M${x - 4} ${y + 1.6} Q${x} ${y + 3.6} ${x + 4.6} ${y + 0.8}`} {...lineEnd} strokeWidth={0.9} opacity={0.7} />
      <path d={wing} {...lineEnd} strokeWidth={2} />
      <path d={lashes} {...lineEnd} strokeWidth={1.1} />
    </g>
  );
}

function Brow({ at: [x, y], side, raised }: { at: Point; side: -1 | 1; raised: boolean }) {
  const lift = raised ? 2.4 : 0;
  // A sharp arch: high toward the outer end, thin at the tail.
  return (
    <path d={`M${x - 4.6 * side} ${y - 7.4 - lift} Q${x + 1.6 * side} ${y - 10.8 - lift} ${x + 6 * side} ${y - 7.6 - lift * 0.6}`}
      {...lineEnd} strokeWidth={1.5} />
  );
}

function Lips({ at: [x, y], mood, color }: { at: Point; mood: GothMood; color: string }) {
  if (mood === 'shock') return <ellipse cx={x} cy={y + 0.6} rx={2.4} ry={3} fill={liner} stroke={color} strokeWidth={1.6} />;
  if (mood === 'cheer') {
    return (
      <g>
        <path d={`M${x - 5} ${y - 0.6} Q${x} ${y + 6.4} ${x + 5} ${y - 0.6} Q${x} ${y + 0.6} ${x - 5} ${y - 0.6}Z`} fill={liner} stroke={color} strokeWidth={1.4} strokeLinejoin='round' />
        {/* One tiny fang. */}
        <path d={`M${x + 1.6} ${y - 0.1} l0.9 2.3 l0.9 -2.3z`} fill='#fff' />
      </g>
    );
  }
  // Painted lips; a smirk lifts one corner.
  const lift = mood === 'smirk' || mood === 'wink' ? 1.3 : 0;
  const droop = mood === 'bored' ? 0.6 : 0;
  return (
    <g>
      <path d={`M${x - 4.4} ${y + droop} Q${x - 2.2} ${y - 1.9} ${x} ${y - 0.8} Q${x + 2.2} ${y - 1.9} ${x + 4.4} ${y - lift}
        Q${x + 1} ${y + 3.6} ${x - 4.4} ${y + droop}Z`} fill={color} />
      <path d={`M${x - 4.2} ${y + droop} Q${x} ${y + 0.6} ${x + 4.2} ${y - lift}`} fill='none' stroke={liner} strokeWidth={0.9} strokeLinecap='round' opacity={0.75} />
      <path d={`M${x - 1.6} ${y + 1.5} q1.4 0.5 2.6 0`} fill='none' stroke='#fff' strokeWidth={0.8} strokeLinecap='round' opacity={0.45} />
    </g>
  );
}

/** Brows, lined eyes, lipstick: shared by the whole cast so moods read alike. */
export function GothFace({ left, right, mouth, mood, makeup }: { left: Point; right: Point; mouth: Point; mood: GothMood; makeup: Makeup }) {
  const kind: EyeKind = mood === 'sleepy' ? 'closed' : mood === 'shock' ? 'wide' : mood === 'smirk' || mood === 'bored' || mood === 'wink' ? 'lidded' : 'open';
  const rightKind: EyeKind = mood === 'wink' ? 'closed' : kind;
  const raised = mood === 'shock';
  return (
    <g className={'gc-face gc-mood-' + mood}>
      <Brow at={left} side={-1} raised={raised} />
      <Brow at={right} side={1} raised={raised || mood === 'smirk'} />
      <g className={kind !== 'closed' && rightKind !== 'closed' ? 'gc-eyes gc-blinks' : 'gc-eyes'}>
        <Eye at={left} kind={kind} side={-1} makeup={makeup} />
        <Eye at={right} kind={rightKind} side={1} makeup={makeup} />
      </g>
      <Lips at={mouth} mood={mood} color={makeup.lips} />
    </g>
  );
}
