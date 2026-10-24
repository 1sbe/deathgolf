import { loadCore, Renderer, OrbitCamera, serializeLevel, loadLevel } from './common.js';

const TOOLS = [
  ['Raise', 0], ['Lower', 1], ['Smooth', 2], ['Flatten', 3], ['Paint green', 4], ['Paint dirt', 5],
  ['Place tee', 'p0'], ['Place hole', 'p1'], ['Place cart', 'p2'], ['Place jetpack', 'p3'], ['Place worm start', 'p4'],
];
const OBJ_LABEL = ['Tee', 'Hole', 'Cart', 'Jetpack', 'Worm start'];
const $ = (id) => document.getElementById(id);

const M = await loadCore();
const canvas = $('c');
const renderer = new Renderer(canvas, M);
const cam = new OrbitCamera([M.NX / 2, 6, M.NZ / 2], 90);
let tool = 0;
const undoStack = [];

function say(t, bad) {
  $('msg').textContent = t;
  $('msg').className = bad ? 'bad' : '';
}

// ---- panel
TOOLS.forEach(([name, id], i) => {
  const b = document.createElement('button');
  b.textContent = name;
  b.onclick = () => {
    tool = id;
    [...$('tools').children].forEach((c) => c.classList.toggle('on', c === b));
  };
  if (i === 0) b.classList.add('on');
  $('tools').appendChild(b);
});
for (let t = 0; t < 5; t++) {
  if (t === 0) continue; // tee is required, moved rather than removed
  const b = document.createElement('button');
  b.textContent = `Remove ${OBJ_LABEL[t].toLowerCase()}`;
  b.className = 'rm';
  b.onclick = () => {
    M._dg_remove(t);
    M._dg_mark_dirty();
    validate();
  };
  $('tools').appendChild(b);
}
$('radius').oninput = () => ($('rv').textContent = $('radius').value);
$('par').onchange = () => M._dg_set_param(0, +$('par').value);
$('wspeed').onchange = () => M._dg_set_param(1, +$('wspeed').value);

function syncParams() {
  $('par').value = M._dg_get_param(0);
  $('wspeed').value = M._dg_get_param(1);
}
function validate() {
  const m = M._dg_validate();
  if (m) say('Missing: ' + [m & 1 && 'tee', m & 2 && 'hole'].filter(Boolean).join(', '), true);
  else say('Level OK');
  return !m;
}
function pushUndo() {
  undoStack.push(M.voxels().slice());
  if (undoStack.length > 40) undoStack.shift();
}
function undo() {
  const s = undoStack.pop();
  if (!s) return;
  M.voxels().set(s);
  M._dg_mark_dirty();
}
function resetWith(fn) {
  pushUndo();
  fn();
  syncParams();
  validate();
}
$('undo').onclick = undo;
$('def').onclick = () => resetWith(() => M._dg_default_level());
$('new').onclick = () =>
  resetWith(() => {
    M._dg_clear();
    M._dg_brush(3, 16, 5, 16, 40);
    M._dg_place(0, 6, 6);
    M._dg_place(1, 25, 25);
    M._dg_place(4, 28, 4);
  });
$('save').onclick = () => {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([JSON.stringify(serializeLevel(M))], { type: 'application/json' }));
  a.download = 'level.json';
  a.click();
};
$('load').onclick = () => $('file').click();
$('file').onchange = async (e) => {
  const f = e.target.files[0];
  if (!f) return;
  try {
    const level = JSON.parse(await f.text());
    resetWith(() => loadLevel(M, level));
  } catch (err) {
    say('Load failed: ' + err.message, true);
  }
};
$('play').onclick = () => {
  if (!validate()) return;
  localStorage.setItem('deathgolf.level', JSON.stringify(serializeLevel(M)));
  window.open('index.html?level=local', '_blank');
};

// ---- input
let orbiting = false, sculpting = false, lastX = 0, lastY = 0, lastApply = 0;
const keys = new Set();
let hover = null;

function pick(ev) {
  const r = canvas.getBoundingClientRect();
  const nx = ((ev.clientX - r.left) / r.width) * 2 - 1, ny = 1 - ((ev.clientY - r.top) / r.height) * 2;
  const { o, d } = cam.ray(nx, ny, r.width / r.height);
  if (!M._dg_raycast(o[0], o[1], o[2], d[0], d[1], d[2])) return null;
  return new Float32Array(M.HEAPF32.buffer, M._dg_ray(), 9).slice();
}
function brushCenter(h, radius) {
  const k = tool === 0 ? 0.4 * radius : tool === 1 ? -0.2 * radius : 0;
  return [h[6] + h[3] * k, h[7] + h[4] * k, h[8] + h[5] * k];
}
function apply(ev) {
  const h = pick(ev);
  if (!h) return;
  const radius = +$('radius').value;
  if (typeof tool === 'string') {
    M._dg_place(+tool[1], h[0], h[2]);
    M._dg_mark_dirty();
    validate();
    return;
  }
  const c = brushCenter(h, radius);
  M._dg_brush(tool, c[0], c[1], c[2], radius);
}

canvas.oncontextmenu = (e) => e.preventDefault();
canvas.onmousedown = (e) => {
  lastX = e.clientX;
  lastY = e.clientY;
  if (e.button === 2) orbiting = true;
  else if (e.button === 0) {
    if (typeof tool !== 'string') {
      pushUndo();
      sculpting = true;
    }
    apply(e);
  }
};
window.onmouseup = () => (orbiting = sculpting = false);
canvas.onmousemove = (e) => {
  if (orbiting) cam.drag(e.clientX - lastX, e.clientY - lastY);
  lastX = e.clientX;
  lastY = e.clientY;
  hover = pick(e);
  if (sculpting && performance.now() - lastApply > 50) {
    lastApply = performance.now();
    apply(e);
  }
};
canvas.onwheel = (e) => {
  e.preventDefault();
  cam.zoom(e.deltaY);
};
window.onkeydown = (e) => {
  if (e.target.tagName === 'INPUT') return;
  if (e.key === 'z') undo();
  keys.add(e.key.toLowerCase());
};
window.onkeyup = (e) => keys.delete(e.key.toLowerCase());

// ---- loop
let prev = performance.now();
function frame(now) {
  const dt = Math.min(0.05, (now - prev) / 1000);
  prev = now;
  const [fx, fz] = cam.forward(), sp = 20 * dt;
  const mv = (k) => (keys.has(k) ? 1 : 0);
  const f = mv('w') - mv('s'), r = mv('d') - mv('a');
  cam.target[0] += (fx * f + -fz * r) * sp;
  cam.target[2] += (fz * f + fx * r) * sp;
  if (hover && typeof tool !== 'string') {
    const c = brushCenter(hover, +$('radius').value);
    M._dg_set_cursor(1, c[0], c[1], c[2], +$('radius').value);
  } else M._dg_set_cursor(0, 0, 0, 0, 0);
  renderer.refreshTerrain();
  renderer.refreshEntities(true);
  renderer.draw(cam);
  requestAnimationFrame(frame);
}

M._dg_default_level();
syncParams();
validate();
requestAnimationFrame(frame);
