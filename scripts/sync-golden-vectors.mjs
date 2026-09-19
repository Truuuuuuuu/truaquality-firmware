// DEV ONLY. Mirrors the backend's golden signing vectors into test/golden/ so the firmware suites can prove
// they build and sign the exact same bytes.
//
//   node scripts/sync-golden-vectors.mjs
//   node scripts/sync-golden-vectors.mjs --check
//
// One-directional, on purpose. The backend is the authority for the wire format
// (backend/src/lib/__fixtures__/signing-vectors.v1.json, written by `npm run generate:signing-vectors`);
// this script only ever reads from it and writes into firmware. It never writes into backend/. If a firmware
// test fails against a vector, the firmware is wrong — never regenerate the backend fixture to make it pass.
//
// Every secret in these vectors is a dummy literal containing "not-real". This script never touches
// DEVICE_SECRET_MASTER_KEY or a provisioned unit's NVS, and it refuses to write anything if a secret that
// doesn't look like a dummy ever shows up — a real device secret must never reach a committed file.
//
// Plain ESM JavaScript with no dependencies: firmware/ is a PlatformIO project, not an npm one, so there is
// no package.json, no node_modules, and no `npm run` wrapper. Import only from node: builtins.
import { readFileSync, writeFileSync, mkdirSync, existsSync } from "node:fs";
import { parseArgs } from "node:util";

// Fixture key order. The generated struct's members follow it exactly, so a reordering of the backend fixture
// shows up as a compile-time shape change here rather than as silently swapped strings.
const FIELDS = ["name", "secret", "deviceId", "topic", "body", "signature", "payload"];

const BACKEND_FIXTURE = new URL("../../backend/src/lib/__fixtures__/signing-vectors.v1.json", import.meta.url);
const JSON_COPY = new URL("../test/golden/signing-vectors.v1.json", import.meta.url);
const HEADER = new URL("../test/golden/signing_vectors.h", import.meta.url);

const JSON_COPY_LABEL = "test/golden/signing-vectors.v1.json";
const HEADER_LABEL = "test/golden/signing_vectors.h";

const { values } = parseArgs({
  options: {
    check: { type: "boolean", default: false },
  },
});

// Read the authority if it's there. If firmware/ is checked out on its own (no sibling backend/), fall back to
// the committed copy so regenerating the header still works offline — but say so loudly, because in that mode
// nothing is actually being verified against the backend.
let sourceLabel = "backend/src/lib/__fixtures__/signing-vectors.v1.json";
let sourceText;
let fromBackend = true;
if (existsSync(BACKEND_FIXTURE)) {
  sourceText = readFileSync(BACKEND_FIXTURE, "utf8");
} else {
  fromBackend = false;
  sourceLabel = JSON_COPY_LABEL;
  if (!existsSync(JSON_COPY)) {
    console.error(
      `Cannot find the backend fixture (backend/src/lib/__fixtures__/signing-vectors.v1.json) or the committed copy (${JSON_COPY_LABEL}). Nothing to sync from.`,
    );
    process.exit(1);
  }
  console.warn(
    `Backend fixture not found — falling back to the committed ${JSON_COPY_LABEL}. Drift against the backend is NOT being checked in this mode.`,
  );
  sourceText = readFileSync(JSON_COPY, "utf8");
}

let fixture;
try {
  fixture = JSON.parse(sourceText);
} catch (err) {
  console.error(`${sourceLabel} is not valid JSON: ${err.message}`);
  process.exit(1);
}

const vectors = fixture.vectors;
if (!Array.isArray(vectors) || vectors.length === 0) {
  console.error(`${sourceLabel} has no "vectors" array.`);
  process.exit(1);
}

for (const [index, vector] of vectors.entries()) {
  for (const field of FIELDS) {
    if (typeof vector[field] !== "string") {
      console.error(`Vector #${index} is missing the string field "${field}". Refusing to generate a header from it.`);
      process.exit(1);
    }
  }
}

