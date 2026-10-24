import { loadCore, Renderer, OrbitCamera, loadLevel } from './common.js';

const $ = (id) => document.getElementById(id);
const M = await loadCore();
const renderer = new Renderer($('c'), M);
renderer.xray = 2.2;
const cam = new OrbitCamera([32, 8, 48], 14);
cam.pitch = 0.45;

async function loadRequestedLevel() {
  const q = new URLSearchParams(location.search).get('level');
  if (!q) return M._dg_default_level();
  const text = q === 'local' ? localStorage.getItem('deathgolf.level') : await (await fetch(q)).text();
  if (!text) throw new Error('no level found');
  loadLevel(M, JSON.parse(text));
}
try {
  await loadRequestedLevel();
} catch (e) {
  alert('Level load failed, using default: ' + e.message);
  M._dg_default_level();
}
M._dg_start_game();

const keys = new Set();
let club = 1, charging = false, chargeT = 0, power = 0, flash = '', flashUntil = 0, dragging = false;
const CLUBS = ['Putter', 'Iron', 'Wedge'];

// Physical key, so Shift/Caps can't leave a key stuck down.
const keyOf = (e) => (e.code.startsWith('Key') ? e.code.slice(3).toLowerCase() : e.code.startsWith('Digit') ? e.code.slice(5) : e.code === 'Space' ? ' ' : e.key.toLowerCase());
window.onkeydown = (e) => {
  const k = keyOf(e);
  if (e.repeat) return;
  keys.add(k);
  if (k === 'e') M._dg_interact();
  if (k === 'r') restart();
  if (k >= '1' && k <= '3') club = +k - 1;
  if (k === 'f' && M._dg_status() === 0) (charging = true), (chargeT = 0);
  if (k === ' ') e.preventDefault();
};
window.onkeyup = (e) => {
  const k = keyOf(e);
  keys.delete(k);
  if (k === 'f' && charging) {
    const [fx, fz] = cam.forward();
    if (!M._dg_shoot(fx, fz, power, club)) say(M._dg_status() ? '' : 'Get next to a resting ball to shoot');
    charging = false;
    M._dg_set_aim(0, 0, 0, 0, 0);
  }
};
window.onblur = () => keys.clear();
$('c').onmousedown = () => (dragging = true);
window.onmouseup = () => (dragging = false);
window.onmousemove = (e) => dragging && cam.drag(e.movementX, e.movementY);
$('c').onwheel = (e) => {
  e.preventDefault();
  cam.zoom(e.deltaY);
};

function restart() {
  M._dg_restart();
  charging = false;
  M._dg_set_aim(0, 0, 0, 0, 0);
  $('end').style.display = 'none';
}
function say(t) {
  flash = t;
  flashUntil = performance.now() + 2500;
}
function scoreName(diff, strokes) {
  if (strokes === 1) return 'Hole in one!';
  return ({ '-3': 'Albatross', '-2': 'Eagle', '-1': 'Birdie', 0: 'Par', 1: 'Bogey', 2: 'Double bogey' })[diff] ?? (diff > 0 ? `+${diff}` : `${diff}`);
}

let prev = performance.now();
function frame(now) {
  const dt = Math.min(0.05, (now - prev) / 1000);
  prev = now;
  const w = (keys.has('w') ? 1 : 0) - (keys.has('s') ? 1 : 0), d = (keys.has('d') ? 1 : 0) - (keys.has('a') ? 1 : 0);
  const [fx, fz] = cam.forward();
  let mx = fx * w - fz * d, mz = fz * w + fx * d;
  const l = Math.hypot(mx, mz);
  if (l > 1) (mx /= l), (mz /= l);
  const jump = keys.has(' ') ? 1 : 0;
  M._dg_input(mx, mz, jump, jump);

  if (charging) {
    // Power sweeps min -> max -> min (triangle wave); release to lock it in.
    chargeT += dt;
    const ph = (chargeT / 1.2) % 2;
    power = ph < 1 ? ph : 2 - ph;
    M._dg_set_aim(M._dg_can_shoot(), fx, fz, power, club);
  }
  M._dg_step(dt);

  const b = M.ball();
  cam.target = [b[3], b[4] + 0.5, b[5]];
  renderer.refreshTerrain();
  renderer.refreshEntities(false);
  renderer.draw(cam);
  hud();
  requestAnimationFrame(frame);
}

