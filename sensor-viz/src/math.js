// Quaternion / angle helpers shared by drivers and UI.
// Quaternions are plain objects {w, x, y, z}.

export const DEG2RAD = Math.PI / 180;
export const RAD2DEG = 180 / Math.PI;

export function quatIdentity() {
  return { w: 1, x: 0, y: 0, z: 0 };
}

export function quatMultiply(a, b) {
  return {
    w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  };
}

export function quatConjugate(q) {
  return { w: q.w, x: -q.x, y: -q.y, z: -q.z };
}

export function quatNormalize(q) {
  const m = Math.hypot(q.w, q.x, q.y, q.z);
  if (m < 1e-9) return quatIdentity();
  return { w: q.w / m, x: q.x / m, y: q.y / m, z: q.z / m };
}

// Intrinsic YXZ composition (q = qYaw ⊗ qPitch ⊗ qRoll) in the Y-up frame:
// yaw about +Y, pitch about +X, roll about +Z — the WebXR/Three.js head
// convention, and the exact inverse of quatToEuler below. Degrees in.
export function eulerToQuat(rollDeg, pitchDeg, yawDeg) {
  const r = rollDeg * DEG2RAD, p = pitchDeg * DEG2RAD, y = yawDeg * DEG2RAD;
  const cy = Math.cos(y * 0.5), sy = Math.sin(y * 0.5);
  const cp = Math.cos(p * 0.5), sp = Math.sin(p * 0.5);
  const cr = Math.cos(r * 0.5), sr = Math.sin(r * 0.5);
  return {
    w: cy * cp * cr + sy * sp * sr,
    x: cy * sp * cr + sy * cp * sr,
    y: sy * cp * cr - cy * sp * sr,
    z: cy * cp * sr - sy * sp * cr,
  };
}

// Back to display-friendly euler degrees (yaw/pitch/roll around Y/X/Z).
export function quatToEuler(q) {
  const { w, x, y, z } = q;
  const sinp = 2 * (w * x - y * z);
  const pitch = Math.abs(sinp) >= 1 ? Math.sign(sinp) * 90 : Math.asin(sinp) * RAD2DEG;
  const yaw = Math.atan2(2 * (w * y + x * z), 1 - 2 * (x * x + y * y)) * RAD2DEG;
  const roll = Math.atan2(2 * (w * z + x * y), 1 - 2 * (x * x + z * z)) * RAD2DEG;
  return { roll, pitch, yaw };
}

// Heading-only component of q: the "twist" around the world Y (up) axis.
// Used for recentering — removing only the twist keeps gravity alignment
// intact, so a recenter taken while pitched doesn't tilt the yaw axis.
export function yawTwist(q) {
  const m = Math.hypot(q.w, q.y);
  if (m < 1e-6) return quatIdentity(); // pure 180° pitch/roll — no defined heading
  return { w: q.w / m, x: 0, y: q.y / m, z: 0 };
}

// Rotate vector v by quaternion q.
export function quatRotateVector(q, v) {
  const { w, x, y, z } = q;
  const tx = 2 * (y * v.z - z * v.y);
  const ty = 2 * (z * v.x - x * v.z);
  const tz = 2 * (x * v.y - y * v.x);
  return {
    x: v.x + w * tx + (y * tz - z * ty),
    y: v.y + w * ty + (z * tx - x * tz),
    z: v.z + w * tz + (x * ty - y * tx),
  };
}

export function fmt(n, digits = 2) {
  return (n >= 0 ? ' ' : '') + n.toFixed(digits);
}
