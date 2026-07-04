// DOM panels: device info, capability matrix, stats chips, event log.

import { fmt } from '../math.js';

const $ = (id) => document.getElementById(id);

const DISPLAY_MODES = {
  0x31: '1920×1080 @60', 0x32: '3840×1080 @60 (SBS)', 0x33: '1920×1080 @90',
  0x34: '1920×1080 @120', 0x35: '3840×1080 @90 (SBS)', 0x36: '1920×1080 60→120',
  0x41: '1920×1200 @60', 0x42: '3840×1200 @60 (SBS)', 0x43: '1920×1200 @90',
  0x44: '1920×1200 @120', 0x45: '3840×1200 @90 (SBS)', 0x46: '1920×1200 60→120',
  0x51: 'ultrawide 60→120', 0x61: 'side mode @60',
};

export function showDeviceInfo(info) {
  $('dev-model').textContent = info?.model ?? '—';
  $('dev-product').textContent = info?.productName ?? '—';
  $('dev-ids').textContent = info
    ? `35ca:${info.productId?.toString(16).padStart(4, '0') ?? '????'}`
    : '—';
  $('dev-fw').textContent = info?.firmware ?? '—';
  $('dev-source').textContent = info
    ? (info.source === 'bridge' ? `bridge (native SDK${info.deviceType === 2 ? ', 6DoF' : ''})` : 'WebHID (3DoF)')
    : '—';
}

export function showDeviceState(device) {
  $('dev-brightness').textContent = device.brightness ?? '—';
  $('dev-volume').textContent = device.volume ?? '—';
  $('dev-dispmode').textContent = device.dispmode != null
    ? (DISPLAY_MODES[device.dispmode] ?? `0x${device.dispmode.toString(16)}`)
    : '—';
  $('dev-film').textContent = device.film ?? '—';
}

export function updateCaps(caps) {
  document.querySelectorAll('#sensor-matrix .cap').forEach((el) => {
    el.classList.toggle('on', !!caps[el.dataset.k]);
  });
}

export function updateStats(state) {
  $('stat-ori-hz').textContent = state.rates.orientation.hz().toFixed(0);
  $('stat-imu-hz').textContent = state.rates.imu.hz().toFixed(0);
  $('stat-pose-hz').textContent = state.rates.pose.hz().toFixed(0);
  $('stat-jitter').textContent = state.rates.orientation.jitterEma.toFixed(1);
  $('stat-packets').textContent = state.packets.toLocaleString();
}

export function updateReadouts(state) {
  const e = state.euler;
  $('val-euler').textContent = `r${fmt(e.roll, 1)}  p${fmt(e.pitch, 1)}  y${fmt(e.yaw, 1)}`;
  const q = state.quat;
  let pose = `q[${fmt(q.w)} ${fmt(q.x)} ${fmt(q.y)} ${fmt(q.z)}]`;
  if (state.hasPosition && state.position) {
    const p = state.position;
    pose += `  p[${fmt(p.x)} ${fmt(p.y)} ${fmt(p.z)}]m`;
    $('val-pos').textContent = `x${fmt(p.x)}  y${fmt(p.y)}  z${fmt(p.z)}`;
  } else {
    $('val-pos').textContent = 'no 6DoF source';
  }
  $('pose-readout').textContent = pose;

  const g = state.series.gyro.last();
  $('val-gyro').textContent = g ? g.v.map((v) => fmt(v, 2)).join('  ') : '—';
  const a = state.series.accel.last();
  $('val-accel').textContent = a ? a.v.map((v) => fmt(v, 2)).join('  ') : '—';
}

export function renderEventLog(state) {
  const el = $('event-log');
  el.innerHTML = state.events
    .slice(-100)
    .map((ev) => `<div class="ev ${ev.cls}"><span>${ev.t.toLocaleTimeString()}</span> ${escapeHtml(ev.text)}</div>`)
    .join('');
  el.scrollTop = el.scrollHeight;
}

function escapeHtml(s) {
  return s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

export function setStatus(text, cls) {
  const pill = $('status-pill');
  pill.textContent = text;
  pill.className = `pill ${cls}`;
}

export function setDofBadge(mode) {
  const b = $('dof-badge');
  if (!mode) { b.hidden = true; return; }
  b.hidden = false;
  b.textContent = mode;
  b.classList.toggle('six', mode === '6DoF');
}

export function showAppPanel(msg) {
  $('app-card').hidden = false;
  $('app-fps').textContent = msg.fps != null ? msg.fps.toFixed(0) : '—';
  $('app-sixdof').textContent = msg.sixdof ? '6DoF LIVE' : 'orientation only';
  $('app-anchored').textContent = msg.anchored ? 'yes' : 'no';
  $('app-distance').textContent = msg.distance != null ? `${fmt(msg.distance, 2)} m` : '—';
  $('app-size').textContent = msg.size != null ? `${msg.size}"` : '—';
  $('app-backend').textContent = msg.backend ?? '—';
  $('app-mode').textContent = msg.direct ? 'direct' : 'window';
  $('app-rss').textContent = msg.rss != null ? `${msg.rss} MB` : '—';
}

export function hideAppPanel() {
  $('app-card').hidden = true;
}
