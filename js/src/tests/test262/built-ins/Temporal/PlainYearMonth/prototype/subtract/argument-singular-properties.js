// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.plainyearmonth.prototype.subtract
description: Singular properties in the property bag are always ignored
features: [Temporal]
---*/

const instance = new Temporal.PlainYearMonth(2000, 5);

[
  { year: 1 },
  { month: 2 },
  { week: 3 },
  { day: 4 },
  { hour: 5 },
  { minute: 6 },
  { second: 7 },
  { millisecond: 8 },
  { microsecond: 9 },
  { nanosecond: 10 },
].forEach((badObject) => {
  assert.throws(TypeError, () => instance.subtract(badObject),
    "Throw TypeError if temporalDurationLike is not valid");
});


reportCompare(0, 0);
