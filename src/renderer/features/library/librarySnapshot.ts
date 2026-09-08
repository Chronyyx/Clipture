import type { ClipRecord } from '../../../shared/types';

// Merge events received during a snapshot without losing the existing library.
export class LibrarySnapshot {
  clips: ClipRecord[] = [];
  private request = 0;
  private pending = false;
  private additions = new Map<string, ClipRecord>();

  begin() {
    this.pending = true;
    return ++this.request;
  }

  upsert(clip: ClipRecord) {
    if (this.pending) this.additions.set(clip.id, clip);
    this.clips = [clip, ...this.clips.filter(candidate => candidate.id !== clip.id)];
    return this.clips;
  }

  complete(request: number, clips: ClipRecord[]) {
    if (request !== this.request) return undefined;
    this.clips = [...this.additions.values()].reverse().concat(
      clips.filter(clip => !this.additions.has(clip.id)));
    this.additions.clear();
    this.pending = false;
    return this.clips;
  }

  invalidate() {
    ++this.request;
    this.pending = false;
    this.additions.clear();
  }
}
