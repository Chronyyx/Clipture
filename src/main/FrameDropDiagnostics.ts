import type {
  EngineDiagnostics,
  FrameDropActivityDeltas,
  FrameDropAnalysis,
  FrameDropReasonDeltas,
  FrameDropTimelineSample
} from "../shared/types";

type CounterSnapshot = {
  sampledAtMs: number;
  captureEpoch: number;
  reasons: FrameDropReasonDeltas;
  activity: FrameDropActivityDeltas;
};

const zeroReasons = (): FrameDropReasonDeltas => ({
  reportedDroppedFrames: 0,
  captureQueueOverflow: 0,
  captureSlotExhaustion: 0,
  schedulerDeadlineMisses: 0,
  encoderBackpressure: 0,
  encoderBackpressureOther: 0,
  captureCallbackErrors: 0,
  captureQueueCoalesced: 0,
  sourceFramesSuperseded: 0,
  captureSamplerRejections: 0,
  captureNonMonotonicTimestamps: 0,
  captureAcquireTimeouts: 0,
  captureAccessLosses: 0,
  captureFallbacks: 0,
  schedulerRepeats: 0,
  encoderQueueDrops: 0,
  encoderRepeatsCoalesced: 0,
  nvencSurfaceDrops: 0,
  nvencInputDrops: 0
});

const zeroActivity = (): FrameDropActivityDeltas => ({
  captureUpdatesAcquired: 0,
  desktopPresents: 0,
  pointerUpdates: 0,
  freshFramesPublished: 0,
  accumulatedSourceFrames: 0,
  accumulationEvents: 0,
  captureAcquireImmediateMisses: 0,
  captureAcquireGraceHits: 0,
  captureAcquireGraceTimeouts: 0,
  captureClockTickRequests: 0,
  captureClockTickWakeups: 0,
  captureClockTickCoalesced: 0,
  captureClockTickCompletions: 0,
  captureClockTickCompletionWaits: 0,
  captureClockTickCompletionTimeouts: 0,
  encoderFramesAccepted: 0,
  encoderPacketsProduced: 0,
  encoderDistinctSourceFrames: 0,
  encoderRepeatedSourceFrames: 0,
  encoderUnknownSourceFrames: 0,
  replayPacketsPersisted: 0
});

function nonNegativeCounter(value: number | undefined): number {
  return Number.isFinite(value) ? Math.max(0, Math.trunc(value ?? 0)) : 0;
}