// Minicube: the level cube rotated with the camera, showing worm, hole, ball and player.
const mini = $('mini').getContext('2d');
function drawMini() {
  const NX = M.NX, NY = M.NY, NZ = M.NZ, W = 180, H = 150, sc = 1.1;
  const cx = W / 2, cy = H / 2 + 14, c = Math.cos(cam.yaw), s = Math.sin(cam.yaw), tilt = 0.55;
  const proj = (x, y, z) => {
    const a = x - NX / 2, b = z - NZ / 2;
    const rx = a * c - b * s, rz = a * s + b * c;
    return [cx + rx * sc, cy + (rz * tilt - y * 0.9) * sc, rz];
  };
  mini.clearRect(0, 0, W, H);
  const corner = (i, y) => proj((i & 1) * NX, y, (i >> 1) * NZ);
  mini.lineWidth = 1;
  mini.strokeStyle = 'rgba(255,255,255,.45)';
  for (const y of [0, NY]) {
    mini.beginPath();
    [0, 1, 3, 2, 0].forEach((i, k) => mini[k ? 'lineTo' : 'moveTo'](...corner(i, y).slice(0, 2)));
    mini.stroke();
  }
  for (let i = 0; i < 4; i++) {
    mini.beginPath();
    mini.moveTo(...corner(i, 0).slice(0, 2));
    mini.lineTo(...corner(i, NY).slice(0, 2));
    mini.stroke();
  }
  const b = M.ball(), o = M.objs();
  const dot = (x, y, z, col, r, drop) => {
    const [px, py] = proj(x, y, z);
    if (drop) {
      const [, fy] = proj(x, 0, z);
      mini.strokeStyle = col + '66';
      mini.beginPath();
      mini.moveTo(px, py);
      mini.lineTo(px, fy);
      mini.stroke();
    }
    mini.fillStyle = col;
    mini.beginPath();
    mini.arc(px, py, r, 0, 7);
    mini.fill();
  };
  dot(o[4], o[5], o[6], '#ff3b3b', 3.5, false); // hole
  dot(b[0], b[1], b[2], '#ffffff', 2.5, false);
  dot(b[3], b[4], b[5], '#4d8dff', 3, false);
  dot(b[7], b[8], b[9], '#ff7ab0', 7, true); // worm
}

function hud() {
  const status = M._dg_status(), strokes = M._dg_strokes(), par = M._dg_get_param(0);
  const eta = M._dg_worm_eta();
  const items = [
    `Strokes <b>${strokes}</b> / Par <b>${par}</b>`,
    `Time <b>${M._dg_time().toFixed(1)}s</b>`,
    `Worm reaches hole in <b class="${eta < 10 ? 'warn' : ''}">${eta.toFixed(0)}s</b>${M._dg_worm_final() ? ' <span class="warn">(going for the hole!)</span>' : ''}`,
    `Club <b>${CLUBS[club]}</b>${M._dg_can_shoot() ? ' · <b>ball in reach</b>' : ''}`,
  ];
  if (M._dg_mounted()) items.push('Riding golf cart (E to hop off)');
  if (M._dg_has_jet()) items.push(`Jetpack fuel <b>${M._dg_fuel().toFixed(1)}</b> (hold Space)`);
  if (charging) items.push(`<div id="power"><div style="width:${power * 100}%"></div></div>`);
  if (performance.now() < flashUntil) items.push(`<span class="warn">${flash}</span>`);
  $('stats').innerHTML = items.join('<br>');
  drawMini();

  const end = $('end');
  if (status !== 0) {
    const diff = strokes - par;
    end.innerHTML =
      status === 1
        ? `${scoreName(diff, strokes)}<small>${strokes} strokes (par ${par}) in ${M._dg_time().toFixed(1)}s</small><small>R to play again</small>`
        : `The worm got you<small>${M._dg_worm_final() ? 'It devoured the hole.' : 'Eaten before you finished.'}</small><small>R to retry</small>`;
    end.style.display = 'flex';
  }
}

requestAnimationFrame(frame);
