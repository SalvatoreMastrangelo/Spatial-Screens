import { describe, it, expect } from 'vitest';
import {
  DEG2RAD,
  RAD2DEG,
  quatIdentity,
  quatMultiply,
  quatConjugate,
  quatNormalize,
  eulerToQuat,
  quatToEuler,
  yawTwist,
  quatRotateVector,
  fmt,
} from './math.js';

function expectQuatClose(a, b, digits = 6) {
  expect(a.w).toBeCloseTo(b.w, digits);
  expect(a.x).toBeCloseTo(b.x, digits);
  expect(a.y).toBeCloseTo(b.y, digits);
  expect(a.z).toBeCloseTo(b.z, digits);
}

describe('quatIdentity', () => {
  it('returns the identity quaternion', () => {
    expect(quatIdentity()).toEqual({ w: 1, x: 0, y: 0, z: 0 });
  });
});

describe('quatMultiply', () => {
  it('identity is a multiplicative identity', () => {
    const q = eulerToQuat(12, -30, 45);
    expectQuatClose(quatMultiply(quatIdentity(), q), q);
    expectQuatClose(quatMultiply(q, quatIdentity()), q);
  });

  it('is not generally commutative', () => {
    const a = eulerToQuat(30, 0, 0); // pure roll (about Z)
    const b = eulerToQuat(0, 45, 0); // pure pitch (about X)
    const ab = quatMultiply(a, b);
    const ba = quatMultiply(b, a);
    // For a Z-rotation times an X-rotation the w/x components coincide under
    // either order; the non-commutativity appears in the y component.
    expect(ab.y).not.toBeCloseTo(ba.y, 6);
  });
});

describe('quatConjugate', () => {
  it('negates the vector part and keeps w', () => {
    const q = { w: 0.5, x: 0.1, y: 0.2, z: 0.3 };
    expect(quatConjugate(q)).toEqual({ w: 0.5, x: -0.1, y: -0.2, z: -0.3 });
  });

  it('composed with the original yields identity for a unit quaternion', () => {
    const q = quatNormalize(eulerToQuat(20, 40, -60));
    expectQuatClose(quatMultiply(q, quatConjugate(q)), quatIdentity());
  });
});

describe('quatNormalize', () => {
  it('scales a quaternion to unit length', () => {
    const q = quatNormalize({ w: 2, x: 0, y: 0, z: 0 });
    expectQuatClose(q, quatIdentity());
  });

  it('falls back to identity for a near-zero quaternion', () => {
    expect(quatNormalize({ w: 0, x: 0, y: 0, z: 0 })).toEqual(quatIdentity());
  });
});

describe('eulerToQuat / quatToEuler round-trip', () => {
  it('recovers the original angles away from gimbal lock', () => {
    const cases = [
      [0, 0, 0],
      [10, 20, 30],
      [-15, 5, -60],
      [45, -45, 90],
    ];
    for (const [roll, pitch, yaw] of cases) {
      const q = eulerToQuat(roll, pitch, yaw);
      const e = quatToEuler(q);
      expect(e.roll).toBeCloseTo(roll, 4);
      expect(e.pitch).toBeCloseTo(pitch, 4);
      expect(e.yaw).toBeCloseTo(yaw, 4);
    }
  });

  it('produces a unit quaternion', () => {
    const q = eulerToQuat(33, -17, 82);
    expect(Math.hypot(q.w, q.x, q.y, q.z)).toBeCloseTo(1, 6);
  });

  it('clamps pitch at the +90 gimbal-lock boundary', () => {
    const e = quatToEuler({ w: Math.SQRT1_2, x: Math.SQRT1_2, y: 0, z: 0 });
    expect(e.pitch).toBeCloseTo(90, 4);
  });
});

describe('yawTwist', () => {
  it('is identity for a pure yaw rotation', () => {
    const q = eulerToQuat(0, 0, 40);
    expectQuatClose(yawTwist(q), q);
  });

  it('strips pitch/roll, keeping only the heading component', () => {
    const q = eulerToQuat(25, 15, 70);
    const twist = yawTwist(q);
    expect(twist.x).toBeCloseTo(0, 6);
    expect(twist.z).toBeCloseTo(0, 6);
    expect(Math.hypot(twist.w, twist.y)).toBeCloseTo(1, 6);
  });

  it('falls back to identity when there is no defined heading', () => {
    const q = eulerToQuat(180, 0, 0);
    expectQuatClose(yawTwist(q), quatIdentity());
  });
});

describe('quatRotateVector', () => {
  it('leaves vectors unchanged under the identity rotation', () => {
    const v = { x: 1, y: 2, z: 3 };
    expect(quatRotateVector(quatIdentity(), v)).toEqual(v);
  });

  it('rotates the +Z axis to +X under a +90deg yaw', () => {
    const q = eulerToQuat(0, 0, 90);
    const v = quatRotateVector(q, { x: 0, y: 0, z: 1 });
    expect(v.x).toBeCloseTo(1, 4);
    expect(v.y).toBeCloseTo(0, 4);
    expect(v.z).toBeCloseTo(0, 4);
  });

  it('preserves vector length', () => {
    const q = quatNormalize(eulerToQuat(12, 34, 56));
    const v = { x: 3, y: -4, z: 5 };
    const rotated = quatRotateVector(q, v);
    expect(Math.hypot(rotated.x, rotated.y, rotated.z)).toBeCloseTo(Math.hypot(v.x, v.y, v.z), 6);
  });
});

describe('fmt', () => {
  it('pads non-negative numbers with a leading space', () => {
    expect(fmt(3.14159)).toBe(' 3.14');
  });

  it('does not pad negative numbers', () => {
    expect(fmt(-3.14159)).toBe('-3.14');
  });

  it('respects the digits argument', () => {
    expect(fmt(1.23456, 3)).toBe(' 1.235');
  });
});

describe('DEG2RAD / RAD2DEG', () => {
  it('are reciprocal conversion factors', () => {
    expect(DEG2RAD * RAD2DEG).toBeCloseTo(1, 10);
  });
});
