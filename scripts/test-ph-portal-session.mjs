import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const source = readFileSync(new URL("../lib/Provisioning/Provisioning.cpp", import.meta.url), "utf8");
function extract(name) {
  const start = source.indexOf(`void ${name}()`);
  assert.ok(start >= 0);
  let depth = 0;
  for (let i = source.indexOf("{", start); i < source.length; i++) {
    if (source[i] === "{") depth++;
    if (source[i] === "}" && --depth === 0) return source.slice(start, i + 1);
  }
  throw new Error(`Unclosed function ${name}`);
}

test("real sampler clears terminal/active sessions, cancels first and preserves stored calibration", () => {
  const dir = mkdtempSync(join(tmpdir(), "ph-portal-session-"));
  try {
    const cpp = join(dir, "session.cpp");
    const binary = join(dir, "session");
    writeFileSync(cpp, `
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>
#include "PhAcceptance.h"
using namespace ph::accept;
CaptureEngine phEngine{};
Slot phCaptureSlot = Slot::None;
HeldPair phHeld{};
PairResult phReview{};
bool phReviewValid = false, phCaptureStartedInPortal = false, phSamplerPrimed = false;
unsigned long phCaptureStartMs = 0, phLastSampleMs = 0;
constexpr unsigned long SAMPLER_INTERVAL_MS = 1000;
float phLiveRing[ph::cal::CAPTURE_WINDOW_SAMPLES]{};
size_t phLiveFilled = 0, phLiveNext = 0;
float phLatestMv = NAN, phLatestPh = NAN, phLatestTempC = NAN;
ph::Calibration phCal{1737,401,1764,916,2760,1780000000,ph::PH_CAL_VERSION};
struct { bool open = false; bool getConfigPortalActive() { return open; } } wm;
struct { float (*readPhSensorMv)(float) = nullptr; float (*pollTemperatureC)() = nullptr;
float (*lastPhValue)() = nullptr; } config;
unsigned long millis() { return 20000; }
unsigned cancellations = 0;
struct {
void println(const char *) {
  assert(phEngine.reason == Refusal::Cancelled);
  assert(phEngine.state == CaptureState::Refused);
  ++cancellations;
}
template<class... T> void printf(const char *, T...) {}
} Serial;
${extract("resetPhLive")}
${extract("tickPhSampler")}
int main() {
  const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&phCal);
  unsigned char stored[sizeof(phCal)]; std::memcpy(stored, bytes, sizeof(phCal));
  const CaptureState states[] = {CaptureState::Refused, CaptureState::Accepted,
    CaptureState::Capturing, CaptureState::Settling};
  for (const auto state : states) {
    wm.open = false;
    phEngine = CaptureEngine{}; phEngine.state = state;
    phEngine.reason = Refusal::BuffersTooClose; phEngine.filled = 20; phEngine.elapsedS = 19;
    phEngine.nominal = 686; phEngine.point.held = true;
    phCaptureSlot = Slot::A; phCaptureStartedInPortal = true; phCaptureStartMs = 1234;
    phHeld.a.held = phHeld.b.held = true;
    phReview.complete = true; phReview.reason = Refusal::BuffersTooClose; phReviewValid = true;
    phLiveFilled = 20; phLiveNext = 7; phSamplerPrimed = true;
    phLatestMv = 1780; phLatestPh = 9; phLatestTempC = 28;
    const auto before = cancellations;
    tickPhSampler();
    assert(cancellations == before + (state == CaptureState::Capturing || state == CaptureState::Settling));
    assert(phEngine.state == CaptureState::Idle);
    assert(phEngine.reason == Refusal::None && phEngine.filled == 0 && phEngine.elapsedS == 0);
    assert(phEngine.nominal == 0 && !phEngine.point.held);
    assert(phCaptureSlot == Slot::None && !phCaptureStartedInPortal && phCaptureStartMs == 0);
    assert(!phHeld.a.held && !phHeld.b.held && !phReviewValid && !phReview.complete);
    assert(phReview.reason == Refusal::None);
    assert(phLiveFilled == 0 && phLiveNext == 0 && !phSamplerPrimed);
    assert(std::isnan(phLatestMv) && std::isnan(phLatestPh) && std::isnan(phLatestTempC));
    tickPhSampler(); // Repeated closed ticks do not repeat cancellation.
    wm.open = true; tickPhSampler(); // No sensor hook: reopening must not revive old metadata.
    assert(phEngine.state == CaptureState::Idle && phCaptureSlot == Slot::None);
    assert(cancellations == before + (state == CaptureState::Capturing || state == CaptureState::Settling));
    assert(std::memcmp(stored, &phCal, sizeof(phCal)) == 0);
  }
  std::puts("4 lifecycle cases passed; active cancellations occurred before reset; stored bytes unchanged");
}
`);
    execFileSync("clang++", ["-std=c++11", "-I", fileURLToPath(new URL("../lib/PhMath", import.meta.url)), cpp, "-o", binary]);
    const output = execFileSync(binary, { encoding: "utf8" });
    assert.match(output, /4 lifecycle cases passed/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
