// |reftest| shell-option(--enable-uint8array-base64) skip-if(!Uint8Array.fromBase64||!xulRuntime.shell) -- uint8array-base64 is not enabled unconditionally, requires shell-options
// Copyright (C) 2024 Kevin Gibbons. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
esid: sec-uint8array.prototype.setfromhex
description: Uint8Array.prototype.setFromHex decodes and writes pairs which occur prior to bad data
features: [uint8array-base64, TypedArray]
---*/

var illegal = [
  'aaa ',
  'aaag',
];
illegal.forEach(function(value) {
  var target = new Uint8Array([255, 255, 255, 255, 255]);
  assert.throws(SyntaxError, function() {
    target.setFromHex(value);
  });
  assert.compareArray(target, [170, 255, 255, 255, 255], "decoding from " + value);
});

var target = new Uint8Array([255, 255, 255, 255, 255]);
assert.throws(SyntaxError, function() {
  target.setFromHex('aaa');
});
assert.compareArray(target, [255, 255, 255, 255, 255], "when length is odd no data is written");

reportCompare(0, 0);