function snapshot(diagnostics: EngineDiagnostics, sampledAtMs: number): CounterSnapshot {
  const encoderBackpressure = nonNegativeCounter(diagnostics.encoderBackpressureDrops);
  const encoderQueueDrops = nonNegativeCounter(diagnostics.encoderQueueDrops);
  const nvencSurfaceDrops = nonNegativeCounter(diagnostics.nvencSurfaceDrops);
  const nvencInputDrops = nonNegativeCounter(diagnostics.nvencInputDrops);
  return {
    sampledAtMs,
    captureEpoch: nonNegativeCounter(diagnostics.captureEpoch),
    reasons: {
      reportedDroppedFrames: nonNegativeCounter(diagnostics.droppedFrames),
      captureQueueOverflow: nonNegativeCounter(diagnostics.captureOverflowDrops),
      captureSlotExhaustion: nonNegativeCounter(diagnostics.captureSlotDrops),
      schedulerDeadlineMisses: nonNegativeCounter(diagnostics.schedulerDroppedFrames),
      encoderBackpressure,
      encoderBackpressureOther: 0,
      captureCallbackErrors: nonNegativeCounter(diagnostics.captureCallbackErrors),
      captureQueueCoalesced: nonNegativeCounter(diagnostics.captureCoalescedDrops),
      sourceFramesSuperseded: nonNegativeCounter(diagnostics.sourceFramesSuperseded),
      captureSamplerRejections: nonNegativeCounter(diagnostics.captureSamplerRejections),
      captureNonMonotonicTimestamps: nonNegativeCounter(diagnostics.captureNonMonotonicTimestamps),
      captureAcquireTimeouts: nonNegativeCounter(diagnostics.captureAcquireTimeouts),
      captureAccessLosses: nonNegativeCounter(diagnostics.captureAccessLosses),
      captureFallbacks: nonNegativeCounter(diagnostics.captureFallbacks),
      schedulerRepeats: nonNegativeCounter(diagnostics.schedulerRepeatedFrames),
      encoderQueueDrops,
      encoderRepeatsCoalesced: nonNegativeCounter(diagnostics.encoderRepeatCoalesced),
      nvencSurfaceDrops,
      nvencInputDrops
    },
    activity: {
      captureUpdatesAcquired: nonNegativeCounter(diagnostics.captureAcquiredUpdates),
      desktopPresents: nonNegativeCounter(diagnostics.captureDesktopPresents),
      pointerUpdates: nonNegativeCounter(diagnostics.capturePointerUpdates),
      freshFramesPublished: nonNegativeCounter(diagnostics.capturePublishedFrames),
      accumulatedSourceFrames: nonNegativeCounter(diagnostics.captureAccumulatedFrames),
      accumulationEvents: nonNegativeCounter(diagnostics.captureAccumulationEvents),
      captureAcquireImmediateMisses: nonNegativeCounter(diagnostics.captureAcquireImmediateMisses),
      captureAcquireGraceHits: nonNegativeCounter(diagnostics.captureAcquireGraceHits),
      captureAcquireGraceTimeouts: nonNegativeCounter(diagnostics.captureAcquireGraceTimeouts),
      captureClockTickRequests: nonNegativeCounter(diagnostics.captureClockTickRequests),
      captureClockTickWakeups: nonNegativeCounter(diagnostics.captureClockTickWakeups),
      captureClockTickCoalesced: nonNegativeCounter(diagnostics.captureClockTickCoalesced),
      captureClockTickCompletions: nonNegativeCounter(diagnostics.captureClockTickCompletions),
      captureClockTickCompletionWaits: nonNegativeCounter(diagnostics.captureClockTickCompletionWaits),
      captureClockTickCompletionTimeouts: nonNegativeCounter(diagnostics.captureClockTickCompletionTimeouts),
      encoderFramesAccepted: nonNegativeCounter(diagnostics.encoderAcceptedFrames),
      encoderPacketsProduced: nonNegativeCounter(diagnostics.encoderOutputPackets),
      encoderDistinctSourceFrames: nonNegativeCounter(diagnostics.encoderDistinctSourceFrames),
      encoderRepeatedSourceFrames: nonNegativeCounter(diagnostics.encoderRepeatedSourceFrames),
      encoderUnknownSourceFrames: nonNegativeCounter(diagnostics.encoderUnknownSourceFrames),
      replayPacketsPersisted: nonNegativeCounter(diagnostics.replayArchivePersistedPackets)
    }
  };
}

function numericRecord<T extends object>(value: T): Record<keyof T, number> {
  return value as unknown as Record<keyof T, number>;
}

function deltaRecord<T extends object>(current: T, previous: T): T {
  const currentValues = numericRecord(current);
  const previousValues = numericRecord(previous);
  return Object.fromEntries(
    (Object.keys(current) as Array<keyof T>).map((key) => [
      key,
      Math.max(0, currentValues[key] - previousValues[key])
    ])
  ) as T;
}

function hasCounterReset(current: CounterSnapshot, previous: CounterSnapshot): boolean {
  const decreased = <T extends object>(left: T, right: T) => {
    const leftValues = numericRecord(left);
    const rightValues = numericRecord(right);
    return (Object.keys(left) as Array<keyof T>).some((key) => leftValues[key] < rightValues[key]);
  };
  return decreased(current.reasons, previous.reasons)
    || decreased(current.activity, previous.activity);
}

