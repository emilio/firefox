// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.plainyearmonth.prototype.tostring
description: If calendarName is "never", the calendar ID should be omitted.
features: [Temporal]
---*/

const yearmonth = new Temporal.PlainYearMonth(2000, 5);
const result = yearmonth.toString({ calendarName: "never" });
assert.sameValue(result, "2000-05", `built-in ISO calendar for calendarName = never`);

reportCompare(0, 0);
