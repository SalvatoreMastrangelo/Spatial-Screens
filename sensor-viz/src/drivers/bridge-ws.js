// WebSocket client for the native bridge daemon (bridge/), which links the
// official VITURE SDK v2.0 and streams the full sensor set: 6DoF pose, raw
// IMU (gyro/accel/mag + temperature), VSync, device state and firmware info.

const DEFAULT_URL = 'ws://localhost:8765';

export class BridgeClient {
  constructor(url = DEFAULT_URL) {
    this.url = url;
    this.ws = null;
    this.connected = false;
    this.listeners = {};
    this._shouldReconnect = false;
    this._retryMs = 1000;
  }

  on(event, cb) {
    (this.listeners[event] ??= new Set()).add(cb);
    return () => this.listeners[event].delete(cb);
  }

  _emit(event, data) {
    this.listeners[event]?.forEach((cb) => cb(data));
  }

  connect() {
    this._shouldReconnect = true;
    return new Promise((resolve, reject) => {
      let settled = false;
      const ws = new WebSocket(this.url);
      this.ws = ws;

      ws.onopen = () => {
        this.connected = true;
        this._retryMs = 1000;
      };

      ws.onmessage = (ev) => {
        let msg;
        try { msg = JSON.parse(ev.data); } catch { return; }
        this._handle(msg);
        if (!settled && msg.type === 'hello') {
          settled = true;
          resolve(this.info);
        }
      };

      ws.onerror = () => {
        if (!settled) {
          settled = true;
          reject(new Error(`Bridge not reachable at ${this.url} — is viture-bridge running?`));
        }
      };

      ws.onclose = () => {
        const was = this.connected;
        this.connected = false;
        this.info = null; // next hello after a reconnect re-announces
        if (was) this._emit('disconnected', {});
        if (this._shouldReconnect && was) {
          setTimeout(() => this.connect().catch(() => {}), this._retryMs);
          this._retryMs = Math.min(this._retryMs * 2, 15000);
        }
      };
    });
  }

  disconnect() {
    this._shouldReconnect = false;
    this.ws?.close();
    this.ws = null;
    this.connected = false;
  }

  send(obj) {
    if (this.ws?.readyState === WebSocket.OPEN) this.ws.send(JSON.stringify(obj));
  }

  resetPose() { this.send({ type: 'reset_pose' }); }

  _handle(msg) {
    const t = performance.now();
    switch (msg.type) {
    case 'hello': {
      // The bridge re-broadcasts hello every 2 s as a heartbeat for clients
      // that join late — only the first one of a socket session means
      // "connected".
      const firstHello = !this.info;
      this.info = {
        source: 'bridge',
        model: msg.model ?? 'VITURE',
        productName: msg.market_name ?? msg.model,
        vendorId: 0x35ca,
        productId: msg.pid,
        firmware: msg.firmware,
        deviceType: msg.device_type, // 0 gen1, 1 gen2, 2 carina (Luma Ultra)
      };
      if (firstHello) this._emit('connected', this.info);
      break;
    }
    case 'pose':
      // Carina 6DoF: OpenGL/EUS coords, position in meters.
      this._emit('pose', {
        t,
        devT: msg.t,
        position: { x: msg.px, y: msg.py, z: msg.pz },
        quat: { w: msg.qw, x: msg.qx, y: msg.qy, z: msg.qz },
      });
      break;
    case 'euler':
      this._emit('orientation', {
        t,
        devT: msg.t,
        euler: { roll: msg.roll, pitch: msg.pitch, yaw: msg.yaw },
        quat: { w: msg.qw, x: msg.qx, y: msg.qy, z: msg.qz },
      });
      break;
    case 'imu':
      this._emit('imu', {
        t,
        devT: msg.t,
        gyro: msg.g,   // [x,y,z] rad/s
        accel: msg.a,  // [x,y,z] m/s^2
        mag: msg.m,    // [x,y,z] uT (Luma series)
        temp: msg.temp,
        values: msg.values, // raw device-defined float array (carina)
      });
      break;
    case 'vsync':
      this._emit('vsync', { t, devT: msg.t });
      break;
    case 'state':
      this._emit('state', msg); // {id, name, value}
      break;
    case 'log':
      this._emit('log', msg);
      break;
    case 'app':
      // spatial-screens status (fps, tracking, screen placement).
      this._emit('app', msg);
      break;
    default:
      this._emit('log', { level: 'debug', text: `unknown msg ${msg.type}` });
    }
  }
}
