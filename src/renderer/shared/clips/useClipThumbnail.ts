import { useEffect, useState } from "react";
import { cachedThumbnail, requestThumbnail } from "./thumbnailLoader";

export function useClipThumbnail(filePath: string, enabled = true): string {
  const [thumbnailUrl, setThumbnailUrl] = useState(() => (enabled ? cachedThumbnail(filePath) : ""));

  useEffect(() => {
    if (!enabled) {
      setThumbnailUrl("");
      return;
    }
    let active = true;
    setThumbnailUrl(cachedThumbnail(filePath));
    const request = requestThumbnail(filePath);
    void request.promise.then((url) => {
      if (active) setThumbnailUrl(url);
    });
    return () => {
      active = false;
      request.release();
    };
  }, [filePath, enabled]);

  return thumbnailUrl;
}
