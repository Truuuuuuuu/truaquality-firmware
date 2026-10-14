import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { runInNewContext } from "node:vm";

const source = readFileSync(new URL("../lib/Provisioning/PhCalibrationPage.h", import.meta.url), "utf8");
const html = source.match(/PH_PAGE_HTML\[\] = R"rawliteral\(([\s\S]*?)\)rawliteral"/)[1];
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const flush = () => new Promise((resolve) => setImmediate(resolve));
const stored = { calAt: 0, bufA: 4.01, bufB: 9.16, tcalC: 27.6,
  slopePct: 2, offsetPh: -2.4, verdict: "replace", age: "date_unknown" };

async function portal(initial)
{
  const elements = new Map();
  const timers = [];
  const requests = [];
  let status = { ph: null, calibrated: true, stored, state: "steady", capture: "idle", ...initial };
  const element = (id) => {
    if (!elements.has(id)) {
      const classes = new Set();
      elements.set(id, { textContent: "", disabled: false, value: id === "sa" ? "686" : "",
        options: [], appendChild() {}, classList: {
          toggle(name, hidden) { if (hidden) classes.add(name); else classes.delete(name); },
          contains(name) { return classes.has(name); },
        } });
    }
    return elements.get(id);
  };
  runInNewContext(script, {
    document: { getElementById: element },
    setTimeout: (callback) => timers.push(callback),
    fetch: async (url) => {
      requests.push(url);
      assert.equal(url, "/ph/status", "these render/cancel scenarios must not send mutations");
      return { json: async () => ({ ...status }) };
    },
  });
  await flush();
  return { element, requests, async poll(next) {
    status = { ...status, ...next };
    assert.equal(timers.length, 1);
    timers.shift()();
    await flush();
  } };
}

test("calibrated null reading says unavailable and preserves the stored calibration", async () => {
  const p = await portal();
  assert.equal(p.element("ph").textContent, "pH: unavailable");
  assert.equal(p.element("sk").classList.contains("hide"), false);
  assert.equal(p.element("sbuf").textContent, "Buffers (pH used): 4.01 and 9.16 at 27.6 °C");
});

test("missing live reading with false or missing calibration stays uncalibrated", async () => {
  const p = await portal({ calibrated: false, stored: null });
  assert.equal(p.element("ph").textContent, "pH: uncalibrated");
  await p.poll({ calibrated: undefined });
  assert.equal(p.element("ph").textContent, "pH: uncalibrated");
});

test("finite readings retain two decimals and recover after unavailable", async () => {
  const p = await portal();
  await p.poll({ ph: 7.126 });
  assert.equal(p.element("ph").textContent, "pH: 7.13");
  await p.poll({ ph: null });
  assert.equal(p.element("ph").textContent, "pH: unavailable");
  await p.poll({ ph: 6.8 });
  assert.equal(p.element("ph").textContent, "pH: 6.80");
});

test("actual Clear then Cancel handlers preserve calibration and send no clear request", async () => {
  const p = await portal();
  const before = p.element("sbuf").textContent;
  p.element("cx").onclick();
  assert.equal(p.element("cf").classList.contains("hide"), false);
  p.element("cn").onclick();
  assert.equal(p.element("cf").classList.contains("hide"), true);
  await p.poll({ ph: null });
  assert.equal(p.element("sbuf").textContent, before);
  assert.equal(p.element("sk").classList.contains("hide"), false);
  assert.deepEqual(p.requests, ["/ph/status", "/ph/status"]);
});

test("pH page fits its UTF-8 budget including terminating null", () => {
  assert.ok(Buffer.byteLength(html, "utf8") + 1 <= 9216);
});
