/** The keyframe time closest to `seconds` (`times` ascending), or `seconds`
 * itself when the clip did not list its keyframes. A seek to a keyframe
 * decodes one frame; a seek between them decodes from the one before, about
 * three times slower on a 2-second keyframe interval. */
export function nearestKeyframe(times: readonly number[] | undefined, seconds: number): number {
  if (!times || times.length === 0) return seconds;
  let low = 0;
  let high = times.length - 1;
  while (low < high) {
    const middle = (low + high) >> 1;
    if (times[middle]! < seconds) low = middle + 1;
    else high = middle;
  }
  const after = times[low]!;
  const before = low > 0 ? times[low - 1]! : after;
  return seconds - before <= after - seconds ? before : after;
}
