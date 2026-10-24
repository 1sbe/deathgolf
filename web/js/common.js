// Shared helpers: WASM loading, level (de)serialization, WebGL renderer, orbit camera.

const OBJ_NAMES = ['tee', 'hole', 'cart', 'jetpack', 'wormStart'];

export async function loadCore() {
  await new Promise((res, rej) => {
    const s = document.createElement('script');
    s.src = 'wasm/core.js';
    s.onload = res;
    s.onerror = () => rej(new Error('wasm/core.js missing - run ./build.sh'));
    document.head.appendChild(s);
  });
  const M = await createCore();
  M.N = M._dg_size();
  M.voxels = () => new Uint8Array(M.HEAPU8.buffer, M._dg_voxels(), M.N ** 3);
  M.objs = () => new Float32Array(M.HEAPF32.buffer, M._dg_objs(), 20);
  M.ball = () => new Float32Array(M.HEAPF32.buffer, M._dg_ball(), 12);
  return M;
}

export function serializeLevel(M) {
  const vox = M.voxels();
  let bin = '';
  for (let i = 0; i < vox.length; i += 4096) bin += String.fromCharCode.apply(null, vox.subarray(i, i + 4096));
  const o = M.objs();
  const level = { version: 1, size: M.N, par: M._dg_get_param(0), wormSpeed: M._dg_get_param(1), voxels: btoa(bin) };
  OBJ_NAMES.forEach((name, t) => {
    level[name] = o[t * 4 + 3] ? [o[t * 4], o[t * 4 + 1], o[t * 4 + 2]] : null;
  });
  return level;
}

export function loadLevel(M, level) {
  if (level.size !== M.N) throw new Error(`level size ${level.size} != ${M.N}`);
  const bin = atob(level.voxels);
  if (bin.length !== M.N ** 3) throw new Error('bad voxel data');
  M._dg_clear();
  const vox = M.voxels();
  for (let i = 0; i < bin.length; i++) vox[i] = bin.charCodeAt(i);
  OBJ_NAMES.forEach((name, t) => {
    const p = level[name];
    if (p) M._dg_place(t, Math.floor(p[0]), Math.floor(p[2]));
    else M._dg_remove(t);
  });
  M._dg_set_param(0, level.par ?? 3);
  M._dg_set_param(1, level.wormSpeed ?? 1.6);
  M._dg_mark_dirty();
}

// ---------------------------------------------------------------- matrices
const mul = (a, b) => {
  const o = new Float32Array(16);
  for (let c = 0; c < 4; c++)
    for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      o[c * 4 + r] = s;
    }
  return o;
};
const perspective = (fov, asp, n, f) => {
  const t = 1 / Math.tan(fov / 2);
  return new Float32Array([t / asp, 0, 0, 0, 0, t, 0, 0, 0, 0, (f + n) / (n - f), -1, 0, 0, (2 * f * n) / (n - f), 0]);
};
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const normalize = (a) => {
  const l = Math.hypot(...a) || 1;
  return [a[0] / l, a[1] / l, a[2] / l];
};
const lookAt = (e, t) => {
  const f = normalize(sub(t, e)), s = normalize(cross(f, [0, 1, 0])), u = cross(s, f);
  return new Float32Array([
    s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0,
    -(s[0] * e[0] + s[1] * e[1] + s[2] * e[2]), -(u[0] * e[0] + u[1] * e[1] + u[2] * e[2]), f[0] * e[0] + f[1] * e[1] + f[2] * e[2], 1,
  ]);
};

