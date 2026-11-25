import { useGothCafe } from './useGothCafe';

// Goth café's decoration is line work, not scenery: hairline vines and small
// four-point stars where the frames meet. All static SVG; nothing animates.

const star = 'M0 -6C0.5 -1.5 1.5 -0.5 6 0 1.5 0.5 0.5 1.5 0 6 -0.5 1.5 -1.5 0.5 -6 0 -1.5 -0.5 -0.5 -1.5 0 -6Z';

/** A four-point star; the sidebar edge and frame joins use it. */
function Star({ className, size = 14 }: { className: string; size?: number }) {
  return (
    <svg className={'gc-star ' + className} viewBox='-7 -7 14 14' width={size} height={size}>
      <path d={star} />
    </svg>
  );
}

// Outline leaves on a vine: [x, y, rotation, scale].
type Leaf = [number, number, number, number];
const leafPath = 'M0 0C-4 -2 -7 -6 -5 -10C-3 -9 -1 -11 0 -14C1 -11 3 -9 5 -10C7 -6 4 -2 0 0Z';

function Vine({ className, viewBox, path, leaves }: { className: string; viewBox: string; path: string; leaves: Leaf[] }) {
  return (
    <svg className={'gc-vine ' + className} viewBox={viewBox}>
      <path d={path} />
      {leaves.map(([x, y, rotate, scale], index) => (
        <g key={index} className={index % 2 ? 'gc-leaf alt' : 'gc-leaf'} transform={`translate(${x} ${y}) rotate(${rotate}) scale(${scale})`}>
          <path d={leafPath} />
          <path className='gc-leaf-vein' d='M0 -1V-11' />
        </g>
      ))}
    </svg>
  );
}

/** Climbs the sidebar's right edge from the bottom. */
const sideVine: Leaf[] = [[14, 186, -50, 1], [24, 160, 40, 0.9], [12, 136, -60, 0.85], [22, 108, 50, 0.8], [14, 84, -40, 0.75], [20, 58, 40, 0.65], [16, 34, -30, 0.55]];
/** Curls in from the window's top-right corner. */
const cornerVine: Leaf[] = [[66, 10, 120, 0.8], [52, 22, -150, 0.75], [44, 40, 140, 0.7], [30, 50, -120, 0.65], [72, 34, 90, 0.6]];

export function GothAmbience() {
  if (!useGothCafe()) return null;
  return (
    <div className='gc-ornaments' aria-hidden='true'>
      <Star className='gc-star-edge-1' />
      <Star className='gc-star-edge-2' size={20} />
      <Star className='gc-star-edge-3' size={18} />
      <Vine className='gc-vine-side' viewBox='0 0 36 200' leaves={sideVine}
        path='M18 200C10 180 26 166 18 146S12 112 20 92 12 58 18 30' />
      <Vine className='gc-vine-corner' viewBox='0 0 90 70' leaves={cornerVine}
        path='M90 0C76 4 64 14 54 26S36 46 22 54' />
    </div>
  );
}
