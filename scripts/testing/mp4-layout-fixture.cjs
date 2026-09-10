// Narrow, read-only inspector for the engine's generated one-video-track fixtures.
// Not a general-purpose parser for untrusted MP4s or a production metadata writer.
const fs = require('node:fs');
const assert = require('node:assert/strict');

function boxes(bytes, start = 0, end = bytes.length) {
  const result = [];
  for (let offset = start; offset < end;) {
    assert.ok(end - offset >= 8, 'truncated atom');
    let length = bytes.readUInt32BE(offset);
    let header = 8;
    if (length === 1) {
      assert.ok(end - offset >= 16);
      const large = bytes.readBigUInt64BE(offset + 8);
      assert.ok(large <= BigInt(Number.MAX_SAFE_INTEGER));
      length = Number(large);
      header = 16;
    }
    assert.ok(length >= header && length <= end - offset, 'invalid atom bounds');
    result.push({ type: bytes.toString('ascii', offset + 4, offset + 8), start: offset,
      data: offset + header, end: offset + length, header });
    offset += length;
  }
  return result;
}

function inspect(bytes) {
  const top = boxes(bytes);
  assert.deepEqual(top.map(box => box.type), top[1].type === 'free'
    ? ['ftyp', 'free', 'mdat', 'moov'] : ['ftyp', 'mdat', 'moov']);
  const mdat = top.find(box => box.type === 'mdat'), moov = top.at(-1);
  let parent = moov;
  for (const type of ['trak', 'mdia', 'minf', 'stbl']) {
    const found = boxes(bytes, parent.data, parent.end).filter(box => box.type === type);
    assert.equal(found.length, 1, 'one video track fixture');
    parent = found[0];
  }
  const tables = boxes(bytes, parent.data, parent.end);
  const get = type => {
    const found = tables.filter(box => box.type === type);
    assert.equal(found.length, 1);
    return found[0];
  };
  const sizes = get('stsz'), offsets = get('co64'), chunks = get('stsc');
  assert.equal(bytes.readUInt32BE(sizes.data + 4), 0, 'explicit sample sizes');
  const count = bytes.readUInt32BE(sizes.data + 8);
  assert.equal(sizes.end - sizes.data, 12 + count * 4);
  assert.equal(bytes.readUInt32BE(offsets.data + 4), count, 'one chunk per sample');
  assert.equal(offsets.end - offsets.data, 8 + count * 8);
  assert.equal(bytes.readUInt32BE(chunks.data + 4), 1);
  assert.equal(bytes.readUInt32BE(chunks.data + 8), 1);
  assert.equal(bytes.readUInt32BE(chunks.data + 12), 1);
  const samples = [];
  for (let index = 0; index < count; index++) {
    const offset = bytes.readBigUInt64BE(offsets.data + 8 + index * 8);
    assert.ok(offset <= BigInt(Number.MAX_SAFE_INTEGER));
    const length = bytes.readUInt32BE(sizes.data + 12 + index * 4);
    assert.ok(length > 0 && Number(offset) >= mdat.data && Number(offset) + length <= mdat.end,
      'sample points inside mdat');
    samples.push({ offset: Number(offset), length });
  }
  const normalizedMoov = Buffer.from(bytes.subarray(moov.start, moov.end));
  normalizedMoov.fill(0, offsets.data + 8 - moov.start, offsets.end - moov.start);
  return { mdat, samples, normalizedMoov };
}

function verifyAlignedMp4(baselinePath, alignedPath) {
  const baseline = fs.readFileSync(baselinePath), aligned = fs.readFileSync(alignedPath);
  const old = inspect(baseline), current = inspect(aligned);
  assert.equal(current.mdat.header, 16, 'aligned path uses large-size mdat header');
  assert.deepEqual(current.normalizedMoov, old.normalizedMoov, 'only chunk offsets may change in movie metadata');
  let cursor = current.mdat.data, padding = 0, mediaBytes = 0;
  for (let i = 0; i < old.samples.length; i++) {
    const before = old.samples[i], after = current.samples[i];
    assert.equal(after.length, before.length, 'no padding included in sample size');
    assert.ok(after.offset >= cursor, 'sample ranges never overlap');
    assert.ok(aligned.subarray(cursor, after.offset).every(byte => byte === 0), 'all gaps are zero-filled');
    padding += after.offset - cursor;
    assert.deepEqual(aligned.subarray(after.offset, after.offset + after.length),
      baseline.subarray(before.offset, before.offset + before.length), 'encoded sample must be unchanged');
    cursor = after.offset + after.length;
    mediaBytes += after.length;
  }
  assert.equal(cursor, current.mdat.end);
  assert.equal(aligned.length, baseline.length + padding + current.mdat.header - old.mdat.header);
  assert.ok(padding > 0, 'fixture must exercise actual placement gaps');
  return { samples: old.samples.length, mediaBytes, paddingBytes: padding, fileBytes: aligned.length };
}

function verifyInPlaceMp4(baselinePath, inPlacePath) {
  const baseline = fs.readFileSync(baselinePath), currentBytes = fs.readFileSync(inPlacePath);
  const old = inspect(baseline), current = inspect(currentBytes);
  assert.equal(current.mdat.data, 4096, 'media stays at its original reserved-header offset');
  assert.equal(current.mdat.header, 16);
  assert.deepEqual(current.normalizedMoov, old.normalizedMoov, 'same timing and sample tables except offsets');
  assert.equal(current.samples.length, old.samples.length);
  let cursor = current.mdat.data;
  for (let i = 0; i < old.samples.length; ++i) {
    const before = old.samples[i], after = current.samples[i];
    assert.equal(after.offset, cursor, 'append-only fixture has contiguous unchanged media');
    assert.equal(after.length, before.length);
    assert.deepEqual(currentBytes.subarray(after.offset, after.offset + after.length),
      baseline.subarray(before.offset, before.offset + before.length));
    cursor += after.length;
  }
  assert.equal(cursor, current.mdat.end);
  assert.equal(currentBytes.length, baseline.length + current.mdat.data - old.mdat.data);
  return { mediaBytes: cursor - current.mdat.data, fileBytes: currentBytes.length };
}

module.exports = { verifyAlignedMp4, verifyInPlaceMp4 };
