// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.plaintime.prototype.until
description: Supports sub-second precision
includes: [temporalHelpers.js]
features: [Temporal, arrow-function]
---*/

const time1 = Temporal.PlainTime.from("10:23:15");
const time2 = Temporal.PlainTime.from("17:15:57.250250250");

TemporalHelpers.assertDuration(time1.until(time2, { largestUnit: "milliseconds" }),
  0, 0, 0, 0, 0, 0, 0, /* milliseconds = */ 24762250, 250, 250, "milliseconds");

TemporalHelpers.assertDuration(time1.until(time2, { largestUnit: "microseconds" }),
  0, 0, 0, 0, 0, 0, 0, /* milliseconds = */ 0, 24762250250, 250, "microseconds");

TemporalHelpers.assertDuration(time1.until(time2, { largestUnit: "nanoseconds" }),
  0, 0, 0, 0, 0, 0, 0, /* milliseconds = */ 0, 0, 24762250250250, "nanoseconds");

reportCompare(0, 0);
