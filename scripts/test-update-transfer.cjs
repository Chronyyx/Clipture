const assert = require("node:assert/strict");
const {
  boundedUpdateWriteSize,
  buildUpdateTransferPlan,
  maximumUpdateWriteBytes,
  UpdateWritePacer,
  UpdateWriteRateController
} = require("../dist/main/UpdateTransferPolicy.js");
const { CaptureAwareUpdateTransfer } = require("../dist/main/CaptureAwareUpdater.js");

const mib = 1024 * 1024;

function testBlockMapPlan() {
  const oldBlockMap = {
    version: "2",
    files: [{
      name: "file",
      offset: 0,
      checksums: ["a", "b", "c"],
      sizes: [4, 4, 2]
    }]
  };
  const newBlockMap = {
    version: "2",
    files: [{
      name: "file",
      offset: 0,
      checksums: ["a", "x", "c"],
      sizes: [4, 3, 2]
    }]
  };

  assert.deepEqual(buildUpdateTransferPlan(oldBlockMap, newBlockMap), {
    operations: [
      { kind: "copy", start: 0, end: 4 },
      { kind: "download", start: 4, end: 7 },
      { kind: "copy", start: 8, end: 10 }
    ],
    copyBytes: 6,
    downloadBytes: 3,
    totalBytes: 9
  });

  assert.throws(
    () => buildUpdateTransferPlan(oldBlockMap, { ...newBlockMap, version: "1" }),
    /versions differ/
  );
}

function testBoundedWrites() {
  assert.equal(boundedUpdateWriteSize(0), 0);
  assert.equal(boundedUpdateWriteSize(1), 1);
  assert.equal(boundedUpdateWriteSize(maximumUpdateWriteBytes), maximumUpdateWriteBytes);
  assert.equal(boundedUpdateWriteSize(maximumUpdateWriteBytes * 4), maximumUpdateWriteBytes);
}

function testPressureController() {
  const controller = new UpdateWriteRateController();
  assert.equal(controller.currentBytesPerSecond(), 32 * mib);

  assert.equal(controller.observePressure("elevated"), true);
  assert.equal(controller.currentBytesPerSecond(), 16 * mib);

  assert.equal(controller.observePressure("critical"), true);
  assert.equal(controller.currentBytesPerSecond(), 4 * mib);

  assert.equal(controller.observePressure("healthy"), true);
  assert.equal(controller.currentBytesPerSecond(), 12 * mib);

  for (let index = 0; index < 32; index += 1) {
    controller.observeWrite(512 * 1024, 1);
  }
  assert.ok(controller.currentBytesPerSecond() > 12 * mib);
  assert.ok(controller.currentBytesPerSecond() <= 96 * mib);
}

async function testPacer() {
  const controller = new UpdateWriteRateController();
  let nowMs = 0;
  const waits = [];
  const pacer = new UpdateWritePacer(
    controller,
    () => nowMs,
    async (delayMs) => {
      waits.push(delayMs);
      nowMs += delayMs;
    }
  );

  assert.equal(await pacer.pace(512 * 1024), 0);
  const secondWait = await pacer.pace(512 * 1024);
  assert.ok(secondWait >= 15 && secondWait <= 16);
  assert.equal(waits.length, 1);
}

async function testBoundedWriter() {
  const writes = [];
  const transfer = new CaptureAwareUpdateTransfer(() => "healthy", async () => {});
  transfer.start(
    { enableNetworkEmulation() {}, disableNetworkEmulation() {} },
    { info() {}, warn() {}, error() {} }
  );
  try {
    const data = Buffer.alloc(maximumUpdateWriteBytes * 2 + 17);
    await transfer.write({
      async write(buffer, offset, length) {
        writes.push(length);
        return { bytesWritten: length, buffer };
      }
    }, data, 0);
    assert.deepEqual(writes, [maximumUpdateWriteBytes, maximumUpdateWriteBytes, 17]);
  } finally {
    transfer.stop();
  }
}

async function main() {
  testBlockMapPlan();
  testBoundedWrites();
  testPressureController();
  await testPacer();
  await testBoundedWriter();
  process.stdout.write("Update transfer policy tests passed.\n");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