function dominantCountedReason(reasons: FrameDropReasonDeltas): string {
  const candidates: Array<[string, number]> = [
    ["capture-queue-overflow", reasons.captureQueueOverflow],
    ["capture-slot-exhaustion", reasons.captureSlotExhaustion],
    ["scheduler-deadline-miss", reasons.schedulerDeadlineMisses],
    ["encoder-backpressure", reasons.encoderBackpressure]
  ];
  const dominant = candidates.reduce((best, candidate) => candidate[1] > best[1] ? candidate : best, candidates[0]);
  if (dominant[1] <= 0) return "none";
  if (dominant[0] !== "encoder-backpressure") return dominant[0];
  const encoderCandidates: Array<[string, number]> = [
    ["encoder-backpressure:queue-eviction", reasons.encoderQueueDrops],
    ["encoder-backpressure:nvenc-surface-starvation", reasons.nvencSurfaceDrops],
    ["encoder-backpressure:nvenc-input-busy", reasons.nvencInputDrops],
    ["encoder-backpressure:other", reasons.encoderBackpressureOther]
  ];
  return encoderCandidates.reduce(
    (best, candidate) => candidate[1] > best[1] ? candidate : best,
    encoderCandidates[0]
  )[0];
}

export function classifyVisualFreshness(
  activity: FrameDropActivityDeltas,
  diagnostics: EngineDiagnostics,
  windowMs: number
): {
  bottleneck: string;
  rates: FrameDropTimelineSample["freshnessRates"];
} {
  const perSecond = (count: number) => windowMs > 0 ? count * 1_000 / windowMs : 0;
  const rates: FrameDropTimelineSample["freshnessRates"] = {
    targetFps: Math.max(1, diagnostics.fps),
    captureClockRequests: perSecond(activity.captureClockTickRequests),
    captureClockWakeups: perSecond(activity.captureClockTickWakeups),
    captureClockCompletionTimeouts: perSecond(activity.captureClockTickCompletionTimeouts),
    captureAcquireImmediateMisses: perSecond(activity.captureAcquireImmediateMisses),
    captureAcquireGraceHits: perSecond(activity.captureAcquireGraceHits),
    captureAcquireGraceTimeouts: perSecond(activity.captureAcquireGraceTimeouts),
    desktopPresents: perSecond(activity.desktopPresents),
    desktopUpdateSupply: perSecond(
      activity.desktopPresents + activity.accumulatedSourceFrames
    ),
    captureUpdates: perSecond(activity.captureUpdatesAcquired),
    freshFramesPublished: perSecond(activity.freshFramesPublished),
    encoderFramesAccepted: perSecond(activity.encoderFramesAccepted),
    encoderPacketsProduced: perSecond(activity.encoderPacketsProduced),
    distinctSourceFramesEncoded: perSecond(activity.encoderDistinctSourceFrames)
  };
  if (windowMs <= 0) return { bottleneck: "collecting", rates };

  const lowRate = rates.targetFps * 0.92;
  if (
    diagnostics.captureClockMode === "encoder-driven-dxgi" ||
    diagnostics.captureClockMode === "encoder-prearmed-dxgi"
  ) {
    if (rates.captureClockRequests < lowRate) {
      return { bottleneck: "capture-clock-request-limited", rates };
    }
    if (rates.captureClockWakeups < Math.min(lowRate, rates.captureClockRequests * 0.92)) {
      return { bottleneck: "capture-clock-handoff-limited", rates };
    }
    if (rates.captureClockCompletionTimeouts >= Math.max(1, rates.targetFps * 0.08)) {
      return { bottleneck: "capture-clock-same-tick-timeout", rates };
    }
    if (rates.captureAcquireGraceTimeouts >= Math.max(1, rates.targetFps * 0.08)) {
      return { bottleneck: "dxgi-poll-grace-exhausted", rates };
    }
  }
  if (rates.desktopUpdateSupply < lowRate) {
    return { bottleneck: "desktop-source-present-limited", rates };
  }
  if (rates.freshFramesPublished < lowRate) {
    if (activity.accumulationEvents > 0 || activity.accumulatedSourceFrames > 0) {
      return { bottleneck: "capture-acquisition-backlog", rates };
    }
    if (rates.captureUpdates >= lowRate) {
      return { bottleneck: "capture-sampler-or-publication-limited", rates };
    }
    return { bottleneck: "capture-source-update-limited", rates };
  }
  if (rates.encoderFramesAccepted < Math.min(lowRate, rates.freshFramesPublished * 0.92)) {
    return { bottleneck: "scheduler-admission-limited", rates };
  }
  if (rates.encoderPacketsProduced < Math.min(lowRate, rates.encoderFramesAccepted * 0.92)) {
    return { bottleneck: "nvenc-output-limited", rates };
  }
  if (rates.distinctSourceFramesEncoded < lowRate) {
    if (diagnostics.stillFrameDuplicationEnabled && rates.encoderPacketsProduced >= lowRate) {
      return { bottleneck: "frame-duplication-masking-source-freshness", rates };
    }
    return { bottleneck: "encoded-source-freshness-limited", rates };
  }
  return { bottleneck: "healthy", rates };
}

