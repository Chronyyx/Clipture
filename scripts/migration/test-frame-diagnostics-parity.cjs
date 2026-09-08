// Read-only Electron/Rust diagnostics oracle. --emit prints synthetic fixtures;
// it never writes files, starts either host, or accesses user data.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');
const root = path.resolve(__dirname, '../..');
const source = fs.readFileSync(path.join(root, 'src/main/frameDropDiagnostics.ts'), 'utf8');
const exportsObject = {};
vm.runInNewContext(ts.transpileModule(source, { compilerOptions: {
  module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022
}}).outputText + '\nexports.zeroActivity = zeroActivity;', { exports: exportsObject });
const { FrameDropDiagnosticsRecorder, classifyVisualFreshness, zeroActivity } = exportsObject;
const plain = value => JSON.parse(JSON.stringify(value));

function freshnessFixture() {
  const healthy = { ...zeroActivity(), captureUpdatesAcquired: 60, desktopPresents: 60,
    freshFramesPublished: 60, captureClockTickRequests: 60, captureClockTickWakeups: 60,
    encoderFramesAccepted: 60, encoderPacketsProduced: 60, encoderDistinctSourceFrames: 60 };
  const definitions = [
    [{}, {}],
    [{captureClockTickRequests:40, captureClockTickWakeups:40}, {captureClockMode:'encoder-driven-dxgi'}],
    [{captureClockTickWakeups:40}, {captureClockMode:'encoder-driven-dxgi'}],
    [{captureClockTickCompletionTimeouts:8}, {captureClockMode:'encoder-prearmed-dxgi'}],
    [{captureAcquireGraceTimeouts:8}, {captureClockMode:'encoder-driven-dxgi'}],
    [{desktopPresents:45,captureUpdatesAcquired:45,freshFramesPublished:45,encoderFramesAccepted:45,encoderPacketsProduced:45,encoderDistinctSourceFrames:45},{}],
    [{desktopPresents:0,captureUpdatesAcquired:0,freshFramesPublished:0,encoderFramesAccepted:0,encoderPacketsProduced:0,encoderDistinctSourceFrames:0},{}],
    [{freshFramesPublished:40,accumulatedSourceFrames:4,accumulationEvents:1},{}],
    [{freshFramesPublished:40},{}],
    [{captureUpdatesAcquired:30,freshFramesPublished:30},{}],
    [{encoderFramesAccepted:40,encoderPacketsProduced:40,encoderDistinctSourceFrames:40},{}],
    [{encoderPacketsProduced:40,encoderDistinctSourceFrames:40},{}],
    [{encoderDistinctSourceFrames:30},{stillFrameDuplicationEnabled:true}],
    [{encoderDistinctSourceFrames:30},{}],
    [{},{},0],
    [{},{},1250],
  ];
  return plain(definitions.map(([changes, context, windowMs=1000]) => {
    const activity = {...healthy,...changes};
    const diagnostics = {fps:60,stillFrameDuplicationEnabled:false,captureClockMode:'capture-sampled',...context};
    return {activity,diagnostics,windowMs,expected:classifyVisualFreshness(activity,diagnostics,windowMs)};
  }));
}
function recorderFixture() {
  const counterFields = [...source.matchAll(/nonNegativeCounter\(diagnostics\.(\w+)\)/g)].map(match=>match[1]);
  const base = {...Object.fromEntries(counterFields.map(key=>[key,0])),
    engineRunning:true,degraded:false,status:'Fixture recording',capturePressure:'healthy',
    fps:60,captureEpoch:1,captureClockMode:'capture-sampled',stillFrameDuplicationEnabled:false};
  const advancing = n => ({captureDesktopPresents:n,captureAcquiredUpdates:n,capturePublishedFrames:n,
    encoderAcceptedFrames:n,encoderOutputPackets:n,encoderDistinctSourceFrames:n});
  const accumulated = {...base,...advancing(60),droppedFrames:12,captureOverflowDrops:2,
    captureSlotDrops:1,schedulerDroppedFrames:1,encoderBackpressureDrops:8,encoderQueueDrops:2,
    nvencSurfaceDrops:3,nvencInputDrops:1,catchUpEvents:2,historicalFramesRecovered:1,encoderAdmissionRejections:3};
  const inputs = [
    [1000,base], [1500,{...base,...advancing(20)}], [2000,accumulated],
    [3500,{...accumulated,...advancing(150),captureEpoch:2}],
    [4500,{...base,captureEpoch:2}], [5500,{...base,...advancing(60),captureEpoch:2}],
    [6500,{...base,...advancing(120),captureEpoch:2,droppedFrames:4,captureSlotDrops:4}]
  ];
  const recorder = new FrameDropDiagnosticsRecorder(1000,3);
  const observations = inputs.map(([at,diagnostics]) => ({at,diagnostics,expected:recorder.observe(diagnostics,at)}));
  return plain({minimumIntervalMs:1000,maximumSamples:3,observations,report:recorder.report()});
}
if (process.argv[2] === '--emit') {
  const fixture = process.argv[3] === 'freshness' ? freshnessFixture() : recorderFixture();
  console.log(JSON.stringify(fixture));
} else {
  const freshness = JSON.parse(fs.readFileSync(path.join(__dirname,'fixtures/frame-freshness.v1.json'),'utf8'));
  for (const entry of freshness) assert.deepEqual(plain(classifyVisualFreshness(entry.activity,entry.diagnostics,entry.windowMs)),entry.expected);
  const fixture = JSON.parse(fs.readFileSync(path.join(__dirname,'fixtures/frame-recorder.v1.json'),'utf8'));
  const recorder = new FrameDropDiagnosticsRecorder(fixture.minimumIntervalMs,fixture.maximumSamples);
  for (const entry of fixture.observations) assert.deepEqual(plain(recorder.observe(entry.diagnostics,entry.at)),entry.expected);
  assert.deepEqual(plain(recorder.report()),fixture.report);
  console.log('Electron diagnostics oracle passed: '+freshness.length+' freshness cases and '+fixture.observations.length+' recorder observations.');
}
