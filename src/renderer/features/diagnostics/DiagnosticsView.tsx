import { CheckSquare2, Search, Square } from "lucide-react";
import { useEffect, useState } from "react";
import type { EngineDiagnostics } from "../../../shared/types";
import { defaultDiagnostics } from "../../shared/diagnostics/defaultDiagnostics";

const diagnosticsPreferencesStorageKey = "clipture:diagnostics-preferences:v1";

type DiagnosticsPreferences = {
  selectedLabels: string[];
  selectedOnly: boolean;
};

function readDiagnosticsPreferences(): DiagnosticsPreferences {
  try {
    const parsed = JSON.parse(window.localStorage.getItem(diagnosticsPreferencesStorageKey) ?? "null") as Partial<DiagnosticsPreferences> | null;
    return {
      selectedLabels: Array.isArray(parsed?.selectedLabels)
        ? parsed.selectedLabels.filter((label): label is string => typeof label === "string")
        : [],
      selectedOnly: parsed?.selectedOnly === true
    };
  } catch {
    return { selectedLabels: [], selectedOnly: false };
  }
}

export function DiagnosticsView({ diagnostics }: { diagnostics: EngineDiagnostics }) {
  const [metricQuery, setMetricQuery] = useState("");
  const [preferences, setPreferences] = useState<DiagnosticsPreferences>(readDiagnosticsPreferences);
  const replayMiB = (bytes: number) => `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
  const recentDropSeconds = diagnostics.recentDropWindowMs > 0
    ? (diagnostics.recentDropWindowMs / 1000).toFixed(1)
    : "0.0";
  const lastClipCadence = diagnostics.lastClipCadence ?? defaultDiagnostics.lastClipCadence;
  const intervalMs = (value100ns: number) => `${((value100ns ?? 0) / 10_000).toFixed(2)} ms`;
  const entries: Array<[string, string]> = [
    ["Capture API", diagnostics.captureApi],
    ["Requested capture backend", diagnostics.requestedCaptureBackend],
    ["Active capture backend", diagnostics.activeCaptureBackend],
    ["Capture fallback", diagnostics.captureFallbackReason || "None"],
    ["Capture target", diagnostics.captureTargetKind + ": " + diagnostics.captureTargetName],
    ["Capture clock", diagnostics.captureClockMode],
    ["Capture clock requests / wakes / coalesced", String(diagnostics.captureClockTickRequests) + " / " + String(diagnostics.captureClockTickWakeups) + " / " + String(diagnostics.captureClockTickCoalesced)],
    ["Same-tick capture completions / waits / timeouts", String(diagnostics.captureClockTickCompletions) + " / " + String(diagnostics.captureClockTickCompletionWaits) + " / " + String(diagnostics.captureClockTickCompletionTimeouts)],
    ["Encode cadence", "Target-capped event-driven VFR"],
    ["Capture history maximum depth", String(diagnostics.frameQueueMaxDepth)],
    ["Display refresh", `${diagnostics.displayRefreshHz.toFixed(3)} Hz (${diagnostics.displayRefreshNumerator}/${diagnostics.displayRefreshDenominator})`],
    ["Desktop present rate", `${diagnostics.desktopPresentFps.toFixed(2)} FPS`],
    ["Published fresh rate", `${diagnostics.publishedFreshFps.toFixed(2)} FPS`],
    ["Recent capture / encoder in / encoder out", `${diagnostics.recentPublishedFreshFps.toFixed(2)} / ${diagnostics.recentEncoderInputFps.toFixed(2)} / ${diagnostics.recentEncoderOutputFps.toFixed(2)} FPS`],
    ["Visual freshness bottleneck", diagnostics.recentVisualFreshnessBottleneck ?? "collecting"],
    ["Duplicate-frame generation", diagnostics.stillFrameDuplicationEnabled ? "Enabled" : "Disabled"],
    ["Recent distinct source output", `${(diagnostics.recentEncoderDistinctSourceFps ?? 0).toFixed(2)} FPS`],
    ["Recent motion repeat ratio (rolling 3s)", `${(diagnostics.recentMotionRepeatRatioPercent ?? 0).toFixed(2)}%`],
    ["Recent motion frames repeated / window", `${diagnostics.recentMotionFramesRepeated ?? 0} / ${diagnostics.recentMotionFramesTotal ?? 0}`],
    ["Cumulative motion repeat ratio (lifetime)", `${(diagnostics.encodedMotionRepeatRatioPercent ?? 0).toFixed(2)}%`],
    ["Cumulative motion frames repeated / lifetime", `${diagnostics.motionFramesRepeated ?? 0} / ${diagnostics.motionFramesTotal ?? 0}`],
    ["Recent encoded source repeats / unknown", `${diagnostics.recentEncoderRepeatedSourceFrames ?? 0} / ${diagnostics.recentEncoderUnknownSourceFrames ?? 0}`],
    ["Encoded source totals distinct / repeat / unknown", `${diagnostics.encoderDistinctSourceFrames ?? 0} / ${diagnostics.encoderRepeatedSourceFrames ?? 0} / ${diagnostics.encoderUnknownSourceFrames ?? 0}`],
    ["Encoded repeat ratio", `${(diagnostics.encodedRepeatRatio * 100).toFixed(2)}%`],
    ["Capture updates acquired", String(diagnostics.captureAcquiredUpdates)],
    ["Desktop presents", String(diagnostics.captureDesktopPresents)],
    ["Pointer updates", String(diagnostics.capturePointerUpdates)],
    ["Fresh frames published", String(diagnostics.capturePublishedFrames)],
    ["Accumulated source frames", String(diagnostics.captureAccumulatedFrames)],
    ["Accumulation events", String(diagnostics.captureAccumulationEvents)],
    ["Sampler rejections", String(diagnostics.captureSamplerRejections)],
    ["Non-monotonic timestamps", String(diagnostics.captureNonMonotonicTimestamps)],
    ["Acquire timeouts", String(diagnostics.captureAcquireTimeouts)],
    ["DXGI immediate misses / grace hits / grace timeouts", `${diagnostics.captureAcquireImmediateMisses} / ${diagnostics.captureAcquireGraceHits} / ${diagnostics.captureAcquireGraceTimeouts}`],
    ["DXGI access losses", String(diagnostics.captureAccessLosses)],
    ["DXGI recreation attempts", String(diagnostics.captureRecreationAttempts)],
    ["DXGI recreation successes", String(diagnostics.captureRecreationSuccesses)],
    ["Capture fallbacks", String(diagnostics.captureFallbacks)],
    ["Encoder", diagnostics.activeEncoder],
    ["Encoder mode", diagnostics.encoderMode],
    ["GPU", diagnostics.gpu],
    ["Display", diagnostics.display],
    ["HDR tonemapping", diagnostics.hdrTonemapping ? "Enabled" : "Disabled"],
    ["Video source", diagnostics.videoSourceResolution],
    ["Output canvas", diagnostics.videoOutputResolution],
    ["Scaling", diagnostics.videoScaling],
    ["Clip target", diagnostics.clipTargetResolution],
    ["Microphone", diagnostics.microphoneDevice],
    ["Codec", diagnostics.codec],
    ["Resolution", diagnostics.resolution],
    ["FPS", String(diagnostics.fps)],
    ["Bitrate", `${diagnostics.bitrateMbps} Mbps`],
    ["Hardware acceleration", diagnostics.hardwareAcceleration ? "Enabled" : "Disabled"],
    ["Engine running", diagnostics.engineRunning ? "Yes" : "No"],
    ["D3D11 ready", diagnostics.d3d11Ready ? "Yes" : "No"],
    ["Capture ready", diagnostics.captureReady ? "Yes" : "No"],
    ["Audio ready", diagnostics.audioReady ? "Yes" : "No"],
    ["Mux ready", diagnostics.muxReady ? "Yes" : "No"],
    ["Buffer window", `${diagnostics.bufferDurationSeconds}s`],
    ["Captured frames", String(diagnostics.capturedFrames)],
    ["Queued frames", String(diagnostics.queuedFrames)],
    ["Encoder accepted", String(diagnostics.encoderAcceptedFrames)],
    ["Encoder packets", String(diagnostics.encoderOutputPackets)],
    ["Audio captured", String(diagnostics.audioCapturedPackets)],
    ["Video packets", String(diagnostics.bufferedVideoPackets)],
    ["Audio packets", String(diagnostics.bufferedAudioPackets)],
    ["Replay archive video / audio", `${diagnostics.videoReplayArchiveHealthy ? "Healthy" : "RAM fallback"} / ${diagnostics.audioReplayArchiveHealthy ? "Healthy" : "RAM fallback"}`],
    ["Replay archive disk", replayMiB(diagnostics.replayArchiveDiskBytes)],
    ["Replay RAM cache", `${replayMiB(diagnostics.replayArchiveResidentBytes)} / ${replayMiB(diagnostics.replayArchiveResidentBudgetBytes)}`],
    ["Replay read cache", replayMiB(diagnostics.replayArchiveReadCacheBytes)],
    ["Replay packets RAM / disk", `${diagnostics.replayArchiveResidentPackets} / ${diagnostics.replayArchiveDiskBackedPackets}`],
    ["Replay archive RAM fallback", replayMiB(diagnostics.replayArchiveRamFallbackBytes)],
    ["Replay archive queued", `${diagnostics.replayArchiveQueuedPackets} packets / ${replayMiB(diagnostics.replayArchiveQueuedBytes)}`],
    ["Replay archive segments", String(diagnostics.replayArchiveSegments)],
    ["Replay archive persisted", String(diagnostics.replayArchivePersistedPackets)],
    ["Replay spill inspections", String(diagnostics.replayArchiveSpillCandidateInspections)],
    ["PCM recovery", diagnostics.pcmRecoveryActive ? "Active" : "Standby"],
    ["Replay archive write failures", String(diagnostics.replayArchiveWriteFailures)],
    ["Replay archive max write", replayMiB(diagnostics.replayArchiveMaximumWriteBytes)],
    ["Recent reported drops", `${diagnostics.recentDroppedFrames} (${recentDropSeconds}s window)`],
    ["Recent dominant drop reason", diagnostics.recentDropDominantReason],
    ["Recent counted reasons", `capture queue ${diagnostics.recentCaptureOverflowDrops} / slots ${diagnostics.recentCaptureSlotDrops} / scheduler ${diagnostics.recentSchedulerDroppedFrames} / encoder ${diagnostics.recentEncoderBackpressureDrops}`],
    ["Recent capture indicators", `callbacks ${diagnostics.recentCaptureCallbackErrors} / sampler rejects ${diagnostics.recentCaptureSamplerRejections} / timeouts ${diagnostics.recentCaptureAcquireTimeouts} / non-monotonic ${diagnostics.recentCaptureNonMonotonicTimestamps}`],
    ["Recent source churn", `coalesced ${diagnostics.recentCaptureCoalescedDrops} / superseded ${diagnostics.recentSourceFramesSuperseded}`],
    ["Recent encoder backpressure detail", `queue ${diagnostics.recentEncoderQueueDrops} / surfaces ${diagnostics.recentNvencSurfaceDrops} / input ${diagnostics.recentNvencInputDrops} / other ${diagnostics.recentEncoderBackpressureOtherDrops}`],
    ["Recent capture recovery", `access losses ${diagnostics.recentCaptureAccessLosses} / fallbacks ${diagnostics.recentCaptureFallbacks}`],
    ["Reported dropped frames (total)", String(diagnostics.droppedFrames)],
    ["Capture queue overflow (counted)", String(diagnostics.captureOverflowDrops)],
    ["Source frames superseded", String(diagnostics.sourceFramesSuperseded)],
    ["Capture queue coalesced", String(diagnostics.captureCoalescedDrops)],
    ["Capture slot exhaustion (counted)", String(diagnostics.captureSlotDrops)],
    ["Capture callback errors", String(diagnostics.captureCallbackErrors)],
    ["Legacy scheduler deadline misses", String(diagnostics.schedulerDroppedFrames)],
    ["Legacy scheduler repeats", String(diagnostics.schedulerRepeatedFrames)],
    ["Encoder queue drops", String(diagnostics.encoderQueueDrops)],
    ["Legacy encoder repeats coalesced", String(diagnostics.encoderRepeatCoalesced)],
    ["Encoder admission rejections recent / total", String(diagnostics.recentEncoderAdmissionRejections) + " / " + String(diagnostics.encoderAdmissionRejections)],
    ["Encoder queued frames", String(diagnostics.encoderQueuedFreshFrames)],
    ["NVENC surface drops", String(diagnostics.nvencSurfaceDrops)],
    ["NVENC input drops", String(diagnostics.nvencInputDrops)],
    ["Encoder backpressure (counted)", String(diagnostics.encoderBackpressureDrops)],
    ["NVENC in flight", String(diagnostics.nvencInFlightFrames)],
    ["Maximum capture gap", `${(diagnostics.maximumCaptureGap100ns / 10_000).toFixed(1)} ms`],
    ["Maximum submit latency", `${(diagnostics.maximumSubmitLatency100ns / 10_000).toFixed(1)} ms`],
    ["Scale latency avg / max", `${(diagnostics.averageScaleLatency100ns / 10_000).toFixed(2)} / ${(diagnostics.maximumScaleLatency100ns / 10_000).toFixed(2)} ms`],
    ["Input map latency avg / max", `${(diagnostics.averageInputMapLatency100ns / 10_000).toFixed(2)} / ${(diagnostics.maximumInputMapLatency100ns / 10_000).toFixed(2)} ms`],
    ["NVENC call latency avg / max", `${(diagnostics.averageNvencCallLatency100ns / 10_000).toFixed(2)} / ${(diagnostics.maximumNvencCallLatency100ns / 10_000).toFixed(2)} ms`],
    ["Output drain latency avg / max", `${(diagnostics.averageOutputDrainLatency100ns / 10_000).toFixed(2)} / ${(diagnostics.maximumOutputDrainLatency100ns / 10_000).toFixed(2)} ms`],
    ["Capture acquire recent p95", `${(diagnostics.recentCaptureAcquireP95_100ns / 10_000).toFixed(2)} ms`],
    ["Source update interval recent p50 / p95 / max", `${intervalMs(diagnostics.recentCaptureSourceIntervalP50_100ns)} / ${intervalMs(diagnostics.recentCaptureSourceIntervalP95_100ns)} / ${intervalMs(diagnostics.recentCaptureSourceIntervalMaximum100ns)}`],
    ["Published PTS interval recent p50 / p95 / max", `${intervalMs(diagnostics.recentPublishedPtsIntervalP50_100ns)} / ${intervalMs(diagnostics.recentPublishedPtsIntervalP95_100ns)} / ${intervalMs(diagnostics.recentPublishedPtsIntervalMaximum100ns)}`],
    ["Published wall interval recent p50 / p95 / max", `${intervalMs(diagnostics.recentPublishedWallIntervalP50_100ns)} / ${intervalMs(diagnostics.recentPublishedWallIntervalP95_100ns)} / ${intervalMs(diagnostics.recentPublishedWallIntervalMaximum100ns)}`],

    ["Scheduler wake lateness recent p50 / p95 / max", `${intervalMs(diagnostics.recentSchedulerWakeLatenessP50_100ns)} / ${intervalMs(diagnostics.recentSchedulerWakeLatenessP95_100ns)} / ${intervalMs(diagnostics.recentSchedulerWakeLatenessMaximum100ns)}`],
    ["Encoder queue residence recent p50 / p95 / max", `${intervalMs(diagnostics.recentEncoderQueueResidenceP50_100ns)} / ${intervalMs(diagnostics.recentEncoderQueueResidenceP95_100ns)} / ${intervalMs(diagnostics.recentEncoderQueueResidenceMaximum100ns)}`],
    ["Encoder input interval recent p50 / p95 / max", `${intervalMs(diagnostics.recentEncoderInputIntervalP50_100ns)} / ${intervalMs(diagnostics.recentEncoderInputIntervalP95_100ns)} / ${intervalMs(diagnostics.recentEncoderInputIntervalMaximum100ns)}`],
    ["Encoder output interval recent p50 / p95 / max", `${intervalMs(diagnostics.recentEncoderOutputIntervalP50_100ns)} / ${intervalMs(diagnostics.recentEncoderOutputIntervalP95_100ns)} / ${intervalMs(diagnostics.recentEncoderOutputIntervalMaximum100ns)}`],
    ["Capture preparation recent p50 / p95", `${(diagnostics.recentCapturePreparationP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentCapturePreparationP95_100ns / 10_000).toFixed(2)} ms`],
    ["Cursor composite recent p95", `${(diagnostics.recentCaptureCursorP95_100ns / 10_000).toFixed(2)} ms`],
    ["Capture processing recent p50 / p95", `${(diagnostics.recentCaptureProcessingP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentCaptureProcessingP95_100ns / 10_000).toFixed(2)} ms`],
    ["Input preparation recent p50 / p95", `${(diagnostics.recentInputPreparationP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentInputPreparationP95_100ns / 10_000).toFixed(2)} ms`],
    ["Input map recent p50 / p95", `${(diagnostics.recentInputMapP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentInputMapP95_100ns / 10_000).toFixed(2)} ms`],
    ["NVENC call recent p50 / p95", `${(diagnostics.recentNvencCallP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentNvencCallP95_100ns / 10_000).toFixed(2)} ms`],
    ["Output event wait recent p50 / p95", `${(diagnostics.recentOutputEventWaitP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentOutputEventWaitP95_100ns / 10_000).toFixed(2)} ms`],
    ["Output lock / copy / unmap recent p95", `${(diagnostics.recentOutputLockP95_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentOutputCopyP95_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentOutputUnmapP95_100ns / 10_000).toFixed(2)} ms`],
    ["NVENC input paths zero-copy / copied / converted", `${diagnostics.nvencZeroCopyFrames} / ${diagnostics.nvencCopyFallbackFrames} / ${diagnostics.nvencConvertedFrames}`],
    ["Last clip distinct source rate", lastClipCadence.available ? `${lastClipCadence.distinctSourceFps.toFixed(2)} / ${lastClipCadence.targetFps} FPS` : "Save a clip to analyze"],
    ["Last clip desktop-present source rate", lastClipCadence.available ? `${lastClipCadence.desktopPresentSourceFps.toFixed(2)} / ${lastClipCadence.targetFps} FPS` : "Not available"],
    ["Last clip worst distinct / desktop-present second", lastClipCadence.available ? `${lastClipCadence.worstSecondDistinctSourceFps.toFixed(2)} / ${lastClipCadence.worstSecondDesktopPresentSourceFps.toFixed(2)} FPS` : "Not available"],
    ["Last clip low distinct / desktop-present seconds", lastClipCadence.available ? `${lastClipCadence.underTargetSeconds} / ${lastClipCadence.underTargetDesktopPresentSeconds}` : "Not available"],
    ["Last clip samples distinct / repeat / unknown", lastClipCadence.available ? `${lastClipCadence.distinctSourceFrames} / ${lastClipCadence.repeatedSourceFrames} / ${lastClipCadence.unknownSourceFrames}` : "Not available"],
    ["Last clip source updates desktop / pointer-only / unknown", lastClipCadence.available ? `${lastClipCadence.desktopPresentSourceFrames} / ${lastClipCadence.pointerOnlySourceFrames} / ${lastClipCadence.unknownUpdateKindSourceFrames}` : "Not available"],
    ["Last clip held run / unfilled target intervals", lastClipCadence.available ? `${lastClipCadence.longestHeldRunSamples} samples / ${lastClipCadence.missingFrameSlots}` : "Not available"],
    ["Last clip maximum sample gap", lastClipCadence.available ? intervalMs(lastClipCadence.maximumSampleGap100ns) : "Not available"],
    ["Capture epoch", String(diagnostics.captureEpoch)],
    ["Capture pressure", diagnostics.capturePressure]
  ];

  const selectedLabels = new Set(preferences.selectedLabels);
  const normalizedQuery = metricQuery.trim().toLocaleLowerCase();
  const visibleEntries = entries.filter(([label, value]) => {
    if (preferences.selectedOnly && !selectedLabels.has(label)) return false;
    return !normalizedQuery || `${label} ${value}`.toLocaleLowerCase().includes(normalizedQuery);
  });
  const selectedCount = entries.reduce(
    (count, [label]) => count + (selectedLabels.has(label) ? 1 : 0),
    0
  );

  useEffect(() => {
    try {
      window.localStorage.setItem(diagnosticsPreferencesStorageKey, JSON.stringify(preferences));
    } catch {
      // Preference persistence is optional when renderer storage is unavailable.
    }
  }, [preferences]);

  const toggleMetric = (label: string) => {
    setPreferences((current) => {
      const nextLabels = new Set(current.selectedLabels);
      if (nextLabels.has(label)) nextLabels.delete(label);
      else nextLabels.add(label);
      return { ...current, selectedLabels: [...nextLabels] };
    });
  };

  return (
    <section className="diagnostics-view">
      <div className="diagnostics-toolbar">
        <label className="diagnostics-search">
          <Search size={18} aria-hidden="true" />
          <input
            aria-label="Search diagnostics"
            placeholder="Search diagnostics or values"
            type="search"
            value={metricQuery}
            onChange={(event) => setMetricQuery(event.target.value)}
          />
        </label>
        <div className="diagnostics-toolbar-actions">
          <span className="diagnostics-count">{visibleEntries.length} of {entries.length} shown | {selectedCount} selected</span>
          {selectedCount > 0 && (
            <button
              className="diagnostics-clear"
              type="button"
              onClick={() => setPreferences((current) => ({ ...current, selectedLabels: [] }))}
            >
              Clear
            </button>
          )}
          <label className="diagnostics-selected-only">
            <span>Selected only</span>
            <input
              aria-label="Show selected diagnostics only"
              className="toggle-switch"
              type="checkbox"
              checked={preferences.selectedOnly}
              onChange={(event) => setPreferences((current) => ({ ...current, selectedOnly: event.target.checked }))}
            />
          </label>
        </div>
      </div>

      <div className="diagnostics panel">
        {visibleEntries.map(([label, value]) => {
          const selected = selectedLabels.has(label);
          return (
            <div className={selected ? "metric selected" : "metric"} key={label}>
              <button
                aria-label={selected ? `Remove ${label} from selected diagnostics` : `Select ${label}`}
                aria-pressed={selected}
                className="metric-selection"
                title={selected ? "Remove from selected diagnostics" : "Keep in selected diagnostics"}
                type="button"
                onClick={() => toggleMetric(label)}
              >
                {selected ? <CheckSquare2 size={18} /> : <Square size={18} />}
              </button>
              <span>{label}</span>
              <strong>{value}</strong>
            </div>
          );
        })}
        {visibleEntries.length === 0 && (
          <div className="diagnostics-empty">
            <strong>No diagnostics match this view</strong>
            <span>{preferences.selectedOnly && selectedCount === 0 ? "Select metrics from the All view first." : "Try a different search."}</span>
          </div>
        )}
      </div>
    </section>
  );
}