function addReasons(target: FrameDropReasonDeltas, source: FrameDropReasonDeltas): void {
  for (const key of Object.keys(target) as Array<keyof FrameDropReasonDeltas>) {
    target[key] += source[key];
  }
}

export class FrameDropDiagnosticsRecorder {
  private baseline: CounterSnapshot | undefined;
  private timeline: FrameDropTimelineSample[] = [];
  private latestReasons = zeroReasons();
  private latestWindowMs = 0;
  private latestDominantReason = "none";
  private latestVisualFreshnessBottleneck = "collecting";

  constructor(
    private readonly minimumSampleIntervalMs = 1_000,
    private readonly maximumRetainedSamples = 900
  ) {}

  observe(diagnostics: EngineDiagnostics, sampledAtMs = Date.now()): EngineDiagnostics {
    const current = snapshot(diagnostics, sampledAtMs);
    const previous = this.baseline;
    if (!previous) {
      this.baseline = current;
      return this.enrich(diagnostics);
    }

    const windowMs = Math.max(0, sampledAtMs - previous.sampledAtMs);
    if (windowMs < this.minimumSampleIntervalMs) return this.enrich(diagnostics);

    const reset = hasCounterReset(current, previous);
    const reasons = reset ? zeroReasons() : deltaRecord(current.reasons, previous.reasons);
    reasons.encoderBackpressureOther = Math.max(
      0,
      reasons.encoderBackpressure
        - reasons.encoderQueueDrops
        - reasons.nvencSurfaceDrops
        - reasons.nvencInputDrops
    );
    const activity = reset ? zeroActivity() : deltaRecord(current.activity, previous.activity);
    const countedReasonTotal = reasons.captureQueueOverflow
      + reasons.captureSlotExhaustion
      + reasons.schedulerDeadlineMisses
      + reasons.encoderBackpressure;
    const dominantReason = dominantCountedReason(reasons);
    const freshness = classifyVisualFreshness(activity, diagnostics, windowMs);
    const captureEpochChanged = current.captureEpoch !== previous.captureEpoch;
    const visualFreshnessBottleneck = reset
      ? "counter-reset"
      : captureEpochChanged
        ? "capture-epoch-transition"
        : freshness.bottleneck;
    const sample: FrameDropTimelineSample = {
      sampledAt: new Date(sampledAtMs).toISOString(),
      windowMs,
      reset,
      captureEpochChanged,
      dominantCountedReason: dominantReason,
      visualFreshnessBottleneck,
      countedReasonTotal,
      accountingDifference: reasons.reportedDroppedFrames - countedReasonTotal,
      reportedDropsPerSecond: windowMs > 0 ? reasons.reportedDroppedFrames * 1_000 / windowMs : 0,
      freshnessRates: freshness.rates,
      reasons,
      activity,
      context: {
        captureEpoch: diagnostics.captureEpoch,
        capturePressure: diagnostics.capturePressure,
        activeCaptureBackend: diagnostics.activeCaptureBackend,
        captureClockMode: diagnostics.captureClockMode,
        desktopPresentFps: diagnostics.desktopPresentFps,
        publishedFreshFps: diagnostics.publishedFreshFps,
        recentCaptureFps: diagnostics.recentPublishedFreshFps,
        recentEncoderInputFps: diagnostics.recentEncoderInputFps,
        recentEncoderOutputFps: diagnostics.recentEncoderOutputFps,
        recentEncoderDistinctSourceFps: diagnostics.recentEncoderDistinctSourceFps,
        stillFrameDuplicationEnabled: diagnostics.stillFrameDuplicationEnabled,
        captureSourceIntervalP95_100ns: diagnostics.recentCaptureSourceIntervalP95_100ns,
        publishedWallIntervalP95_100ns: diagnostics.recentPublishedWallIntervalP95_100ns,
        schedulerWakeLatenessP95_100ns: diagnostics.recentSchedulerWakeLatenessP95_100ns,
        encoderQueueResidenceP95_100ns: diagnostics.recentEncoderQueueResidenceP95_100ns,
        encoderInputIntervalP95_100ns: diagnostics.recentEncoderInputIntervalP95_100ns,
        encoderOutputIntervalP95_100ns: diagnostics.recentEncoderOutputIntervalP95_100ns,
        queuedFrames: diagnostics.queuedFrames,
        encoderQueuedFreshFrames: diagnostics.encoderQueuedFreshFrames,
        encoderQueuedRepeatFrames: diagnostics.encoderQueuedRepeatFrames,
        nvencInFlightFrames: diagnostics.nvencInFlightFrames,
        captureAcquireP95_100ns: diagnostics.recentCaptureAcquireP95_100ns,
        capturePreparationP95_100ns: diagnostics.recentCapturePreparationP95_100ns,
        captureProcessingP95_100ns: diagnostics.recentCaptureProcessingP95_100ns,
        inputPreparationP95_100ns: diagnostics.recentInputPreparationP95_100ns,
        inputMapP95_100ns: diagnostics.recentInputMapP95_100ns,
        nvencCallP95_100ns: diagnostics.recentNvencCallP95_100ns,
        outputEventWaitP95_100ns: diagnostics.recentOutputEventWaitP95_100ns,
        outputLockP95_100ns: diagnostics.recentOutputLockP95_100ns,
        outputCopyP95_100ns: diagnostics.recentOutputCopyP95_100ns,
        outputUnmapP95_100ns: diagnostics.recentOutputUnmapP95_100ns,
        maximumCaptureGap100ns: diagnostics.maximumCaptureGap100ns,
        maximumSubmitLatency100ns: diagnostics.maximumSubmitLatency100ns,
        replayArchiveQueuedBytes: diagnostics.replayArchiveQueuedBytes,
        replayArchiveQueuedPackets: diagnostics.replayArchiveQueuedPackets,
        replayArchiveWriteFailures: diagnostics.replayArchiveWriteFailures
      }
    };

    this.baseline = current;
    this.latestReasons = reasons;
    this.latestWindowMs = windowMs;
    this.latestDominantReason = dominantReason;
    this.latestVisualFreshnessBottleneck = visualFreshnessBottleneck;
    this.timeline.push(sample);
    if (this.timeline.length > this.maximumRetainedSamples) {
      this.timeline.splice(0, this.timeline.length - this.maximumRetainedSamples);
    }
    return this.enrich(diagnostics);
  }