export class OrbitCamera {
  constructor(target, dist = 45) {
    this.target = target;
    this.dist = dist;
    this.yaw = 0.8;
    this.pitch = 0.6;
  }
  eye() {
    const { yaw, pitch, dist, target } = this;
    return [
      target[0] + dist * Math.cos(pitch) * Math.sin(yaw),
      target[1] + dist * Math.sin(pitch),
      target[2] + dist * Math.cos(pitch) * Math.cos(yaw),
    ];
  }
  // Horizontal unit vector the camera looks along.
  forward() {
    return [-Math.sin(this.yaw), -Math.cos(this.yaw)];
  }
  drag(dx, dy) {
    this.yaw -= dx * 0.008;
    this.pitch = Math.max(0.05, Math.min(1.5, this.pitch + dy * 0.008));
  }
  zoom(dy) {
    this.dist = Math.max(6, Math.min(120, this.dist * Math.exp(dy * 0.001)));
  }
  matrices(aspect) {
    return { view: lookAt(this.eye(), this.target), proj: perspective(1.0, aspect, 0.3, 400) };
  }
  // World-space ray through a canvas pixel (nx, ny in -1..1).
  ray(nx, ny, aspect) {
    const e = this.eye();
    const f = normalize(sub(this.target, e)), s = normalize(cross(f, [0, 1, 0])), u = cross(s, f);
    const t = Math.tan(0.5);
    const d = [0, 1, 2].map((i) => f[i] + s[i] * nx * t * aspect + u[i] * ny * t);
    return { o: e, d: normalize(d) };
  }
}

export class Renderer {
  constructor(canvas, M) {
    this.canvas = canvas;
    this.M = M;
    const gl = (this.gl = canvas.getContext('webgl'));
    const sh = (type, src) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
      return s;
    };
    const p = (this.prog = gl.createProgram());
    gl.attachShader(p, sh(gl.VERTEX_SHADER, `attribute vec3 pos; attribute vec3 col; uniform mat4 mvp; varying vec3 c; varying float d;
      void main(){ gl_Position = mvp*vec4(pos,1.0); c = col; d = gl_Position.w; }`));
    gl.attachShader(p, sh(gl.FRAGMENT_SHADER, `precision mediump float; varying vec3 c; varying float d; uniform vec3 sky;
      void main(){ gl_FragColor = vec4(mix(c, sky, clamp((d-60.0)/120.0, 0.0, 0.6)), 1.0); }`));
    gl.linkProgram(p);
    gl.useProgram(p);
    this.locs = { mvp: gl.getUniformLocation(p, 'mvp'), sky: gl.getUniformLocation(p, 'sky') };
    gl.uniform3f(this.locs.sky, 0.55, 0.75, 0.95);
    this.bufs = { terrain: { buf: gl.createBuffer(), n: 0 }, ents: { buf: gl.createBuffer(), n: 0 } };
    gl.enable(gl.DEPTH_TEST);
    gl.clearColor(0.55, 0.75, 0.95, 1);
  }
  upload(which, ptr, nverts) {
    const gl = this.gl, b = this.bufs[which];
    b.n = nverts;
    gl.bindBuffer(gl.ARRAY_BUFFER, b.buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(this.M.HEAPF32.buffer, ptr, nverts * 6), gl.DYNAMIC_DRAW);
  }
  draw(cam) {
    const { gl, canvas } = this;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
    gl.viewport(0, 0, w, h);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    const { view, proj } = cam.matrices(w / h);
    gl.uniformMatrix4fv(this.locs.mvp, false, mul(proj, view));
    const pos = gl.getAttribLocation(this.prog, 'pos'), col = gl.getAttribLocation(this.prog, 'col');
    gl.enableVertexAttribArray(pos);
    gl.enableVertexAttribArray(col);
    for (const b of Object.values(this.bufs)) {
      if (!b.n) continue;
      gl.bindBuffer(gl.ARRAY_BUFFER, b.buf);
      gl.vertexAttribPointer(pos, 3, gl.FLOAT, false, 24, 0);
      gl.vertexAttribPointer(col, 3, gl.FLOAT, false, 24, 12);
      gl.drawArrays(gl.TRIANGLES, 0, b.n);
    }
  }
  refreshTerrain() {
    if (this.M._dg_take_dirty()) {
      const n = this.M._dg_terrain_mesh();
      this.upload('terrain', this.M._dg_terrain_ptr(), n);
    }
  }
  refreshEntities(editor) {
    const n = this.M._dg_entities_mesh(editor ? 1 : 0);
    this.upload('ents', this.M._dg_entities_ptr(), n);
  }
}
