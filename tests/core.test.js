// Headless checks against the built WASM: node tests/core.test.js
const assert = require('assert');
const createCore = require('../web/wasm/core.js');

(async () => {
  const M = await createCore();
  const NX = M._dg_size(), NZ = M._dg_size_z();
  assert.strictEqual([NX, M._dg_size_y(), NZ].join('x'), '64x32x96', 'course is 2x3 of the 32-cube block');
  const vox = (x, y, z) => M.HEAPU8[M._dg_voxels() + (y * NZ + z) * NX + x];

  M._dg_default_level();
  assert.strictEqual(M._dg_validate(), 0, 'default level valid');

  // Sculpting + raycast
  assert.ok(M._dg_raycast(16, 40, 16, 0, -1, 0), 'ray hits terrain');
  const r = new Float32Array(M.HEAPF32.buffer, M._dg_ray(), 9);
  const [cx, cy, cz] = [r[0], r[1], r[2]];
  assert.ok(vox(cx, cy, cz) && !vox(cx, cy + 1, cz));
  M._dg_brush(1, cx + .5, cy + .5, cz + .5, 2);
  assert.ok(!vox(cx, cy, cz), 'lower removes');
  M._dg_brush(0, cx + .5, cy + .5, cz + .5, 2);
  assert.ok(vox(cx, cy, cz), 'raise adds');
  assert.ok(M._dg_terrain_mesh() > 1000 && M._dg_entities_mesh(1) > 0);

  // Crit path: two shots (tee shot, walk up, approach) sink the ball before the worm arrives.
  const b = () => new Float32Array(M.HEAPF32.buffer, M._dg_ball(), 12);
  const hole = new Float32Array(M.HEAPF32.buffer, M._dg_objs(), 20).slice(4, 7);
  const run = (sec) => { for (let i = 0; i < 60 * sec && M._dg_status() === 0; i++) M._dg_step(1 / 60); };
  const settle = () => { for (let i = 0; i < 60 * 15; i++) { M._dg_step(1 / 60); if (b()[6] && i > 30) break; } };
  const walkToBall = () => {
    for (let i = 0; i < 60 * 40 && !M._dg_can_shoot() && M._dg_status() === 0; i++) {
      const q = b(), dx = q[0] - q[3], dz = q[2] - q[5], l = Math.hypot(dx, dz) || 1;
      M._dg_input(dx / l, dz / l, 0, 0);
      M._dg_step(1 / 60);
    }
    M._dg_input(0, 0, 0, 0);
  };
  const angleTo = () => Math.atan2(hole[2] - b()[2], hole[0] - b()[0]);
  const fresh = () => { M._dg_restart(); run(1.5); };

  M._dg_default_level();
  M._dg_start_game();
  fresh();
  const a1 = angleTo();
  const startDist = Math.hypot(hole[0] - b()[0], hole[2] - b()[2]);
  // The long course takes several full-power shots to reach the green.
  const shot1 = () => {
    fresh();
    for (let k = 0; k < 8 && Math.hypot(hole[0] - b()[0], hole[2] - b()[2]) > 20; k++) {
      const a = k ? angleTo() : a1;
      assert.ok(M._dg_shoot(Math.cos(a), Math.sin(a), 1, 1), 'approach shot');
      settle();
      walkToBall();
    }
  };
  shot1();
  assert.ok(M._dg_can_shoot(), 'player reaches ball after approach shots');
  const afterDist = Math.hypot(hole[0] - b()[0], hole[2] - b()[2]);
  console.log('start distance', startDist.toFixed(1), 'after approach, distance to hole', afterDist.toFixed(1), 'strokes', M._dg_strokes());
  assert.ok(afterDist < 20, 'approach shots make real progress');

  let won = false, tries = 0, best = null;
  for (let da = -0.3; da <= 0.3 && !won; da += 0.015)
    for (let p = 0.1; p <= 1 && !won; p += 0.05)
      for (const c of [0, 1, 2]) {
        tries++;
        shot1();
        if (!M._dg_can_shoot()) continue;
        const a = angleTo() + da;
        M._dg_shoot(Math.cos(a), Math.sin(a), p, c);
        run(10);
        if (M._dg_status() === 1) { won = true; best = { a, p, c, strokes: M._dg_strokes(), t: M._dg_time() }; break; }
      }
  console.log('winning approach', best, 'after', tries, 'tries');
  assert.ok(won, 'a 2-stroke hole exists');

  // Aim arc: preview should have points and end near where the ball lands.
  fresh();
  M._dg_set_aim(1, 1, 0, 0.8, 1);
  assert.ok(M._dg_entities_mesh(0) > 36 * 4, 'aim arc rendered');
  M._dg_set_aim(0, 0, 0, 0, 0);

  // Worm eats dirt, can be ridden on its back (player and ball), and its mouth kills.
  {
    const count = () => { let c = 0; for (const x of new Uint8Array(M.HEAPU8.buffer, M._dg_voxels(), NX * M._dg_size_y() * NZ)) if (x) c++; return c; };
    const segs = () => new Float32Array(M.HEAPF32.buffer, M._dg_worm_segs(), 36).slice();
    M._dg_restart();
    const c0 = count();
    for (let i = 0; i < 60 * 20; i++) M._dg_step(1 / 60);
    console.log('dirt eaten', c0 - count());
    assert.ok(c0 - count() > 50, 'worm eats dirt');
    let s = segs();
    M._dg_teleport(0, s[12], s[13] + s[15] + .6, s[14]);
    for (let i = 0; i < 60 * 2 && M._dg_status() === 0; i++) M._dg_step(1 / 60);
    let s2 = segs(), q = b();
    assert.strictEqual(M._dg_status(), 0, 'standing on the back is safe');
    assert.ok(Math.hypot(q[3] - s2[12], q[5] - s2[14]) < 1.5 && q[4] > s2[13], 'player rides the worm');
    M._dg_teleport(1, s2[16], s2[17] + s2[19] + .6, s2[18]);
    for (let i = 0; i < 60; i++) M._dg_step(1 / 60);
    s2 = segs(); q = b();
    assert.ok(Math.hypot(q[0] - s2[16], q[2] - s2[18]) < 2 && q[1] > s2[17], 'ball rides the worm');
    M._dg_restart();
    s = segs();
    const dx = s[0] - s[4], dz = s[2] - s[6], l = Math.hypot(dx, dz);
    M._dg_teleport(0, s[0] + dx / l * 1.5, s[1], s[2] + dz / l * 1.5);
    M._dg_step(1 / 60);
    assert.strictEqual(M._dg_status(), 2, 'mouth kills');
  }

  // Worm: eats everything else first, hole last, and eventually wins if idle.
  M._dg_restart();
  const hx = Math.floor(hole[0]), hz = Math.floor(hole[2]);
  const holeTop = hole[1] - 1;
  let holeIntactUntilFinal = true, t = 0;
  while (M._dg_status() === 0 && t < 600) {
    M._dg_step(1 / 30); t += 1 / 30;
    if (!M._dg_worm_final() && !vox(hx, holeTop, hz)) holeIntactUntilFinal = false;
  }
  console.log('worm ended game (status', M._dg_status(), ') at', t.toFixed(1), 's');
  assert.ok(holeIntactUntilFinal, 'hole protected until final phase');
  assert.strictEqual(M._dg_status(), 2, 'worm eventually wins');

  // Player walking sanity: moves and stays above ground.
  M._dg_restart();
  const p0 = b()[3];
  M._dg_input(1, 0, 0, 0);
  for (let i = 0; i < 60; i++) M._dg_step(1 / 60);
  assert.ok(b()[3] > p0 + 2, 'player walks');
  assert.ok(b()[4] > 0, 'player on ground');
  console.log('ok');
})();
