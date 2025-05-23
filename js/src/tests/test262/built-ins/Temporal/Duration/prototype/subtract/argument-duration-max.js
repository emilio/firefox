// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2023 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.duration.prototype.subtract
description: Maximum allowed duration
features: [Temporal]
---*/

const instance = new Temporal.Duration();

const maxCases = [
  ["P104249991374DT7H36M31.999999999S", "string with max days"],
  [{ days: 104249991374, nanoseconds: 27391999999999 }, "property bag with max days"],
  ["PT2501999792983H36M31.999999999S", "string with max hours"],
  [{ hours: 2501999792983, nanoseconds: 2191999999999 }, "property bag with max hours"],
  ["PT150119987579016M31.999999999S", "string with max minutes"],
  [{ minutes: 150119987579016, nanoseconds: 31999999999 }, "property bag with max minutes"],
  ["PT9007199254740991.999999999S", "string with max seconds"],
  [{ seconds: 9007199254740991, nanoseconds: 999999999 }, "property bag with max seconds"],
];

for (const [arg, descr] of maxCases) {
  const result = instance.subtract(arg);
  assert.sameValue(result.with({ years: 0, months: 0, weeks: 0 }).total("seconds"), -9007199254740991.999999999, `operation succeeds with ${descr}`);
}

const minCases = [
  ["-P104249991374DT7H36M31.999999999S", "string with min days"],
  [{ days: -104249991374, nanoseconds: -27391999999999 }, "property bag with min days"],
  ["-PT2501999792983H36M31.999999999S", "string with min hours"],
  [{ hours: -2501999792983, nanoseconds: -2191999999999 }, "property bag with min hours"],
  ["-PT150119987579016M31.999999999S", "string with min minutes"],
  [{ minutes: -150119987579016, nanoseconds: -31999999999 }, "property bag with min minutes"],
  ["-PT9007199254740991.999999999S", "string with min seconds"],
  [{ seconds: -9007199254740991, nanoseconds: -999999999 }, "property bag with min seconds"],
];

for (const [arg, descr] of minCases) {
  const result = instance.subtract(arg);
  assert.sameValue(result.with({ years: 0, months: 0, weeks: 0 }).total("seconds"), 9007199254740991.999999999, `operation succeeds with ${descr}`);
}

reportCompare(0, 0);
