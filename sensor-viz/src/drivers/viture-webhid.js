// Direct WebHID driver for VITURE XR glasses.
//
// Protocol: reverse-engineered VITURE HID framing (see docs/specs and
// reference/viture-webxr-extension/VITURE_PROTOCOL.md). The glasses expose two
// vendor HID interfaces (usage page 0xFF00): an MCU interface for commands and
// events, and an IMU interface streaming orientation packets.
//
// This path yields 3DoF orientation (euler) plus MCU events. Full 6DoF pose,
// raw gyro/accel/mag and temperature come from the native bridge driver.

import { eulerToQuat, quatNormalize } from '../math.js';

const VENDOR_ID = 0x35ca;

export const PRODUCT_IDS = {
  'VITURE One': [0x1011, 0x1013, 0x1017],
  'VITURE One Lite': [0x1015, 0x101b],
  'VITURE Pro': [0x1019, 0x101d],
  'VITURE Luma': [0x1131],
  'VITURE Luma Pro': [0x1121, 0x1141],
  // 0x1101/0x1104 is the glasses' main USB device; the two vendor HID
  // interfaces (MCU + IMU) enumerate on the companion 0x1102 device
  // ("VITURE Microphone" composite) — confirmed on real hardware.
  'VITURE Luma Ultra': [0x1101, 0x1104, 0x1102],
  'VITURE Luma Cyber': [0x1151],
  'VITURE Beast': [0x1201],
};

const ALL_PIDS = Object.values(PRODUCT_IDS).flat();

function modelForPid(pid) {
  for (const [model, pids] of Object.entries(PRODUCT_IDS)) {
    if (pids.includes(pid)) return model;
  }
  return `VITURE (0x${pid.toString(16)})`;
}

