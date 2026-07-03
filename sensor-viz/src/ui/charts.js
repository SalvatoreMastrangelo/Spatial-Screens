// Dependency-free canvas strip charts. Fixed time window, auto-scaling,
// devicePixelRatio aware. One Scope per <canvas>.

const WINDOW_MS = 15000;

export class Scope {
  constructor(canvas, series, opts = {}) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.series = series;         // state.js Series instance
    this.fixedRange = opts.range ?? null; // [min,max] or null for auto
    this.gridSteps = opts.gridSteps ?? 4;
    // Defer to the next frame: resizing the canvas backing store inside the
    // observer callback re-triggers observation ("ResizeObserver loop").
    this._ro = new ResizeObserver(() => requestAnimationFrame(() => this._fit()));
    this._ro.observe(canvas);
    this._fit();
  }

  _fit() {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = this.canvas.clientWidth || 300;
    const h = this.canvas.clientHeight || 120;
    if (this.canvas.width !== w * dpr || this.canvas.height !== h * dpr) {
      this.canvas.width = w * dpr;
      this.canvas.height = h * dpr;
    }
    this.dpr = dpr;
  }

  draw(now) {
    const { ctx, canvas } = this;
    const W = canvas.width, H = canvas.height;
    ctx.clearRect(0, 0, W, H);
    const samples = this.series.samples;
    const pad = 6 * this.dpr;

    // range
    let min = Infinity, max = -Infinity;
    if (this.fixedRange) {
      [min, max] = this.fixedRange;
    } else {
      for (const s of samples) for (const v of s.v) {
        if (v < min) min = v;
        if (v > max) max = v;
      }
      if (!Number.isFinite(min)) { min = -1; max = 1; }
      if (max - min < 1e-6) { max += 0.5; min -= 0.5; }
      const m = (max - min) * 0.1;
      min -= m; max += m;
    }

    // grid
    ctx.strokeStyle = 'rgba(120,140,180,0.12)';
    ctx.lineWidth = 1;
    ctx.font = `${10 * this.dpr}px ui-monospace, monospace`;
    ctx.fillStyle = 'rgba(160,175,205,0.45)';
    for (let i = 0; i <= this.gridSteps; i++) {
      const y = pad + ((H - 2 * pad) * i) / this.gridSteps;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(W, y);
      ctx.stroke();
      const val = max - ((max - min) * i) / this.gridSteps;
      ctx.fillText(val.toFixed(Math.abs(max - min) > 20 ? 0 : 2), 4 * this.dpr, y - 2 * this.dpr);
    }

    if (samples.length < 2) return;

    const t1 = now;
    const t0 = t1 - WINDOW_MS;
    const nSeries = samples[0].v.length;
    for (let si = 0; si < nSeries; si++) {
      ctx.strokeStyle = this.series.colors[si] ?? '#fff';
      ctx.lineWidth = 1.5 * this.dpr;
      ctx.beginPath();
      let started = false;
      for (const s of samples) {
        const x = ((s.t - t0) / WINDOW_MS) * W;
        if (x < -10) continue;
        const y = pad + (1 - (s.v[si] - min) / (max - min)) * (H - 2 * pad);
        if (!started) { ctx.moveTo(x, y); started = true; }
        else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }
  }
}
