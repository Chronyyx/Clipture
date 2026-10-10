/** Live per-track levels for audio chunks that hold each track apart: one
 * stereo pair per track, in order. Each pair goes through its own gain, and
 * the tracks are summed at 1/N each, as loud as the host's mixed chunks.
 * Chunks with two channels or fewer pass straight through. Each track also
 * feeds a meter, read after its level. */
export function createTrackLevels() {
  let nodes: GainNode[] = [];
  let meters: AnalyserNode[] = [];
  let gains: number[] | undefined;
  let samples = new Float32Array(0);
  const level = (track: number, pairs: number) => (gains?.[track] ?? 1) / pairs;

  function gainFor(context: BaseAudioContext, output: AudioNode, track: number, pairs: number) {
    let node = nodes[track];
    if (!node || node.context !== context) {
      node = context.createGain();
      node.connect(output);
      nodes[track] = node;
      const meter = context.createAnalyser();
      meter.fftSize = 1024;
      node.connect(meter);
      meters[track] = meter;
    }
    node.gain.value = level(track, pairs);
    return node;
  }

  return {
    /** Connects `source` to `output`; returns the nodes made for it, to
     * disconnect with the source. */
    route(context: BaseAudioContext, source: AudioBufferSourceNode, output: AudioNode): AudioNode[] {
      const channels = source.buffer?.numberOfChannels ?? 2;
      if (channels <= 2) {
        source.connect(output);
        return [];
      }
      const pairs = Math.floor(channels / 2);
      const splitter = context.createChannelSplitter(channels);
      source.connect(splitter);
      const made: AudioNode[] = [splitter];
      for (let track = 0; track < pairs; track += 1) {
        const merger = context.createChannelMerger(2);
        splitter.connect(merger, track * 2, 0);
        splitter.connect(merger, track * 2 + 1, 1);
        merger.connect(gainFor(context, output, track, pairs));
        made.push(merger);
      }
      return made;
    },
    /** New levels are heard at once, eased over a few milliseconds. */
    setGains(next: number[] | undefined) {
      gains = next;
      for (const [track, node] of nodes.entries()) {
        node?.gain.setTargetAtTime(level(track, nodes.length), node.context.currentTime, 0.015);
      }
    },
    /** Each track's peak right now, after its level, where 1 is full scale
     * (above it the saved track would clip). Empty until tracks play apart. */
    peaks(): number[] {
      return meters.map((meter) => {
        if (!meter) return 0;
        if (samples.length !== meter.fftSize) samples = new Float32Array(meter.fftSize);
        meter.getFloatTimeDomainData(samples);
        let peak = 0;
        for (const sample of samples) peak = Math.max(peak, Math.abs(sample));
        // Undo the 1/N the sum needs: this is the track as it will be saved.
        return peak * meters.length;
      });
    },
    /** Drops the gains (the audio context they belong to is gone). */
    reset() {
      for (const node of nodes) node?.disconnect();
      nodes = [];
      meters = [];
    }
  };
}