// CRC-16-CCITT, poly 0x1021, init 0xFFFF — used by the VITURE packet framing.
const CRC_TABLE = new Uint16Array(256);
for (let i = 0; i < 256; i++) {
  let crc = i << 8;
  for (let j = 0; j < 8; j++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  CRC_TABLE[i] = crc;
}
function crc16(bytes, start, len) {
  let crc = 0xffff;
  for (let i = start; i < start + len; i++) {
    crc = ((crc << 8) ^ CRC_TABLE[((crc >> 8) ^ bytes[i]) & 0xff]) & 0xffff;
  }
  return crc;
}

const HDR_IMU = 0xfc;
const HDR_MCU_RESPONSE = 0xfd;
const HDR_MCU_COMMAND = 0xfe;
const CMD_IMU_CONTROL = 0x15;
const PAYLOAD_OFFSET = 0x12;

// Big-endian float stored byte-reversed in IMU packets.
function beFloat(bytes, off) {
  const dv = new DataView(new ArrayBuffer(4));
  dv.setUint8(0, bytes[off + 3]);
  dv.setUint8(1, bytes[off + 2]);
  dv.setUint8(2, bytes[off + 1]);
  dv.setUint8(3, bytes[off]);
  return dv.getFloat32(0, true);
}

export class VitureWebHID {
  constructor() {
    this.devices = [];
    this.connected = false;
    this.listeners = {};
    this._msgCounter = 0;
    this._packetCount = 0;
    this._lastImuT = 0;
  }

  static isSupported() {
    return typeof navigator !== 'undefined' && 'hid' in navigator;
  }

  on(event, cb) {
    (this.listeners[event] ??= new Set()).add(cb);
    return () => this.listeners[event].delete(cb);
  }

  _emit(event, data) {
    this.listeners[event]?.forEach((cb) => cb(data));
  }

  async connect() {
    if (!VitureWebHID.isSupported()) {
      throw new Error('WebHID not supported in this browser (use Chrome/Edge, or the bridge)');
    }
    const filters = ALL_PIDS.map((productId) => ({ vendorId: VENDOR_ID, productId }));
    const picked = await navigator.hid.requestDevice({ filters });
    if (picked.length === 0) throw new Error('No VITURE device selected');

    // The picker returns one entry, but both HID interfaces (MCU + IMU) are
    // authorized for the same physical device — open them all.
    const all = await navigator.hid.getDevices();
    this.devices = all.filter(
      (d) => d.vendorId === VENDOR_ID && ALL_PIDS.includes(d.productId)
    );
    for (const dev of this.devices) {
      if (!dev.opened) await dev.open();
      dev.addEventListener('inputreport', (ev) => this._onReport(ev));
    }
    if (this.devices.length === 0) throw new Error('Could not open any VITURE HID interface');

    this.connected = true;
    const primary = this.devices[0];
    this.info = {
      source: 'webhid',
      model: modelForPid(primary.productId),
      productName: primary.productName,
      vendorId: VENDOR_ID,
      productId: primary.productId,
      interfaces: this.devices.length,
    };

    await this.setImuEnabled(true);
    this._emit('connected', this.info);

    navigator.hid.addEventListener('disconnect', (ev) => {
      if (this.devices.includes(ev.device)) this.disconnect();
    });
    return this.info;
  }

  async disconnect() {
    if (!this.connected) return;
    this.connected = false;
    try { await this.setImuEnabled(false); } catch { /* device may be gone */ }
    for (const dev of this.devices) {
      try { if (dev.opened) await dev.close(); } catch { /* ignore */ }
    }
    this.devices = [];
    this._emit('disconnected', {});
  }

  buildCommand(cmdId, dataBytes = new Uint8Array(0)) {
    const packetLen = PAYLOAD_OFFSET + dataBytes.length + 1;
    const p = new Uint8Array(packetLen);
    p[0] = 0xff;
    p[1] = HDR_MCU_COMMAND;
    const payloadLen = 8 + 2 + 2 + dataBytes.length + 1;
    p[4] = payloadLen & 0xff;
    p[5] = (payloadLen >> 8) & 0xff;
    p[14] = cmdId & 0xff;
    p[15] = (cmdId >> 8) & 0xff;
    this._msgCounter = (this._msgCounter + 1) & 0xffff;
    p[16] = this._msgCounter & 0xff;
    p[17] = (this._msgCounter >> 8) & 0xff;
    p.set(dataBytes, PAYLOAD_OFFSET);
    p[packetLen - 1] = 0x03;
    const crc = crc16(p, 4, packetLen - 4);
    p[2] = (crc >> 8) & 0xff;
    p[3] = crc & 0xff;
    return p;
  }

  async sendCommand(cmdId, dataBytes) {
    const packet = this.buildCommand(cmdId, dataBytes);
    let sent = false;
    for (const dev of this.devices) {
      try {
        await dev.sendReport(0x00, packet);
        sent = true;
      } catch {
        // The IMU-only interface rejects output reports; that's expected.
      }
    }
    if (!sent) throw new Error(`No interface accepted command 0x${cmdId.toString(16)}`);
  }

  async setImuEnabled(enabled) {
    await this.sendCommand(CMD_IMU_CONTROL, new Uint8Array([enabled ? 1 : 0]));
  }

  _onReport(event) {
    const dv = event.data;
    const bytes = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
    if (bytes.length < PAYLOAD_OFFSET || bytes[0] !== 0xff) return;
    this._packetCount++;

    if (bytes[1] === HDR_IMU) this._parseImu(bytes);
    else if (bytes[1] === HDR_MCU_RESPONSE) this._parseMcu(bytes);
  }

  _parseImu(bytes) {
    const t = performance.now();
    const roll = beFloat(bytes, PAYLOAD_OFFSET);
    const pitch = beFloat(bytes, PAYLOAD_OFFSET + 4);
    const yaw = beFloat(bytes, PAYLOAD_OFFSET + 8);
    if (![roll, pitch, yaw].every((v) => Number.isFinite(v) && Math.abs(v) <= 360)) return;

    // Device euler → app convention (see VITURE_PROTOCOL.md axis mapping).
    const mapped = { yaw: -roll, roll: -pitch, pitch: yaw };
    let quat = eulerToQuat(mapped.roll, mapped.pitch, mapped.yaw);

    // Some firmwares also append a quaternion after the euler block; prefer it
    // when it looks valid.
    const payloadLen = bytes[4] | (bytes[5] << 8);
    if (payloadLen >= 13 + 16 && bytes.length >= PAYLOAD_OFFSET + 28) {
      const qw = beFloat(bytes, PAYLOAD_OFFSET + 12);
      const qx = beFloat(bytes, PAYLOAD_OFFSET + 16);
      const qy = beFloat(bytes, PAYLOAD_OFFSET + 20);
      const qz = beFloat(bytes, PAYLOAD_OFFSET + 24);
      const mag = Math.hypot(qw, qx, qy, qz);
      if (Number.isFinite(mag) && mag > 0.8 && mag < 1.2) {
        quat = quatNormalize({ w: qw, x: qx, y: qy, z: qz });
      }
    }

    // Timestamp from the device (ms domain unknown — expose for rate stats).
    const devT = bytes[6] | (bytes[7] << 8) | (bytes[8] << 16) | (bytes[9] << 24);

    this._emit('orientation', {
      t,
      devT,
      euler: { roll: mapped.roll, pitch: mapped.pitch, yaw: mapped.yaw },
      quat,
      packets: this._packetCount,
    });
  }

  _parseMcu(bytes) {
    const cmdId = bytes[14] | (bytes[15] << 8);
    const payloadLen = bytes[4] | (bytes[5] << 8);
    const dataLen = Math.max(0, Math.min(payloadLen - 13, bytes.length - PAYLOAD_OFFSET - 1));
    const data = bytes.slice(PAYLOAD_OFFSET, PAYLOAD_OFFSET + dataLen);
    this._emit('mcu-event', {
      t: performance.now(),
      cmdId,
      data: Array.from(data),
      hex: Array.from(data, (b) => b.toString(16).padStart(2, '0')).join(' '),
    });
  }
}
