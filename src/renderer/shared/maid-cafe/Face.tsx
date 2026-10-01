export type Mood = 'happy' | 'cheer' | 'sleepy' | 'shy' | 'worried' | 'wink';

const ink = '#5b2a4e';
const mouthFill = '#d6477c';
const stroke = { fill: 'none', stroke: ink, strokeWidth: 2.4, strokeLinecap: 'round', strokeLinejoin: 'round' } as const;

type Point = readonly [number, number];

function OpenEye({ at: [x, y] }: { at: Point }) {
  return (
    <g>
      <ellipse cx={x} cy={y} rx={3.6} ry={4.6} fill={ink} />
      <circle cx={x + 1.2} cy={y - 1.7} r={1.4} fill='#fff' />
      <circle cx={x - 1.3} cy={y + 1.9} r={0.7} fill='#fff' opacity={0.8} />
    </g>
  );
}

function Eye({ at, kind, side }: { at: Point; kind: 'open' | 'closed' | 'arc' | 'squeeze'; side: -1 | 1 }) {
  const [x, y] = at;
  if (kind === 'open') return <OpenEye at={at} />;
  if (kind === 'closed') return <path d={`M${x - 4} ${y} q4 4 8 0`} {...stroke} />;
  if (kind === 'arc') return <path d={`M${x - 4} ${y + 1.5} q4 -5.5 8 0`} {...stroke} />;
  // "> <": the point faces the nose.
  return <path d={`M${x - 3.2 * side} ${y - 3.2} L${x + 2.6 * side} ${y} L${x - 3.2 * side} ${y + 3.2}`} {...stroke} />;
}

function Mouth({ at: [x, y], mood }: { at: Point; mood: Mood }) {
  switch (mood) {
    case 'happy':
      return <path d={`M${x - 5} ${y - 1} q2.5 3.4 5 0 q2.5 3.4 5 0`} {...stroke} />;
    case 'cheer':
    case 'wink':
      return <path d={`M${x - 4.5} ${y - 1.5} q4.5 8 9 0 z`} fill={mouthFill} stroke={ink} strokeWidth={2} strokeLinejoin='round' />;
    case 'sleepy':
      return <ellipse cx={x} cy={y} rx={1.8} ry={2.2} fill={mouthFill} />;
    default:
      return <path d={`M${x - 5} ${y} q1.7 -2.2 3.4 0 q1.6 2.2 3.3 0 q1.7 -2.2 3.3 0`} {...stroke} />;
  }
}

/** Eyes, blush and mouth for every mascot, so moods read the same on each. */
export function Face({ left, right, mouth, mood }: { left: Point; right: Point; mouth: Point; mood: Mood }) {
  const leftKind = mood === 'sleepy' ? 'closed' : mood === 'cheer' ? 'arc' : mood === 'shy' ? 'squeeze' : 'open';
  const rightKind = mood === 'wink' ? 'arc' : leftKind;
  const blush = mood === 'shy' ? 0.9 : 0.55;
  return (
    <g className={'mc-face mc-mood-' + mood}>
      <ellipse cx={left[0] - 5} cy={left[1] + 6} rx={4.4} ry={2.6} fill='#ff8fb4' opacity={blush} />
      <ellipse cx={right[0] + 5} cy={right[1] + 6} rx={4.4} ry={2.6} fill='#ff8fb4' opacity={blush} />
      {mood === 'worried' && (
        <g {...stroke} strokeWidth={1.8}>
          <path d={`M${left[0] - 4} ${left[1] - 8} l7 -2.5`} />
          <path d={`M${right[0] + 4} ${right[1] - 8} l-7 -2.5`} />
        </g>
      )}
      <g className={leftKind === 'open' && rightKind === 'open' ? 'mc-eyes mc-blinks' : 'mc-eyes'}>
        <Eye at={left} kind={leftKind} side={1} />
        <Eye at={right} kind={rightKind} side={-1} />
      </g>
      <Mouth at={mouth} mood={mood} />
    </g>
  );
}
