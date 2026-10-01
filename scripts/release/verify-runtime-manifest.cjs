const fs = require("node:fs");
const { createHash, createPublicKey, verify } = require("node:crypto");

function verifyManifest(bytes, publicEnvelope, signatureEnvelope) {
  const keyLines = Buffer.from(publicEnvelope.trim(), "base64").toString("utf8").trim().split(/\r?\n/);
  const lines = Buffer.from(signatureEnvelope.trim(), "base64").toString("utf8").trim().split(/\r?\n/);
  const key = Buffer.from(keyLines[1] || "", "base64");
  const packet = Buffer.from(lines[1] || "", "base64");
  if (key.length !== 42 || key.subarray(0, 2).toString() !== "Ed" || packet.length !== 74 || packet.subarray(0, 2).toString() !== "ED" || !packet.subarray(2, 10).equals(key.subarray(2, 10)) || !lines[2]?.startsWith("trusted comment: ")) {
    throw new Error("Invalid Minisign envelope");
  }
  const publicKey = createPublicKey({ key: Buffer.concat([Buffer.from("302a300506032b6570032100", "hex"), key.subarray(10)]), format: "der", type: "spki" });
  const signature = packet.subarray(10);
  if (!verify(null, createHash("blake2b512").update(bytes).digest(), publicKey, signature) ||
      !verify(null, Buffer.concat([signature, Buffer.from(lines[2].slice("trusted comment: ".length))]), publicKey, Buffer.from(lines[3] || "", "base64"))) {
    throw new Error("Runtime manifest signature verification failed");
  }
}

if (require.main === module) {
  const [manifest, config] = process.argv.slice(2);
  const key = JSON.parse(fs.readFileSync(config, "utf8")).plugins.updater.pubkey;
  verifyManifest(fs.readFileSync(manifest), key, fs.readFileSync(`${manifest}.sig`, "utf8"));
  console.log("Runtime manifest signature verified against release public key.");
}
module.exports = { verifyManifest };
