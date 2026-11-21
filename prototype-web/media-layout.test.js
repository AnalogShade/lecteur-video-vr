import assert from 'node:assert/strict';
import test from 'node:test';
import { displayAspect, eyeRegion } from './media-layout.js';

test('stereo samples the left and right halves independently', () => {
  assert.deepEqual(eyeRegion('sbs', 0), [0.5, 1, 0, 0]);
  assert.deepEqual(eyeRegion('sbs', 1), [0.5, 1, 0.5, 0]);
  assert.deepEqual(eyeRegion('sbs', 0, true), eyeRegion('sbs', 1));
  assert.deepEqual(eyeRegion('sbs', 1, true), eyeRegion('sbs', 0));
});

test('stereo top/bottom samples the top half for the left eye', () => {
  assert.deepEqual(eyeRegion('tb', 0), [1, 0.5, 0, 0.5]);
  assert.deepEqual(eyeRegion('tb', 1), [1, 0.5, 0, 0]);
  assert.deepEqual(eyeRegion('tb', 0, true), eyeRegion('tb', 1));
  assert.deepEqual(eyeRegion('tb', 1, true), eyeRegion('tb', 0));
});

test('a flat screen uses the aspect ratio of one eye', () => {
  assert.equal(displayAspect(1920, 1080, 'mono'), 16 / 9);
  assert.equal(displayAspect(3840, 1080, 'sbs'), 16 / 9);
  assert.equal(displayAspect(1920, 2160, 'tb'), 16 / 9);
});
