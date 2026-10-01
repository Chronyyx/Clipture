export type SpookyMood = 'happy' | 'cheer' | 'sleepy' | 'shy' | 'spooked' | 'wink';

export const ink = '#1d1133';
const tongue = '#ff6b8b';
const stroke = { fill: 'none', stroke: ink, strokeWidth: 1.7, strokeLinecap: 'round', strokeLinejoin: 'round' } as const;

type Point = readonly [number, number];

function Eye({ at: [x, y], kind, side, color }: { at: Point; kind: 'open' | 'closed' | 'arc' | 'squeeze' | 'wide'; side: 1 | -1; color: string }) {
  const line = { ...stroke, stroke: color };
  if (kind === 'closed') return <path d={`M${x - 4} ${y} q4 4 8 0`} {...line} />;
  if (kind === 'arc') return <path d={`M${x - 4} ${y + 1.5} q4 -5.5 8 0`} {...line} />;
  if (kind === 'squeeze') return <path d={`M${x - 3.2 * side} ${y - 3.2} L${x + 2.6 * side} ${y} L${x - 3.2 * side} ${y + 3.2}`} {...line} />;
  const r = kind === 'wide' ? 5 : 4;
  return (
    <g>
      <ellipse cx={x} cy={y} rx={r - 0.6} ry={r + 0.8} fill={color} />
      <circle cx={x + 1.3} cy={y - 1.8} r={1.5} fill='#fff' />
      <circle cx={x - 1.2} cy={y + 2} r={0.7} fill='#fff' opacity={0.8} />
    </g>
  );
}

function Mouth({ at: [x, y], mood, color }: { at: Point; mood: SpookyMood; color: string }) {
  const line = { ...stroke, stroke: color };
  switch (mood) {
    case 'happy':
      return <path d={`M${x - 5} ${y - 1} q2.5 3.4 5 0 q2.5 3.4 5 0`} {...line} />;
    case 'cheer':
    case 'wink':
      return (
        <g>
          <path d={`M${x - 5} ${y - 1.5} q5 9 10 0 z`} fill={color} stroke={color} strokeWidth={1.6} strokeLinejoin='round' />
          <ellipse cx={x} cy={y + 2.6} rx={2.2} ry={1.4} fill={tongue} />
        </g>
      );
    case 'sleepy':
      return <ellipse cx={x} cy={y} rx={1.7} ry={2.1} fill={color} />;
    case 'spooked':
      return <ellipse cx={x} cy={y + 1} rx={3} ry={4} fill={color} />;
    default:
      return <path d={`M${x - 5} ${y} q1.7 -2.2 3.4 0 q1.6 2.2 3.3 0 q1.7 -2.2 3.3 0`} {...line} />;
  }
}

/** Shared expressions for the drawn (non-carved) mascots. */
export function SpookyFace({ left, right, mouth, mood, color = ink, blush = '#ff7fa0' }: {
  left: Point; right: Point; mouth: Point; mood: SpookyMood; color?: string; blush?: string;
}) {
  const leftKind = mood === 'sleepy' ? 'closed' : mood === 'cheer' ? 'arc' : mood === 'shy' ? 'squeeze' : mood === 'spooked' ? 'wide' : 'open';
  const rightKind = mood === 'wink' ? 'arc' : leftKind;
  return (
    <g className={'hw-face hw-mood-' + mood}>
      <ellipse cx={left[0] - 5} cy={left[1] + 6.5} rx={4.2} ry={2.4} fill={blush} opacity={mood === 'shy' ? 0.6 : 0.28} />
      <ellipse cx={right[0] + 5} cy={right[1] + 6.5} rx={4.2} ry={2.4} fill={blush} opacity={mood === 'shy' ? 0.6 : 0.28} />
      <g className={leftKind === 'open' && rightKind === 'open' ? 'hw-eyes hw-blinks' : 'hw-eyes'}>
        <Eye at={left} kind={leftKind} side={1} color={color} />
        <Eye at={right} kind={rightKind} side={-1} color={color} />
      </g>
      <Mouth at={mouth} mood={mood} color={color} />
    </g>
  );
}
