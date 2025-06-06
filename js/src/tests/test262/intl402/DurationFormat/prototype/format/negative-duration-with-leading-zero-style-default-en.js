// |reftest| skip-if(!Intl.hasOwnProperty('DurationFormat')) -- Intl.DurationFormat is not enabled unconditionally
// Copyright (C) 2024 André Bargull. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-Intl.DurationFormat.prototype.format
description: >
  Test format method with negative duration and leading zero using the default style.
locale: [en-US]
includes: [testIntl.js]
features: [Intl.DurationFormat]
---*/

const duration = {
  hours: 0,
  seconds: -1,
};

const df = new Intl.DurationFormat("en", {hoursDisplay: "always"});

const expected = formatDurationFormatPattern(df, duration);

assert.sameValue(
  df.format(duration),
  expected,
  `DurationFormat format output using default style option`
);

reportCompare(0, 0);
