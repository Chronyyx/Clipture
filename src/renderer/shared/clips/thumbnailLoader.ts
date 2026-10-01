import { clipture, isHostBusyError } from "../../platform";

// The host admits only a few background commands at once and extracts
// thumbnails one at a time, rejecting overflow as "busy". Every library card
// asking at once used to lose most of those races and stay blank. Requests
// are cached, shared per clip, kept under the host limit and retried when busy.
const maximumInFlight = 2;
const maximumCached = 256;
const busyRetryDelaysMs = [250, 500, 1000, 2000, 4000];

interface Pending {
  filePath: string;
  subscribers: number;
  promise: Promise<string>;
  resolve: (url: string) => void;
}

const cache = new Map<string, string>();
const pending = new Map<string, Pending>();
const queue: Pending[] = [];
let inFlight = 0;

function remember(filePath: string, url: string) {
  cache.delete(filePath);
  cache.set(filePath, url);
  while (cache.size > maximumCached) cache.delete(cache.keys().next().value as string);
}

async function fetchWithRetry(filePath: string): Promise<string> {
  for (let attempt = 0; ; attempt += 1) {
    try {
      return (await clipture.clipThumbnailUrl(filePath)) || "";
    } catch (error) {
      if (!isHostBusyError(error) || attempt >= busyRetryDelaysMs.length) return "";
      await new Promise((resolve) => setTimeout(resolve, busyRetryDelaysMs[attempt]));
    }
  }
}

function pump() {
  while (inFlight < maximumInFlight && queue.length > 0) {
    const next = queue.shift()!;
    // Cards scrolled away before their turn cost nothing.
    if (next.subscribers === 0) {
      pending.delete(next.filePath);
      next.resolve("");
      continue;
    }
    inFlight += 1;
    void fetchWithRetry(next.filePath).then((url) => {
      if (url) remember(next.filePath, url);
      next.resolve(url);
    }).finally(() => {
      pending.delete(next.filePath);
      inFlight -= 1;
      pump();
    });
  }
}

export function cachedThumbnail(filePath: string): string {
  return cache.get(filePath) ?? "";
}

/** Returns the thumbnail URL ("" on failure) and a release for unmounting callers. */
export function requestThumbnail(filePath: string): { promise: Promise<string>; release: () => void } {
  const cached = cache.get(filePath);
  if (cached) return { promise: Promise.resolve(cached), release: () => {} };
  let entry = pending.get(filePath);
  if (!entry) {
    let resolve!: (url: string) => void;
    const promise = new Promise<string>((done) => { resolve = done; });
    entry = { filePath, subscribers: 0, promise, resolve };
    pending.set(filePath, entry);
    queue.push(entry);
  }
  const subscribed = entry;
  subscribed.subscribers += 1;
  pump();
  let released = false;
  return {
    promise: subscribed.promise,
    release: () => {
      if (released) return;
      released = true;
      subscribed.subscribers -= 1;
    }
  };
}
