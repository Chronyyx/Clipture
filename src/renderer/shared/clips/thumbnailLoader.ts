import { clipture } from "../../platform";

// A clip's thumbnail URL. The host answers at once with a URL whose image it
// makes on first load and keeps on disk, so this only shares one request per
// clip and remembers the answer; the <img> element does the loading.
const maximumCached = 512;

const cache = new Map<string, string>();
const pending = new Map<string, Promise<string>>();

function remember(filePath: string, url: string) {
  cache.delete(filePath);
  cache.set(filePath, url);
  while (cache.size > maximumCached) cache.delete(cache.keys().next().value as string);
}

export function cachedThumbnail(filePath: string): string {
  return cache.get(filePath) ?? "";
}

/** The thumbnail URL, or "" when the clip has none. */
export function requestThumbnail(filePath: string): Promise<string> {
  const cached = cache.get(filePath);
  if (cached) return Promise.resolve(cached);
  let request = pending.get(filePath);
  if (!request) {
    request = clipture.clipThumbnailUrl(filePath)
      .then((url) => {
        if (url) remember(filePath, url);
        return url || "";
      })
      .catch(() => "")
      .finally(() => pending.delete(filePath));
    pending.set(filePath, request);
  }
  return request;
}

/** Drops a URL that failed to load (the clip changed); the next request asks again. */
export function forgetThumbnail(filePath: string, url: string) {
  if (cache.get(filePath) === url) cache.delete(filePath);
}
