import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { runInNewContext } from "node:vm";

const page = readFileSync(new URL("../lib/Provisioning/CalibrationPage.h", import.meta.url), "utf8");
const script = page.match(/<script>([\s\S]*?)<\/script>/)[1];
const busy = "A capture is running; wait for it to finish.";
const flush = () => new Promise((resolve) => setImmediate(resolve));

async function portal()
{
  const elements = new Map();
  const timers = [];
  let status = { state: "steady", capture: "idle", mv: 2974, ntu: 0,
    storedMv: 2974, calibratedAt: 0, samples: 2, needed: 20,
    medianMv: 2974, spreadMv: 4, reason: "signal_lost" };
  let captureStatus = 409;
  let networkFailure = false;
  const element = (id) => {
    if (!elements.has(id)) elements.set(id, { textContent: "", disabled: false, value: "" });
    return elements.get(id);
  };
  runInNewContext(script, {
    document: { getElementById: element },
    setTimeout: (callback) => timers.push(callback),
    fetch: async (url, options) => {
      if (url === "/cal/status") return { json: async () => ({ ...status }) };
      assert.equal(url, "/cal/capture");
      assert.equal(options.method, "POST");
      if (networkFailure) throw new Error("offline");
      return { status: captureStatus };
    },
  });
  await flush();
  return {
    element,
    async capture(code = 409, offline = false) {
      captureStatus = code;
      networkFailure = offline;
      element("cap").onclick();
      await flush();
    },
    async poll(capture) {
      status = { ...status, capture };
      assert.equal(timers.length, 1, "polling remains a single timeout chain");
      timers.shift()();
      await flush();
    },
  };
}

test("busy request survives idle polls and leaves Capture available for retry", async () => {
  const p = await portal();
  await p.capture();
  assert.equal(p.element("cr").textContent, busy);
  await p.poll("idle");
  await p.poll("idle");
  assert.equal(p.element("cr").textContent, busy);
  assert.equal(p.element("cap").disabled, false);
});

test("stale refused and accepted results cannot erase a new busy request", async () => {
  const p = await portal();
  await p.capture();
  await p.poll("refused");
  assert.equal(p.element("cr").textContent, busy);
  await p.poll("accepted");
  assert.equal(p.element("cr").textContent, busy);
});

test("retry clears request feedback and renders real progress and saved result", async () => {
  const p = await portal();
  await p.capture();
  await p.capture(200);
  assert.equal(p.element("cr").textContent, "Starting capture…");
  await p.poll("capturing");
  assert.equal(p.element("cr").textContent, "Capturing 2/20 - hold still");
  assert.equal(p.element("cap").disabled, true);
  await p.poll("accepted");
  assert.equal(p.element("cr").textContent, "Saved: 2974 mV (spread 4 mV)");
  assert.equal(p.element("cap").disabled, false);
});

test("network feedback survives idle then clears when actual capture progress arrives", async () => {
  const p = await portal();
  await p.capture(200, true);
  await p.poll("idle");
  assert.equal(p.element("cr").textContent, "Could not reach the unit.");
  await p.poll("capturing");
  assert.equal(p.element("cr").textContent, "Capturing 2/20 - hold still");
  await p.poll("accepted");
  assert.equal(p.element("cr").textContent, "Saved: 2974 mV (spread 4 mV)");
});
