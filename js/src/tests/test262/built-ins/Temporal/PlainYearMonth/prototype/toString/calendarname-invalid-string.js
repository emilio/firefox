// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2021 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.plainyearmonth.protoype.tostring
description: RangeError thrown when calendarName option not one of the allowed string values
info: |
    sec-getoption step 10:
      10. If _values_ is not *undefined* and _values_ does not contain an element equal to _value_, throw a *RangeError* exception.
    sec-temporal-toshowcalendaroption step 1:
      1. Return ? GetOption(_normalizedOptions_, *"calendarName"*, « *"string"* », « *"auto"*, *"always"*, *"never"*, *"critical"* », *"auto"*).
    sec-temporal.plainyearmonth.protoype.tostring step 4:
      4. Let _showCalendar_ be ? ToShowCalendarOption(_options_).
features: [Temporal]
---*/

const yearmonth = new Temporal.PlainYearMonth(2000, 5);
const invalidValues = ["ALWAYS", "sometimes", "other string", "auto\0"];

for (const calendarName of invalidValues) {
  assert.throws(
    RangeError,
    () => yearmonth.toString({ calendarName }),
    `${calendarName} is an invalid value for calendarName option`
  );
}

reportCompare(0, 0);
