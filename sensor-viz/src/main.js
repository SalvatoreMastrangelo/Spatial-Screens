import { VitureWebHID } from './drivers/viture-webhid.js';
import { BridgeClient } from './drivers/bridge-ws.js';
import { state } from './state.js';
import { Scene3D } from './ui/scene3d.js';
import { FlatView } from './ui/flatview.js';
import { Scope } from './ui/charts.js';
import {
  showDeviceInfo, showDeviceState, updateCaps, updateStats,
  updateReadouts, renderEventLog, setStatus, setDofBadge,
  showAppPanel, hideAppPanel,
} from './ui/panels.js';

const $ = (id) => document.getElementById(id);

// WebGL can legitimately fail (context exhaustion, NVIDIA/Optimus quirks,
// remote desktops). Fall back to the 2D instrument view — same interface —
// so the spatial panel and all controls keep working.
let scene = null;
try {
  scene = new Scene3D($('view3d'));
} catch (err) {
  console.error('WebGL unavailable, using 2D fallback view:', err);
  scene = new FlatView($('view3d'));
}

const scopes = [
  new Scope($('chart-euler'), state.series.euler, { range: [-180, 180] }),
  new Scope($('chart-pos'), state.series.pos),
  new Scope($('chart-gyro'), state.series.gyro),
  new Scope($('chart-accel'), state.series.accel),
];

let driver = null;

state.onEvent = () => renderEventLog(state);

// ---- MCU event decoding (WebHID path) ------------------------------------

const MCU_EVENT_NAMES = {
  // Values observed via the SDK GlassStateCallback ids; HID event ids are the
  // raw command ids — shown in hex when unknown.
};

function describeMcuEvent(ev) {
  const name = MCU_EVENT_NAMES[ev.cmdId];
  const base = name ?? `MCU event 0x${ev.cmdId.toString(16).padStart(4, '0')}`;
  return `${base} [${ev.hex}]`;
}

// ---- driver wiring ---------------------------------------------------------

function wireCommon(d) {
  d.on('connected', (info) => {
    state.source = info.source;
    state.info = info;
    setStatus(`connected — ${info.model}`, 'ok');
    setDofBadge(info.source === 'bridge' && info.deviceType === 2 ? '6DoF' : '3DoF');
    showDeviceInfo(info);
    state.log(`connected via ${info.source}: ${info.model}`, 'ok');
    $('btn-recenter').disabled = false;
    $('btn-level').disabled = false;
    $('btn-pause').disabled = false;
    $('btn-disconnect').disabled = false;
    if (state.pitchTrimDeg) state.log(`applying saved pitch trim ${state.pitchTrimDeg.toFixed(1)}°`, 'dim');
    state.caps.events = true;
  });
  d.on('disconnected', () => {
    setStatus('disconnected', 'off');
    setDofBadge(null);
    state.log('device disconnected', 'warn');
    teardown();
  });
  d.on('orientation', (f) => state.ingestOrientation(f));
}

async function connectHID() {
  if (!VitureWebHID.isSupported()) {
    $('hid-warning').hidden = false;
    return;
  }
  disconnectCurrent();
  const d = new VitureWebHID();
  wireCommon(d);
  d.on('mcu-event', (ev) => state.log(describeMcuEvent(ev), 'dim'));
  try {
    setStatus('connecting…', 'busy');
    await d.connect();
    driver = d;
  } catch (err) {
    setStatus('disconnected', 'off');
    state.log(`WebHID connect failed: ${err.message}`, 'err');
  }
}

async function connectBridge() {
  disconnectCurrent();
  const d = new BridgeClient();
  wireCommon(d);
  d.on('pose', (f) => state.ingestPose(f));
  d.on('imu', (f) => state.ingestImu(f));
  d.on('vsync', (f) => state.ingestVsync(f));
  d.on('state', (msg) => {
    const map = { 0: 'brightness', 1: 'volume', 2: 'dispmode', 3: 'film', 4: 'dof' };
    const key = msg.name ?? map[msg.id] ?? `state_${msg.id}`;
    const changed = state.device[key] !== msg.value && state.device[key] !== undefined;
    const first = state.device[key] === undefined;
    state.device[key] = msg.value;
    showDeviceState(state.device);
    // The bridge re-announces unchanged state for late joiners — only log
    // genuine changes (and the first snapshot quietly).
    if (changed) state.log(`state: ${key} → ${msg.value}`, 'warn');
    else if (first) state.log(`state: ${key} = ${msg.value}`, 'dim');
  });
  d.on('log', (msg) => state.log(`[bridge] ${msg.text}`, 'dim'));
  d.on('app', (msg) => showAppPanel(msg));
  try {
    setStatus('connecting to bridge…', 'busy');
    await d.connect();
    driver = d;
    if (d.info?.firmware) $('dev-fw').textContent = d.info.firmware;
  } catch (err) {
    setStatus('disconnected', 'off');
    state.log(err.message, 'err');
  }
}

function disconnectCurrent() {
  driver?.disconnect();
  driver = null;
  teardown();
}

function teardown() {
  state.reset();
  scene?.clearTrail();
  showDeviceInfo(null);
  showDeviceState({});
  hideAppPanel();
  updateCaps(state.caps);
  $('btn-recenter').disabled = true;
  $('btn-level').disabled = true;
  $('btn-pause').disabled = true;
  $('btn-disconnect').disabled = true;
}

// ---- controls ---------------------------------------------------------------

$('btn-hid').addEventListener('click', connectHID);
$('btn-bridge').addEventListener('click', connectBridge);
$('btn-disconnect').addEventListener('click', () => {
  disconnectCurrent();
  setStatus('disconnected', 'off');
  setDofBadge(null);
});
// Recenter: when the bridge is connected, spatial-screens owns the pose —
// request its full VIO-reset recenter (it also re-places the virtual
// screen) and drop the local offsets so the re-zeroed stream isn't
// recentered twice. WebHID has no app side: recenter locally as before.
$('btn-recenter').addEventListener('click', () => {
  if (driver?.resetPose) {
    driver.resetPose();
    state.clearRecenter();
    state.log('pose reset requested (app-side recenter)');
  } else {
    state.recenter();
  }
  scene?.clearTrail();
});
$('btn-level').addEventListener('click', () => state.level());
$('btn-pause').addEventListener('click', (e) => {
  state.paused = !state.paused;
  e.target.textContent = state.paused ? 'Resume' : 'Pause';
});
$('btn-clearlog').addEventListener('click', () => {
  state.events.length = 0;
  renderEventLog(state);
});

// ---- render loop --------------------------------------------------------------

let lastPanelUpdate = 0;
function frame(now) {
  scene?.update(state);
  if (!state.paused) for (const s of scopes) s.draw(now);
  if (now - lastPanelUpdate > 250) {
    lastPanelUpdate = now;
    updateStats(state);
    updateReadouts(state);
    updateCaps(state.caps);
  }
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

setStatus('disconnected', 'off');
state.log('ready — connect via WebHID (Chrome) or start the native bridge for full 6DoF');
