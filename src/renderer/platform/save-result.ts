import type { SaveClipResult } from '../../shared/types';

/** Keep additive engine recovery metadata intact through either host adapter. */
export function preserveSaveResult(result: SaveClipResult): SaveClipResult {
  const clip = result.clip;
  if (!clip?.segmentAudioTracks) return result;
  if (!clip.segmentFiles || clip.segmentAudioTracks.length !== clip.segmentFiles.length ||
      clip.segmentAudioTracks.some(tracks => !Array.isArray(tracks) ||
        tracks.some(track => typeof track !== 'string'))) {
    throw new Error('The host returned inconsistent segment audio metadata.');
  }
  return { ...result, clip: { ...clip, segmentAudioTracks: clip.segmentAudioTracks.map(tracks => [...tracks]) } };
}
