// 2D canvas fallback for the spatial view when WebGL is unavailable.
// Left: attitude indicator (artificial horizon + roll) with heading arc.
// Right: top-down position map (x/z, meters) with trail — 6DoF only.
// Implements the same interface as Scene3D: update(state), clearTrail().

const TRAIL_MAX = 2000;

export class FlatView {
  constructor(container) {
    this.container = container;
    container.innerHTML = '';
    this.canvas = document.createElement('canvas');
    this.canvas.style.cssText = 'width:100%;height:100%;display:block';
    container.appendChild(this.canvas);
    this.ctx = this.canvas.getContext('2d');
    this.trail = [];
    new ResizeObserver(() => requestAnimationFrame(() => this._fit())).observe(container);
    this._fit();
  }

  _fit() {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = this.container.clientWidth || 600;
    const h = this.container.clientHeight || 380;
    this.canvas.width = w * dpr;
    this.canvas.height = h * dpr;
    this.dpr = dpr;
  }

  clearTrail() {
    this.trail.length = 0;
  }

  update(state) {
    const { ctx } = this;
    const W = this.canvas.width, H = this.canvas.height, dpr = this.dpr;
    ctx.clearRect(0, 0, W, H);

    if (state.hasPosition && state.position) {
      this.trail.push([state.position.x, state.position.z]);
      if (this.trail.length > TRAIL_MAX) this.trail.shift();
    }

    const half = W / 2;
    this._drawAttitude(ctx, 0, 0, half, H, state, dpr);
    this._drawMap(ctx, half, 0, W - half, H, state, dpr);

    ctx.strokeStyle = 'rgba(120,140,180,0.2)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(half, 8 * dpr);
    ctx.lineTo(half, H - 8 * dpr);
    ctx.stroke();

    ctx.fillStyle = 'rgba(160,175,205,0.4)';
    ctx.font = `${10 * dpr}px ui-monospace, monospace`;
    ctx.textAlign = 'right';
    ctx.fillText('2D fallback — WebGL unavailable', W - 10 * dpr, H - 8 * dpr);
    ctx.textAlign = 'left';
  }

  _drawAttitude(ctx, x0, y0, w, h, state, dpr) {
    const cx = x0 + w / 2, cy = y0 + h / 2;
    const R = Math.min(w, h) * 0.36;
    const { roll, pitch, yaw } = state.euler;

    // horizon disc, clipped to a circle
    ctx.save();
    ctx.beginPath();
    ctx.arc(cx, cy, R, 0, Math.PI * 2);
    ctx.clip();
    ctx.translate(cx, cy);
    ctx.rotate(-roll * Math.PI / 180);
    const pitchPx = (pitch / 45) * R; // 45° pitch = full radius
    // sky
    ctx.fillStyle = '#16283a';
    ctx.fillRect(-R * 1.6, -R * 1.6 + pitchPx, R * 3.2, R * 1.6);
    // ground
    ctx.fillStyle = '#1a220e';
    ctx.fillRect(-R * 1.6, pitchPx, R * 3.2, R * 1.6);
    // horizon line
    ctx.strokeStyle = '#aad94c';
    ctx.lineWidth = 1.5 * dpr;
    ctx.beginPath();
    ctx.moveTo(-R * 1.6, pitchPx);
    ctx.lineTo(R * 1.6, pitchPx);
    ctx.stroke();
    // pitch ladder every 15°
    ctx.strokeStyle = 'rgba(205,214,228,0.4)';
    ctx.fillStyle = 'rgba(205,214,228,0.5)';
    ctx.font = `${9 * dpr}px ui-monospace, monospace`;
    ctx.lineWidth = dpr;
    for (let p = -60; p <= 60; p += 15) {
      if (p === 0) continue;
      const y = pitchPx - (p / 45) * R;
      const lw = R * 0.25;
      ctx.beginPath();
      ctx.moveTo(-lw, y);
      ctx.lineTo(lw, y);
      ctx.stroke();
      ctx.fillText(String(p), lw + 4 * dpr, y + 3 * dpr);
    }
    ctx.restore();

    // bezel
    ctx.strokeStyle = '#273044';
    ctx.lineWidth = 2 * dpr;
    ctx.beginPath();
    ctx.arc(cx, cy, R, 0, Math.PI * 2);
    ctx.stroke();

    // fixed aircraft reference
    ctx.strokeStyle = '#ffb454';
    ctx.lineWidth = 2 * dpr;
    ctx.beginPath();
    ctx.moveTo(cx - R * 0.35, cy);
    ctx.lineTo(cx - R * 0.1, cy);
    ctx.moveTo(cx + R * 0.1, cy);
    ctx.lineTo(cx + R * 0.35, cy);
    ctx.moveTo(cx, cy - 3 * dpr);
    ctx.lineTo(cx, cy + 3 * dpr);
    ctx.stroke();

    // heading arc under the horizon ball
    const hy = cy + R + 18 * dpr;
    ctx.strokeStyle = 'rgba(120,140,180,0.3)';
    ctx.lineWidth = dpr;
    ctx.beginPath();
    ctx.moveTo(cx - R, hy);
    ctx.lineTo(cx + R, hy);
    ctx.stroke();
    ctx.fillStyle = 'rgba(205,214,228,0.6)';
    ctx.font = `${9 * dpr}px ui-monospace, monospace`;
    ctx.textAlign = 'center';
    // ticks every 30° of yaw, ±90° window around current heading
    for (let d = -90; d <= 90; d += 30) {
      const hdg = Math.round((yaw + d + 540) % 360) - 180;
      const tx = cx + (d / 90) * R;
      ctx.beginPath();
      ctx.moveTo(tx, hy - 3 * dpr);
      ctx.lineTo(tx, hy + 3 * dpr);
      ctx.stroke();
      ctx.fillText(String(hdg), tx, hy + 14 * dpr);
    }
    // current heading marker
    ctx.fillStyle = '#59c2ff';
    ctx.beginPath();
    ctx.moveTo(cx, hy - 6 * dpr);
    ctx.lineTo(cx - 4 * dpr, hy - 12 * dpr);
    ctx.lineTo(cx + 4 * dpr, hy - 12 * dpr);
    ctx.closePath();
    ctx.fill();
    ctx.fillStyle = 'rgba(205,214,228,0.8)';
    ctx.fillText(`yaw ${yaw.toFixed(0)}°  pitch ${pitch.toFixed(0)}°  roll ${roll.toFixed(0)}°`, cx, y0 + 16 * dpr);
    ctx.textAlign = 'left';
  }

