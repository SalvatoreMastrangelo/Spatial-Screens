// Central sensor store: normalized frames from either driver, recentering,
// ring-buffered time series for the charts, and rate statistics.

import {
  quatIdentity, quatMultiply, quatConjugate, quatToEuler,
  yawTwist, quatRotateVector, eulerToQuat,
} from './math.js';

const TRIM_KEY = 'viture-pitch-trim-deg';

const WINDOW_MS = 15000;

class Series {
  constructor(labels, colors) {
    this.labels = labels;
    this.colors = colors;
    this.samples = []; // {t, v: [..]}
  }
  push(t, values) {
    this.samples.push({ t, v: values });
    const cutoff = t - WINDOW_MS;
    while (this.samples.length && this.samples[0].t < cutoff) this.samples.shift();
  }
  last() {
    return this.samples[this.samples.length - 1] ?? null;
  }
}

class RateMeter {
  constructor() { this.times = []; this.jitterEma = 0; }
  tick(t) {
    const prev = this.times[this.times.length - 1];
    if (prev !== undefined) {
      const dt = t - prev;
      this.jitterEma = this.jitterEma * 0.95 + Math.abs(dt - this.meanDt()) * 0.05;
    }
    this.times.push(t);
    const cutoff = t - 2000;
    while (this.times.length && this.times[0] < cutoff) this.times.shift();
  }
  meanDt() {
    if (this.times.length < 2) return 0;
    return (this.times[this.times.length - 1] - this.times[0]) / (this.times.length - 1);
  }
  hz() {
    const dt = this.meanDt();
    return dt > 0 ? 1000 / dt : 0;
  }
}

export const state = {
  source: null,          // 'webhid' | 'bridge'
  info: null,
  paused: false,

  // live pose (post-recenter)
  quat: quatIdentity(),
  euler: { roll: 0, pitch: 0, yaw: 0 },
  position: null,        // {x,y,z} meters, only with 6DoF
  hasPosition: false,

  // recenter references
  _oriOffset: quatIdentity(),
  _posOffset: { x: 0, y: 0, z: 0 },
  _rawQuat: quatIdentity(),
  _rawPos: { x: 0, y: 0, z: 0 },

  // Constant pitch offset of the sensor rig vs. the wearer's line of sight
  // (the Luma Ultra's tracking cameras are angled downward, so the VIO pose
  // carries a fixed pitch). Applied as a body-frame post-rotation, which
  // cannot tilt the yaw axis. Degrees, persisted across sessions.
  pitchTrimDeg: Number(localStorage.getItem(TRIM_KEY) ?? 0) || 0,

  packets: 0,

  series: {
    euler: new Series(['roll', 'pitch', 'yaw'], ['#ffb454', '#59c2ff', '#aad94c']),
    pos: new Series(['x', 'y', 'z'], ['#f07178', '#59c2ff', '#aad94c']),
    gyro: new Series(['x', 'y', 'z'], ['#f07178', '#59c2ff', '#aad94c']),
    accel: new Series(['x', 'y', 'z'], ['#f07178', '#59c2ff', '#aad94c']),
  },

  rates: {
    orientation: new RateMeter(),
    imu: new RateMeter(),
    pose: new RateMeter(),
    vsync: new RateMeter(),
  },

  caps: { orientation: false, position: false, gyro: false, accel: false, vsync: false, events: false },
  device: {},            // brightness, volume, dispmode, film, firmware...
  events: [],            // rolling log [{t, text, cls}]

  onEvent: null,         // UI hook

  log(text, cls = '') {
    this.events.push({ t: new Date(), text, cls });
    if (this.events.length > 200) this.events.shift();
    this.onEvent?.();
  },

  recenter() {
    // Remove only the heading (yaw twist), never pitch/roll: zeroing the full
    // orientation while the glasses are pitched tilts the yaw axis and makes
    // horizontal turns couple into vertical motion.
    this._oriOffset = yawTwist(this._rawQuat);
    this._posOffset = { ...this._rawPos };
    this.log('recentered (heading zeroed' + (this.hasPosition ? ' + position zeroed' : '') + ')');
  },

  // For app-side recenters (spatial-screens reset_pose): the incoming pose
  // re-zeros at the source, so local offsets must drop to identity or the
  // view double-recenters.
  clearRecenter() {
    this._oriOffset = quatIdentity();
    this._posOffset = { x: 0, y: 0, z: 0 };
  },

  // Capture the current pitch as the sensor mounting offset: call while
  // looking straight ahead at the horizon.
  level() {
    const untrimmed = quatMultiply(quatConjugate(this._oriOffset), this._rawQuat);
    const measured = quatToEuler(untrimmed).pitch;
    this.pitchTrimDeg = -measured;
    localStorage.setItem(TRIM_KEY, String(this.pitchTrimDeg));
    this.log(`leveled: pitch trim set to ${this.pitchTrimDeg.toFixed(1)}° (persisted)`);
  },

  _applyTrim(q) {
    if (!this.pitchTrimDeg) return q;
    return quatMultiply(q, eulerToQuat(0, this.pitchTrimDeg, 0));
  },

  ingestOrientation(f) {
    this._rawQuat = f.quat;
    this.quat = this._applyTrim(quatMultiply(quatConjugate(this._oriOffset), f.quat));
    this.euler = quatToEuler(this.quat);
    this.caps.orientation = true;
    this.packets = f.packets ?? this.packets + 1;
    this.rates.orientation.tick(f.t);
    if (!this.paused) this.series.euler.push(f.t, [this.euler.roll, this.euler.pitch, this.euler.yaw]);
  },

  ingestPose(f) {
    this._rawQuat = f.quat;
    this._rawPos = f.position;
    this.quat = this._applyTrim(quatMultiply(quatConjugate(this._oriOffset), f.quat));
    this.euler = quatToEuler(this.quat);
    // Express position in the recentered frame too, so the trail stays
    // consistent with the rotated view.
    this.position = quatRotateVector(quatConjugate(this._oriOffset), {
      x: f.position.x - this._posOffset.x,
      y: f.position.y - this._posOffset.y,
      z: f.position.z - this._posOffset.z,
    });
    this.hasPosition = true;
    this.caps.orientation = true;
    this.caps.position = true;
    this.packets++;
    this.rates.pose.tick(f.t);
    if (!this.paused) {
      this.series.euler.push(f.t, [this.euler.roll, this.euler.pitch, this.euler.yaw]);
      this.series.pos.push(f.t, [this.position.x, this.position.y, this.position.z]);
    }
  },

  ingestImu(f) {
    this.rates.imu.tick(f.t);
    if (f.gyro) { this.caps.gyro = true; if (!this.paused) this.series.gyro.push(f.t, f.gyro); }
    if (f.accel) { this.caps.accel = true; if (!this.paused) this.series.accel.push(f.t, f.accel); }
  },

  ingestVsync(f) {
    this.caps.vsync = true;
    this.rates.vsync.tick(f.t);
  },

  reset() {
    this.source = null;
    this.info = null;
    this.position = null;
    this.hasPosition = false;
    this.quat = quatIdentity();
    this._oriOffset = quatIdentity();
    this._posOffset = { x: 0, y: 0, z: 0 };
    this.packets = 0;
    for (const k of Object.keys(this.caps)) this.caps[k] = false;
    this.device = {};
    for (const s of Object.values(this.series)) s.samples.length = 0;
  },
};
