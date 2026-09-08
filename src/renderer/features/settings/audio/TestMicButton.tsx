import { useEffect, useRef, useState } from "react";
// @ts-ignore
import { RNNoiseNode } from "simple-rnnoise-wasm";
// @ts-ignore
import workletUrl from "simple-rnnoise-wasm/rnnoise.worklet.js?url";
// @ts-ignore
import wasmUrl from "simple-rnnoise-wasm/rnnoise.wasm?url";
import { visualizerLevelFromRms } from "./audioMath";

type GateMeterSample = {
  level: number;
  open: boolean;
};

const emptyGateMeterSamples = Array.from({ length: 36 }, () => ({ level: 0, open: false }));

export function TestMicButton({
  volume,
  voiceIsolation,
  voiceIsolationWeight,
  noiseGateEnabled,
  autoNoiseGate,
  noiseGateThreshold,
  noiseGateDebounceMs
}: {
  volume: number;
  voiceIsolation: boolean;
  voiceIsolationWeight: number;
  noiseGateEnabled: boolean;
  autoNoiseGate: boolean;
  noiseGateThreshold: number;
  noiseGateDebounceMs: number;
}) {
  const [testing, setTesting] = useState(false);
  const [meterSamples, setMeterSamples] = useState<GateMeterSample[]>(emptyGateMeterSamples);
  const audioContextRef = useRef<AudioContext | null>(null);
  const streamRef = useRef<MediaStream | null>(null);
  const wetGainRef = useRef<GainNode | null>(null);
  const dryGainRef = useRef<GainNode | null>(null);
  const masterGainRef = useRef<GainNode | null>(null);
  const gateGainRef = useRef<GainNode | null>(null);
  const gateConfigRef = useRef({ noiseGateEnabled, autoNoiseGate, noiseGateThreshold, noiseGateDebounceMs });

  useEffect(() => {
    gateConfigRef.current = { noiseGateEnabled, autoNoiseGate, noiseGateThreshold, noiseGateDebounceMs };
  }, [noiseGateEnabled, autoNoiseGate, noiseGateThreshold, noiseGateDebounceMs]);

  useEffect(() => {
    if (!testing) {
      setMeterSamples(emptyGateMeterSamples);
      if (streamRef.current) {
        streamRef.current.getTracks().forEach(t => t.stop());
        streamRef.current = null;
      }
      if (audioContextRef.current) {
        audioContextRef.current.close().catch(console.error);
        audioContextRef.current = null;
      }
      return;
    }

    let isCancelled = false;

    navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false } })
      .then(async (stream) => {
        if (isCancelled) {
          stream.getTracks().forEach(t => t.stop());
          return;
        }

        streamRef.current = stream;
        const ctx = new AudioContext();
        audioContextRef.current = ctx;

        const source = ctx.createMediaStreamSource(stream);
        masterGainRef.current = ctx.createGain();
        masterGainRef.current.connect(ctx.destination);
        masterGainRef.current.gain.value = volume;
        
        gateGainRef.current = ctx.createGain();
        gateGainRef.current.connect(masterGainRef.current);
        
        const analyser = ctx.createAnalyser();
        analyser.fftSize = 512;
        source.connect(analyser);
        
        const pcmData = new Float32Array(analyser.fftSize);
        let lastGateOpen = false;
        let lastVoiceMs = performance.now();
        let lastMeterUpdateMs = 0;
        
        const checkGate = () => {
          if (isCancelled) return;
          requestAnimationFrame(checkGate);
          
          analyser.getFloatTimeDomainData(pcmData);
          let sum = 0;
          for (let i = 0; i < pcmData.length; i++) sum += pcmData[i] * pcmData[i];
          const rms = Math.sqrt(sum / pcmData.length);
          
          const { noiseGateEnabled, autoNoiseGate, noiseGateThreshold, noiseGateDebounceMs } = gateConfigRef.current;
          const threshold = autoNoiseGate ? 0.01 : noiseGateThreshold;
          const now = performance.now();
          const crossedThreshold = rms > threshold;
          if (crossedThreshold) lastVoiceMs = now;
          
          const gateOpen = !noiseGateEnabled || crossedThreshold || now - lastVoiceMs <= noiseGateDebounceMs;
          if (gateOpen !== lastGateOpen && gateGainRef.current) {
            lastGateOpen = gateOpen;
            gateGainRef.current.gain.setTargetAtTime(gateOpen ? 1.0 : 0.0, ctx.currentTime, gateOpen ? 0.015 : 0.05);
          }

          if (now - lastMeterUpdateMs > 45) {
            lastMeterUpdateMs = now;
            const level = visualizerLevelFromRms(rms);
            setMeterSamples((samples) => [...samples.slice(1), { level, open: gateOpen }]);
          }
        };
        requestAnimationFrame(checkGate);

        if (voiceIsolation) {
          try {
            const wasmPromise = fetch(wasmUrl).then(r => r.arrayBuffer()).then(buf => WebAssembly.compile(buf));
            await RNNoiseNode.register(ctx, [workletUrl, wasmPromise]);
            if (isCancelled) return;
            
            const rnnoise = new RNNoiseNode(ctx);
            
            // RNNoise has its own VAD logic we could theoretically hook into, but RMS is fine for UI testing
            rnnoise.onstatus = (e: any) => {
              if (gateConfigRef.current.noiseGateEnabled && gateConfigRef.current.autoNoiseGate) {
                const vadProb = (e as any).data?.vad ?? (typeof (e as any).data === 'number' ? (e as any).data : 0.0);
                const now = performance.now();
                if (vadProb > 0.5) lastVoiceMs = now;
                const gateOpen = vadProb > 0.5 || now - lastVoiceMs <= gateConfigRef.current.noiseGateDebounceMs;
                if (gateOpen !== lastGateOpen && gateGainRef.current) {
                  lastGateOpen = gateOpen;
                  gateGainRef.current.gain.setTargetAtTime(gateOpen ? 1.0 : 0.0, ctx.currentTime, gateOpen ? 0.015 : 0.05);
                }
              }
            };
            
            wetGainRef.current = ctx.createGain();
            dryGainRef.current = ctx.createGain();
            
            source.connect(rnnoise);
            rnnoise.connect(wetGainRef.current);
            source.connect(dryGainRef.current);
            
            wetGainRef.current.connect(gateGainRef.current);
            dryGainRef.current.connect(gateGainRef.current);
            
            wetGainRef.current.gain.value = voiceIsolationWeight;
            dryGainRef.current.gain.value = 1.0 - voiceIsolationWeight;
          } catch (e) {
            console.error("Failed to load RNNoise WASM:", e);
            source.connect(gateGainRef.current);
          }
        } else {
          source.connect(gateGainRef.current);
        }
      })
      .catch(console.error);

    return () => {
      isCancelled = true;
      if (streamRef.current) {
        streamRef.current.getTracks().forEach(t => t.stop());
        streamRef.current = null;
      }
      if (audioContextRef.current) {
        audioContextRef.current.close().catch(console.error);
        audioContextRef.current = null;
      }
    };
  }, [testing, voiceIsolation]);

  useEffect(() => {
    if (testing) {
      if (masterGainRef.current) masterGainRef.current.gain.value = volume;
      if (wetGainRef.current) wetGainRef.current.gain.value = voiceIsolationWeight;
      if (dryGainRef.current) dryGainRef.current.gain.value = 1.0 - voiceIsolationWeight;
    }
  }, [volume, voiceIsolationWeight, testing]);

  const thresholdPercent = noiseGateEnabled
    ? visualizerLevelFromRms(autoNoiseGate ? 0.01 : noiseGateThreshold) * 100
    : 0;

  return (
    <div className="mic-test">
      <div className="mic-visualizer" title="Mic gate preview">
        <div className="mic-threshold-line" style={{ bottom: `${thresholdPercent}%` }} />
        {meterSamples.map((sample, index) => (
          <span
            className={sample.open ? "mic-meter-bar open" : "mic-meter-bar cut"}
            key={index}
            style={{ height: `${Math.max(4, sample.level * 100)}%` }}
          />
        ))}
      </div>
      <button 
        className={testing ? "secondary-button active" : "secondary-button"} 
        onClick={() => setTesting(!testing)}
      >
        {testing ? "Stop Testing" : "Test Mic"}
      </button>
    </div>
  );
}
