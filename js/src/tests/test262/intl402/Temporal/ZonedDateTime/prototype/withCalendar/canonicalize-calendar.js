// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2024 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.zonedatetime.prototype.withcalendar
description: Calendar ID is canonicalized
features: [Temporal]
---*/

const instance = new Temporal.ZonedDateTime(1719923640_000_000_000n, "UTC");
const result = instance.withCalendar("islamicc");
assert.sameValue(result.calendarId, "islamic-civil", "calendar ID is canonicalized");

reportCompare(0, 0);
