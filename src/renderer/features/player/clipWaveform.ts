import { useEffect, useState } from 'react';

/** The clip's audio from the host in audiowaveform's JSON format (what the
 * trim editor's waveform views draw): `length` pairs of the lowest and
 * highest sample, -128 to 127, each `samples_per_pixel` samples long. */
export interface ClipWaveform {
  version: number;
  channels: number;
  sample_rate: number;
  samples_per_pixel: number;
  bits: number;
  length: number;
  data: number[];
}

const RETRIES = 4;

/** Fetches the waveform once `url` is set, retrying while the host is busy.
 * The host keeps it on disk, so a clip seen before answers at once. */
export function useClipWaveform(url: string | undefined) {
  const [waveform, setWaveform] = useState<ClipWaveform>();
  useEffect(() => {
    setWaveform(undefined);
    if (!url) return;
    const controller = new AbortController();
    void (async () => {
      for (let attempt = 0; attempt < RETRIES && !controller.signal.aborted; attempt += 1) {
        try {
          const response = await fetch(url, { signal: controller.signal });
          if (response.ok) {
            const body = await response.json() as ClipWaveform;
            if (valid(body)) setWaveform(body);
            return;
          }
        } catch (error) {
          if (controller.signal.aborted) return;
          console.warn('Waveform failed:', error);
        }
        await new Promise((resolve) => setTimeout(resolve, 400 * (attempt + 1)));
      }
    })();
    return () => controller.abort();
  }, [url]);
  return waveform;
}

function valid(body: ClipWaveform) {
  return body.version === 2 && body.channels === 1 && body.bits === 8 && body.length > 0
    && body.sample_rate > 0 && body.samples_per_pixel > 0
    && Array.isArray(body.data) && body.data.length === body.length * 2;
}
