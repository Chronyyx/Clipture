import { useCallback, useEffect, useRef, useState } from 'react';
import type { ClipRecord } from '../../../shared/types';
import { clipture } from '../../platform';
import { LibrarySnapshot } from './librarySnapshot';

export function useClipLibrary(onNotice: (message: string, durationMs?: number) => void) {
  const [clips, setClips] = useState<ClipRecord[]>([]);
  const snapshot = useRef(new LibrarySnapshot());
  const refreshLibrary = useCallback(async () => {
    const request = snapshot.current.begin();
    const next = snapshot.current.complete(request, await clipture.listClips());
    if (next) setClips(next);
  }, []);
  const addClip = useCallback((clip: ClipRecord) => {
    setClips(snapshot.current.upsert(clip));
  }, []);

  useEffect(() => {
    const refresh = () => void refreshLibrary().catch(error =>
      onNotice(error instanceof Error ? error.message : 'Could not refresh library.', 6000));
    const unsubscribe = clipture.onLibraryChanged(clip => clip ? addClip(clip) : refresh());
    refresh();
    return () => { snapshot.current.invalidate(); unsubscribe(); };
  }, [addClip, onNotice, refreshLibrary]);

  async function importVideos() {
    try {
      const imported = await clipture.importVideoFolders();
      if (!imported) return false;
      await refreshLibrary();
      onNotice('Imported video folder added');
      return true;
    } catch (error) {
      onNotice(error instanceof Error ? error.message : 'Could not import video folders.', 6000);
      return false;
    }
  }
  return { clips, addClip, refreshLibrary, importVideos };
}