  _drawMap(ctx, x0, y0, w, h, state, dpr) {
    const cx = x0 + w / 2, cy = y0 + h / 2;

    ctx.fillStyle = 'rgba(205,214,228,0.6)';
    ctx.font = `${10 * dpr}px ui-monospace, monospace`;
    ctx.textAlign = 'center';
    ctx.fillText('top-down (x/z)', cx, y0 + 16 * dpr);
    ctx.textAlign = 'left';

    if (!state.hasPosition) {
      ctx.fillStyle = 'rgba(160,175,205,0.4)';
      ctx.textAlign = 'center';
      ctx.fillText('no 6DoF source — connect the bridge', cx, cy);
      ctx.textAlign = 'left';
      return;
    }

    // scale: fit trail extent, min ±0.5 m
    let ext = 0.5;
    for (const [tx, tz] of this.trail) ext = Math.max(ext, Math.abs(tx), Math.abs(tz));
    const R = Math.min(w, h) * 0.42;
    const s = R / ext;

    // grid rings each 0.5 m
    ctx.strokeStyle = 'rgba(120,140,180,0.15)';
    ctx.lineWidth = dpr;
    ctx.fillStyle = 'rgba(160,175,205,0.35)';
    ctx.font = `${9 * dpr}px ui-monospace, monospace`;
    for (let r = 0.5; r <= ext + 0.01; r += 0.5) {
      ctx.beginPath();
      ctx.arc(cx, cy, r * s, 0, Math.PI * 2);
      ctx.stroke();
      ctx.fillText(`${r.toFixed(1)}m`, cx + r * s + 3 * dpr, cy - 3 * dpr);
    }
    ctx.beginPath();
    ctx.moveTo(cx - R, cy); ctx.lineTo(cx + R, cy);
    ctx.moveTo(cx, cy - R); ctx.lineTo(cx, cy + R);
    ctx.stroke();

    // trail (world x → right, z → down: looking from above, -z forward is up)
    ctx.strokeStyle = 'rgba(170,217,76,0.7)';
    ctx.lineWidth = 1.5 * dpr;
    ctx.beginPath();
    this.trail.forEach(([tx, tz], i) => {
      const px = cx + tx * s, py = cy + tz * s;
      if (i === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    });
    ctx.stroke();

    // current position + heading arrow
    const p = state.position;
    const px = cx + p.x * s, py = cy + p.z * s;
    const yawRad = state.euler.yaw * Math.PI / 180;
    ctx.fillStyle = '#59c2ff';
    ctx.save();
    ctx.translate(px, py);
    ctx.rotate(-yawRad); // yaw 0 = -z (up on the map)
    ctx.beginPath();
    ctx.moveTo(0, -8 * dpr);
    ctx.lineTo(-5 * dpr, 6 * dpr);
    ctx.lineTo(5 * dpr, 6 * dpr);
    ctx.closePath();
    ctx.fill();
    ctx.restore();

    ctx.fillStyle = 'rgba(205,214,228,0.8)';
    ctx.textAlign = 'center';
    ctx.fillText(`x ${p.x.toFixed(2)}  y ${p.y.toFixed(2)}  z ${p.z.toFixed(2)} m`, cx, y0 + h - 10 * dpr);
    ctx.textAlign = 'left';
  }
}
