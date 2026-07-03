// Three.js spatial view: a stylized pair of glasses driven by the live
// orientation quaternion, with a 6DoF position trail when available.

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

const TRAIL_MAX = 4000;

export class Scene3D {
  constructor(container) {
    this.container = container;

    this.renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    container.appendChild(this.renderer.domElement);

    this.scene = new THREE.Scene();
    this.scene.fog = new THREE.Fog(0x0b0e14, 8, 22);

    this.camera = new THREE.PerspectiveCamera(50, 1, 0.01, 100);
    this.camera.position.set(1.6, 1.2, 2.2);

    this.controls = new OrbitControls(this.camera, this.renderer.domElement);
    this.controls.enableDamping = true;
    this.controls.target.set(0, 0.9, 0);

    // lighting
    this.scene.add(new THREE.HemisphereLight(0x8899bb, 0x222833, 1.2));
    const key = new THREE.DirectionalLight(0xffffff, 1.4);
    key.position.set(2, 4, 3);
    this.scene.add(key);

    // floor + reference
    const grid = new THREE.GridHelper(10, 20, 0x2d3646, 0x1c2330);
    this.scene.add(grid);
    this.scene.add(new THREE.AxesHelper(0.5));

    this.head = this._buildGlasses();
    this.head.position.set(0, 0.9, 0); // resting head height reference
    this.scene.add(this.head);

    // forward gaze ray
    const gazeGeo = new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(0, 0, 0),
      new THREE.Vector3(0, 0, -3),
    ]);
    const gaze = new THREE.Line(gazeGeo, new THREE.LineDashedMaterial({ color: 0x59c2ff, dashSize: 0.08, gapSize: 0.05, transparent: true, opacity: 0.7 }));
    gaze.computeLineDistances();
    this.head.add(gaze);

    // position trail (6DoF)
    this.trailPositions = new Float32Array(TRAIL_MAX * 3);
    this.trailCount = 0;
    this.trailGeo = new THREE.BufferGeometry();
    this.trailAttr = new THREE.BufferAttribute(this.trailPositions, 3);
    this.trailAttr.setUsage(THREE.DynamicDrawUsage);
    this.trailGeo.setAttribute('position', this.trailAttr);
    this.trail = new THREE.Line(this.trailGeo, new THREE.LineBasicMaterial({ color: 0xaad94c, transparent: true, opacity: 0.85 }));
    this.trail.frustumCulled = false;
    this.scene.add(this.trail);

    // ground shadow dot
    this.dot = new THREE.Mesh(
      new THREE.CircleGeometry(0.06, 24),
      new THREE.MeshBasicMaterial({ color: 0xaad94c, transparent: true, opacity: 0.35 })
    );
    this.dot.rotation.x = -Math.PI / 2;
    this.dot.position.y = 0.001;
    this.scene.add(this.dot);

    this._resize = this._resize.bind(this);
    window.addEventListener('resize', this._resize);
    new ResizeObserver(() => requestAnimationFrame(this._resize)).observe(container);
    this._resize();
  }

  _buildGlasses() {
    const g = new THREE.Group();
    const frameMat = new THREE.MeshStandardMaterial({ color: 0x30394a, roughness: 0.4, metalness: 0.6 });
    const lensMat = new THREE.MeshPhysicalMaterial({
      color: 0x111722, roughness: 0.1, metalness: 0.2,
      transparent: true, opacity: 0.82, envMapIntensity: 0.6,
    });
    const accentMat = new THREE.MeshStandardMaterial({ color: 0x59c2ff, emissive: 0x59c2ff, emissiveIntensity: 0.6 });

    const lensGeo = new THREE.BoxGeometry(0.085, 0.05, 0.012);
    for (const side of [-1, 1]) {
      const lens = new THREE.Mesh(lensGeo, lensMat);
      lens.position.set(side * 0.05, 0, 0);
      g.add(lens);
      const temple = new THREE.Mesh(new THREE.BoxGeometry(0.012, 0.012, 0.14), frameMat);
      temple.position.set(side * 0.095, 0.012, 0.07);
      g.add(temple);
    }
    const bridge = new THREE.Mesh(new THREE.BoxGeometry(0.022, 0.012, 0.012), frameMat);
    g.add(bridge);
    const bar = new THREE.Mesh(new THREE.BoxGeometry(0.2, 0.014, 0.014), frameMat);
    bar.position.set(0, 0.03, 0);
    g.add(bar);
    // camera pods (Luma Ultra spatial cameras)
    for (const side of [-1, 1]) {
      const pod = new THREE.Mesh(new THREE.CylinderGeometry(0.008, 0.008, 0.006, 16), accentMat);
      pod.rotation.x = Math.PI / 2;
      pod.position.set(side * 0.085, 0.03, -0.008);
      g.add(pod);
    }
    const scaleUp = 4; // stylized, larger than life
    g.scale.setScalar(scaleUp);
    return g;
  }

  update(state) {
    const q = state.quat;
    this.head.quaternion.set(q.x, q.y, q.z, q.w);

    if (state.hasPosition && state.position) {
      const p = state.position;
      this.head.position.set(p.x, 0.9 + p.y, p.z);
      this.dot.position.set(p.x, 0.001, p.z);

      const i = this.trailCount % TRAIL_MAX;
      this.trailPositions[i * 3] = p.x;
      this.trailPositions[i * 3 + 1] = 0.9 + p.y;
      this.trailPositions[i * 3 + 2] = p.z;
      this.trailCount++;
      this.trailGeo.setDrawRange(0, Math.min(this.trailCount, TRAIL_MAX));
      this.trailAttr.needsUpdate = true;
    }

    this.controls.update();
    this.renderer.render(this.scene, this.camera);
  }

  clearTrail() {
    this.trailCount = 0;
    this.trailGeo.setDrawRange(0, 0);
    this.trailAttr.needsUpdate = true;
  }

  _resize() {
    const w = this.container.clientWidth || 1;
    const h = this.container.clientHeight || 1;
    this.renderer.setSize(w, h, false);
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
  }
}