  report(): FrameDropAnalysis {
    const totals = zeroReasons();
    for (const sample of this.timeline) addReasons(totals, sample.reasons);
    return {
      sampleIntervalMinimumMs: this.minimumSampleIntervalMs,
      maximumRetainedSamples: this.maximumRetainedSamples,
      retainedWindowMs: this.timeline.reduce((total, sample) => total + sample.windowMs, 0),
      definitions: {
        reportedDroppedFrames: "The dashboard headline: a sum of capture queue evictions, capture-slot failures, scheduler ticks skipped while late, and encoder-backpressure losses. The components are different pipeline units, so use the reason timeline rather than treating this as measured MP4 frame loss.",
        countedReasons: [
          "captureQueueOverflow: the bounded capture queue evicted its oldest captured frame",
          "captureSlotExhaustion: no owned capture texture slot was available",
          "schedulerDeadlineMisses: the CFR scheduler was too late to catch up every target tick",
          "encoderBackpressure: umbrella count for encoder queue eviction, NVENC output-surface starvation, NVENC input still in flight, or another encoder-side pressure loss"
        ],
        supportingIndicators: [
          "captureQueueCoalesced, sourceFramesSuperseded, and sampler rejections describe replaced or intentionally rejected source work and are not added to reportedDroppedFrames",
          "encoderQueueDrops, nvencSurfaceDrops, nvencInputDrops, and encoderBackpressureOther subdivide encoderBackpressure and are not added twice",
          "NVENC surface/input drops isolate failures after encoder admission",
          "schedulerRepeats describe intentional CFR duplication rather than loss",
          "acquire timeouts on a static desktop can be normal when DXGI has no new frame; correlate them with desktop presents and output FPS",
          "desktopUpdateSupply combines acquired desktop-present events with DXGI AccumulatedFrames so capture backlog is not mislabeled as a slow source",
          "captureClockTickRequests and captureClockTickWakeups identify scheduler-side request loss or coalescing before DXGI acquisition",
          "captureClockTickCompletions and completion timeouts identify capture-thread work that did not publish inside the scheduler tick budget",
          "schedulerWakeLateness measures how late the video clock woke after its target deadline, before capture, queueing, or NVENC work",
          "captureAcquireImmediateMisses, captureAcquireGraceHits, and captureAcquireGraceTimeouts show whether the bounded DXGI grace recovered a zero-timeout OBS-style poll miss",
          "visualFreshnessBottleneck names the first one-second pipeline boundary below 92% of target; it remains meaningful even when reportedDroppedFrames is zero",
          "distinctSourceFramesEncoded follows sourceFrameSequence, so repeated CFR samples cannot make source freshness look healthier than it is"
        ]
      },
      totals,
      latest: this.timeline.length > 0 ? this.timeline[this.timeline.length - 1] : null,
      timeline: this.timeline.map((sample) => ({
        ...sample,
        reasons: { ...sample.reasons },
        activity: { ...sample.activity },
        freshnessRates: { ...sample.freshnessRates },
        context: { ...sample.context }
      }))
    };
  }