// Secret discipline, mirroring the assertion in backend/src/lib/deviceMessages.test.ts. A vector secret that
// isn't a dummy means a real one leaked into the fixture; writing it here would publish it to a public repo.
for (const vector of vectors) {
  if (!/not-real/.test(vector.secret)) {
    console.error(
      `Vector "${vector.name}" has a secret that does not contain "not-real". Refusing to write it into a committed file — golden vectors must use dummy secrets only.`,
    );
    process.exit(1);
  }
}

// The generated header wraps every string in R"RAW(...)RAW". That delimiter cannot be escaped, so a value
// containing the closing sequence would silently truncate the literal and produce a wrong-but-compiling test.
for (const vector of vectors) {
  for (const field of FIELDS) {
    if (vector[field].includes(')RAW"')) {
      console.error(
        `Vector "${vector.name}" field "${field}" contains the raw-string delimiter )RAW" and cannot be emitted safely. Change the delimiter in this script before syncing it.`,
      );
      process.exit(1);
    }
  }
}

// (a) The JSON copy — the backend's bytes verbatim, so `diff` against the authority is meaningful and a
// whitespace-only difference still counts as drift.
const jsonText = fromBackend ? sourceText : JSON.stringify(fixture, null, 2) + "\n";

// (b) The C++ header — what the two test suites actually compile against. The ESP32 cannot read a file
// without SPIFFS, so the vectors have to arrive as literals.
function renderHeader() {
  const lines = [];
  lines.push("#pragma once");
  lines.push("");
  lines.push(
    "// GENERATED by scripts/sync-golden-vectors.mjs from backend/src/lib/__fixtures__/signing-vectors.v1.json — do not edit.",
  );
  lines.push(
    '// Every secret below is a dummy literal containing "not-real", so this file is safe to commit. Regenerate',
  );
  lines.push("// with `node scripts/sync-golden-vectors.mjs`; check for drift with `--check`.");
  lines.push("");
  lines.push("struct GoldenVector");
  lines.push("{");
  for (const field of FIELDS) {
    lines.push(`  const char *${field};`);
  }
  lines.push("};");
  lines.push("");
  lines.push("static const GoldenVector GOLDEN_VECTORS[] = {");
  for (const vector of vectors) {
    lines.push("  {");
    for (const field of FIELDS) {
      lines.push(`    R"RAW(${vector[field]})RAW",`);
    }
    lines.push("  },");
  }
  lines.push("};");
  lines.push("");
  lines.push(`static const unsigned GOLDEN_VECTOR_COUNT = ${vectors.length};`);
  return lines.join("\n") + "\n";
}

const headerText = renderHeader();

if (values.check) {
  const drifted = [];
  for (const [url, label, expected] of [
    [JSON_COPY, JSON_COPY_LABEL, jsonText],
    [HEADER, HEADER_LABEL, headerText],
  ]) {
    if (!existsSync(url)) {
      drifted.push(`${label} (missing)`);
    } else if (readFileSync(url, "utf8") !== expected) {
      drifted.push(label);
    }
  }
  if (drifted.length > 0) {
    console.error(`Golden vectors have drifted from ${sourceLabel}: ${drifted.join(", ")}.`);
    console.error("Run `node scripts/sync-golden-vectors.mjs` (without --check) to resync, then commit the result.");
    process.exit(1);
  }
  console.log(`Golden vectors are in sync with ${sourceLabel} (${vectors.length} vectors).`);
  process.exit(0);
}

mkdirSync(new URL("../test/golden/", import.meta.url), { recursive: true });
writeFileSync(JSON_COPY, jsonText);
writeFileSync(HEADER, headerText);
console.log(`Wrote ${vectors.length} golden vectors from ${sourceLabel} to ${JSON_COPY_LABEL} and ${HEADER_LABEL}.`);
