import { useEffect, useState } from "react";
import { cachedThumbnail, forgetThumbnail, requestThumbnail } from "./thumbnailLoader";

/** The clip's thumbnail URL ("" until known or when there is none), and a
 * handler for an image that failed to load, which asks for a fresh URL once. */
export function useClipThumbnail(filePath: string, enabled = true): { url: string; failed: () => void } {
  const [url, setUrl] = useState(() => (enabled ? cachedThumbnail(filePath) : ""));
  const [attempt, setAttempt] = useState(0);

  useEffect(() => {
    if (!enabled) {
      setUrl("");
      return;
    }
    let active = true;
    setUrl(cachedThumbnail(filePath));
    void requestThumbnail(filePath).then((next) => {
      if (active) setUrl(next);
    });
    return () => {
      active = false;
    };
  }, [filePath, enabled, attempt]);

  useEffect(() => setAttempt(0), [filePath]);

  const failed = () => {
    forgetThumbnail(filePath, url);
    setUrl("");
    if (attempt === 0) setAttempt(1);
  };

  return { url, failed };
}
