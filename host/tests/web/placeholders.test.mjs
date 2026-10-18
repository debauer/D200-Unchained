// node --test tests/web/*.test.mjs  (run from host/; also started by tests/test_web_js.py)
import assert from "node:assert/strict";
import { test } from "node:test";

import { PENDING, builtinValues, fill, isDynamic, nameProblem, names } from "../../src/d200_deck/web/static/placeholders.js";

test("names and literal braces, like the host", () => {
  assert.deepEqual([...names("CPU {cpu}% at {time}")].sort(), ["cpu", "time"]);
  assert.equal(names("{{not}} {Cpu} {9x} {}").size, 0);
  assert.equal(isDynamic("plain"), false);
});

test("fill: values, pending, unknown", () => {
  const out = fill("{cpu}% {ram} {typo} {{x}}", { cpu: "42" }, new Set(["cpu", "ram"]));
  assert.equal(out, `42% ${PENDING} {typo} {x}`);
});

test("built-in time and date use fixed English names", () => {
  assert.deepEqual(builtinValues(new Date(2026, 9, 17, 9, 5)), { time: "09:05", date: "Sat 17.10." });
});

test("variable names", () => {
  assert.equal(nameProblem("cpu", []), "");
  assert.match(nameProblem("Cpu", []), /a-z/);
  assert.match(nameProblem("time", []), /built in/);
  assert.match(nameProblem("cpu", ["cpu"]), /exists/);
});