  private enrich(diagnostics: EngineDiagnostics): EngineDiagnostics {
    return {
      ...diagnostics,
      recentDropWindowMs: this.latestWindowMs,
      recentDroppedFrames: this.latestReasons.reportedDroppedFrames,
      recentCaptureOverflowDrops: this.latestReasons.captureQueueOverflow,
      recentCaptureSlotDrops: this.latestReasons.captureSlotExhaustion,
      recentSchedulerDroppedFrames: this.latestReasons.schedulerDeadlineMisses,
      recentEncoderBackpressureDrops: this.latestReasons.encoderBackpressure,
      recentEncoderBackpressureOtherDrops: this.latestReasons.encoderBackpressureOther,
      recentCaptureCallbackErrors: this.latestReasons.captureCallbackErrors,
      recentCaptureCoalescedDrops: this.latestReasons.captureQueueCoalesced,
      recentSourceFramesSuperseded: this.latestReasons.sourceFramesSuperseded,
      recentCaptureSamplerRejections: this.latestReasons.captureSamplerRejections,
      recentCaptureNonMonotonicTimestamps: this.latestReasons.captureNonMonotonicTimestamps,
      recentCaptureAcquireTimeouts: this.latestReasons.captureAcquireTimeouts,
      recentCaptureAccessLosses: this.latestReasons.captureAccessLosses,
      recentCaptureFallbacks: this.latestReasons.captureFallbacks,
      recentSchedulerRepeatedFrames: this.latestReasons.schedulerRepeats,
      recentEncoderQueueDrops: this.latestReasons.encoderQueueDrops,
      recentEncoderRepeatCoalesced: this.latestReasons.encoderRepeatsCoalesced,
      recentNvencSurfaceDrops: this.latestReasons.nvencSurfaceDrops,
      recentNvencInputDrops: this.latestReasons.nvencInputDrops,
      recentDropDominantReason: this.latestDominantReason,
      recentVisualFreshnessBottleneck: this.latestVisualFreshnessBottleneck
    };
  }
}
