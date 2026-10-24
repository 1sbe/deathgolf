// Death Golf core: voxel world, sculpting, meshing, ball/player physics, worm.
// Compiled to WASM with emcc; the JS side only renders and forwards input.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define API extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define API extern "C"
#endif

// The course is 2x3 of the original 32-cube block: 2 wide (x), 3 deep (z).
static const int NX = 64, NY = 32, NZ = 96;
static const int VOL = NX * NY * NZ;
static const float GRAV = 20.f;
static const float SUB_DT = 1.f / 120.f;
static const float WORM_R = 1.2f;
static const float HOLE_PROTECT = 1.6f;

enum { MAT_AIR = 0, MAT_DIRT = 1, MAT_GREEN = 2 };
enum { OBJ_TEE, OBJ_HOLE, OBJ_CART, OBJ_JET, OBJ_WORM, OBJ_COUNT };
enum { ST_PLAY, ST_WON, ST_LOST };

struct V {
  float x, y, z;
};
static V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V operator*(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float len(V a) { return std::sqrt(dot(a, a)); }
static V norm(V a) {
  float l = len(a);
  return l > 1e-6f ? a * (1.f / l) : V{0, 1, 0};
}

// ---------------------------------------------------------------- world
static uint8_t vox[VOL], orig[VOL];
static float objs[OBJ_COUNT][4];  // x, y, z, present
static float params[4] = {3.f, 1.6f, 0, 0};  // par, worm speed
static bool dirty = true;

static inline int vidx(int x, int y, int z) { return (y * NZ + z) * NX + x; }
static inline uint8_t at(int x, int y, int z) {
  if (y < 0) return 1;
  if (x < 0 || x >= NX || z < 0 || z >= NZ || y >= NY) return 0;
  return vox[vidx(x, y, z)];
}
static int topY(int x, int z) {
  for (int y = NY - 1; y >= 0; y--)
    if (at(x, y, z)) return y;
  return -1;
}

static int origTopY(int x, int z) {
  for (int y = NY - 1; y >= 0; y--)
    if (orig[vidx(x, y, z)]) return y;
  return 0;
}

// Smooth solidity: trilinear blend of voxel centres. Surface sits at 0.5.
static float field(V p) {
  float u = p.x - .5f, v = p.y - .5f, w = p.z - .5f;
  int i = (int)std::floor(u), j = (int)std::floor(v), k = (int)std::floor(w);
  float fx = u - i, fy = v - j, fz = w - k, s = 0;
  for (int dz = 0; dz < 2; dz++)
    for (int dy = 0; dy < 2; dy++)
      for (int dx = 0; dx < 2; dx++) {
        float wt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
        if (wt > 0 && at(i + dx, j + dy, k + dz)) s += wt;
      }
  return s;
}
static V fieldGrad(V p) {
  const float h = .5f;
  return {(field({p.x + h, p.y, p.z}) - field({p.x - h, p.y, p.z})),
          (field({p.x, p.y + h, p.z}) - field({p.x, p.y - h, p.z})),
          (field({p.x, p.y, p.z + h}) - field({p.x, p.y, p.z - h}))};
}

// Pushes a sphere of radius r out of the smooth terrain. Returns true if in
// contact; n receives the contact normal and mat the surface material.
static bool resolve(V& p, V& v, float r, float rest, V* nOut, int* mat) {
  const float T = .5f - r;
  bool hit = false;
  for (int it = 0; it < 4; it++) {
    float f = field(p);
    if (f <= T) break;
    V g = fieldGrad(p);
    float gl = len(g);
    V n = gl < 1e-3f ? V{0, 1, 0} : g * (-1.f / gl);
    float pen = gl < 1e-3f ? .4f : std::min(.5f, (f - T) / std::max(gl, .3f));
    p = p + n * pen;
    float vn = dot(v, n);
    if (vn < 0) {
      float e = -vn < 1.5f ? 0.f : rest;
      v = v - n * (vn * (1.f + e));
    }
    if (!hit || n.y > nOut->y) *nOut = n;
    hit = true;
  }
  if (hit && mat) {
    V q = p - *nOut * .6f;
    *mat = at((int)std::floor(q.x), (int)std::floor(q.y), (int)std::floor(q.z));
  }
  return hit;
}

// ---------------------------------------------------------------- level
static void clearWorld() {
  memset(vox, 0, sizeof vox);
  for (int z = 0; z < NZ; z++)
    for (int x = 0; x < NX; x++) vox[vidx(x, 0, z)] = MAT_DIRT;
  memset(objs, 0, sizeof objs);
  params[0] = 3.f;
  params[1] = 1.6f;
  dirty = true;
}

static void place(int type, int x, int z) {
  x = std::max(0, std::min(NX - 1, x));
  z = std::max(0, std::min(NZ - 1, z));
  int t = topY(x, z);
  objs[type][0] = x + .5f;
  objs[type][1] = (float)(t + 1);
  objs[type][2] = z + .5f;
  objs[type][3] = 1;
}

API void dg_default_level() {
  clearWorld();
  static float h[NX][NZ];
  const int hx = NX - 4, hz = NZ - 4, tx = 3, tz = 3;
  for (int z = 0; z < NZ; z++)
    for (int x = 0; x < NX; x++)
      h[x][z] = 7.f + 2.5f * std::sin(x * .25f) * std::cos(z * .2f) + 1.5f * std::sin((x + z) * .15f);
  float hh = std::round(h[hx][hz]), th = std::round(h[tx][tz]);
  for (int z = 0; z < NZ; z++)
    for (int x = 0; x < NX; x++) {
      float dg = std::hypot(x - hx, z - hz), dt = std::hypot(x - tx, z - tz);
      if (dg < 4.5f) h[x][z] = hh;
      else if (dt < 2.5f) h[x][z] = th;
      int top = (int)std::round(h[x][z]);
      for (int y = 1; y <= top; y++) vox[vidx(x, y, z)] = MAT_DIRT;
      if (dg < 3.8f) vox[vidx(x, top, z)] = MAT_GREEN;
    }
  place(OBJ_TEE, tx, tz);
  place(OBJ_HOLE, hx, hz);
  place(OBJ_CART, 14, 22);
  place(OBJ_JET, 36, 56);
  place(OBJ_WORM, NX - 4, 4);
  params[0] = 4.f;
  dirty = true;
}

API void dg_clear() { clearWorld(); }
API uint8_t* dg_voxels() { return vox; }
API int dg_size() { return NX; }
API int dg_size_y() { return NY; }
API int dg_size_z() { return NZ; }
API float* dg_objs() { return &objs[0][0]; }
API void dg_place(int type, int x, int z) {
  if (type >= 0 && type < OBJ_COUNT) place(type, x, z);
}
API void dg_remove(int type) {
  if (type >= 0 && type < OBJ_COUNT) objs[type][3] = 0;
}
API void dg_set_param(int i, float v) {
  if (i >= 0 && i < 4) params[i] = v;
}
API float dg_get_param(int i) { return params[i]; }
API void dg_mark_dirty() { dirty = true; }
API int dg_take_dirty() {
  int d = dirty;
  dirty = false;
  return d;
}
// Returns 0 if the level is playable, otherwise a bitmask of what is missing.
API int dg_validate() {
  int m = 0;
  if (!objs[OBJ_TEE][3]) m |= 1;
  if (!objs[OBJ_HOLE][3]) m |= 2;
  return m;
}

// ---------------------------------------------------------------- sculpting
// tools: 0 raise, 1 lower, 2 smooth, 3 flatten, 4 paint green, 5 paint dirt
API void dg_brush(int tool, float cx, float cy, float cz, float r) {
  int x0 = std::max(0, (int)std::floor(cx - r)), x1 = std::min(NX - 1, (int)std::ceil(cx + r));
  int y0 = std::max(1, (int)std::floor(cy - r)), y1 = std::min(NY - 1, (int)std::ceil(cy + r));
  int z0 = std::max(0, (int)std::floor(cz - r)), z1 = std::min(NZ - 1, (int)std::ceil(cz + r));
  static uint8_t snap[VOL];
  if (tool == 2) memcpy(snap, vox, sizeof vox);
  int level = (int)std::floor(cy);
  for (int z = z0; z <= z1; z++)
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        float dx = x + .5f - cx, dy = y + .5f - cy, dz = z + .5f - cz;
        uint8_t& c = vox[vidx(x, y, z)];
        switch (tool) {
          case 0:
            if (dx * dx + dy * dy + dz * dz <= r * r && !c) c = MAT_DIRT;
            break;
          case 1:
            if (dx * dx + dy * dy + dz * dz <= r * r) c = 0;
            break;
          case 2: {
            if (dx * dx + dy * dy + dz * dz > r * r) break;
            int cnt = 0;
            for (int a = -1; a <= 1; a++)
              for (int b = -1; b <= 1; b++)
                for (int d = -1; d <= 1; d++) {
                  int xx = x + d, yy = y + a, zz = z + b;
                  if (xx < 0 || xx >= NX || zz < 0 || zz >= NZ) continue;
                  if (yy < 0 || (yy < NY && snap[vidx(xx, yy, zz)])) cnt++;
                }
            if (cnt >= 14) { if (!c) c = MAT_DIRT; }
            else c = 0;
            break;
          }
          case 3:
            if (dx * dx + dz * dz > r * r) break;
            if (y <= level) { if (!c) c = MAT_DIRT; }
            else c = 0;
            break;
          case 4:
          case 5:
            if (dx * dx + dy * dy + dz * dz <= r * r && c && !at(x, y + 1, z)) c = tool == 4 ? MAT_GREEN : MAT_DIRT;
            break;
        }
      }
  // Objects follow the terrain they stand on.
  for (int t = 0; t < OBJ_COUNT; t++)
    if (objs[t][3]) place(t, (int)objs[t][0], (int)objs[t][2]);
  dirty = true;
}

static float ray[9];
API float* dg_ray() { return ray; }
// Voxel DDA. On hit fills ray: cell xyz, normal xyz, point xyz.
API int dg_raycast(float ox, float oy, float oz, float dx, float dy, float dz) {
  V o{ox, oy, oz}, d = norm({dx, dy, dz});
  float tmin = 0, tmax = 1e9f;
  float oa[3] = {o.x, o.y, o.z}, da[3] = {d.x, d.y, d.z};
  const int dims[3] = {NX, NY, NZ};
  int entry = -1;
  for (int a = 0; a < 3; a++) {
    if (std::fabs(da[a]) < 1e-9f) {
      if (oa[a] < 0 || oa[a] > dims[a]) return 0;
      continue;
    }
    float t1 = (0 - oa[a]) / da[a], t2 = (dims[a] - oa[a]) / da[a];
    if (t1 > t2) std::swap(t1, t2);
    if (t1 > tmin) { tmin = t1; entry = a; }
    tmax = std::min(tmax, t2);
  }
  if (tmin > tmax) return 0;
  float t = tmin + 1e-4f;
  float pa[3] = {oa[0] + da[0] * t, oa[1] + da[1] * t, oa[2] + da[2] * t};
  int c[3], step[3];
  float tn[3], td[3];
  for (int a = 0; a < 3; a++) {
    c[a] = std::max(0, std::min(dims[a] - 1, (int)std::floor(pa[a])));
    step[a] = da[a] > 0 ? 1 : -1;
    td[a] = std::fabs(da[a]) < 1e-9f ? 1e9f : std::fabs(1.f / da[a]);
    float edge = da[a] > 0 ? c[a] + 1 : c[a];
    tn[a] = std::fabs(da[a]) < 1e-9f ? 1e9f : t + (edge - pa[a]) / da[a];
  }
  int axis = entry;
  for (int i = 0; i < 256; i++) {
    if (c[0] < 0 || c[0] >= NX || c[1] < 0 || c[1] >= NY || c[2] < 0 || c[2] >= NZ) return 0;
    if (vox[vidx(c[0], c[1], c[2])]) {
      for (int a = 0; a < 3; a++) {
        ray[a] = (float)c[a];
        ray[3 + a] = a == axis ? (float)-step[a] : 0.f;
        ray[6 + a] = oa[a] + da[a] * t;
      }
      return 1;
    }
    axis = tn[0] < tn[1] ? (tn[0] < tn[2] ? 0 : 2) : (tn[1] < tn[2] ? 1 : 2);
    t = tn[axis];
    c[axis] += step[axis];
    tn[axis] += td[axis];
  }
  return 0;
}

// ---------------------------------------------------------------- game state
struct Worm {
  V head, dir;
  V wp[10];
  int nwp = 0, cur = 0;
  std::vector<V> trail;
  float sinceTrail = 0;
  bool final_ = false;
  float ph[3] = {0, 0, 0};
} worm;

static V ball, ballV, lastRest, player, playerV;
static bool ballResting, grounded, mounted, hasJet;
static float restTimer, fuel, gameTime;
static int strokes, status;
static float inMx, inMz;
static bool inJump, inThrust;
static float aimOn, aimDx, aimDz, aimPow;
static int penalties;
static int runCount;
static const float BALL_R = .35f, PLAYER_R = .45f;
static const float SEG_SPACING = 1.0f;
static const int SEGS = 9;

static float segRadius(int k) { return WORM_R - .06f * k; }
static V segCur[SEGS], segPrev[SEGS];
static bool ballOnWorm;
// Point `d` units behind the head, measured along the worm's trail.
static V pointBehind(float d) {
  V prev = worm.head;
  for (int i = (int)worm.trail.size() - 1; i >= 0; i--) {
    V q = worm.trail[i];
    float l = len(q - prev);
    if (l >= d && l > 1e-6f) return prev + (q - prev) * (d / l);
    d -= l;
    prev = q;
  }
  return prev;
}
static void computeSegs() {
  segCur[0] = worm.head;
  for (int k = 1; k < SEGS; k++) segCur[k] = pointBehind(k * SEG_SPACING);
}
static V segPos(int k) { return segCur[k]; }

static unsigned rngState;
static float rnd() {
  rngState = rngState * 1664525u + 1013904223u;
  return (rngState >> 8) / 16777216.f;
}

// The worm rides just below the original surface so its back sticks out and it eats a trench.
static float wormSurfaceY(V p) {
  int x = std::max(0, std::min(NX - 1, (int)p.x)), z = std::max(0, std::min(NZ - 1, (int)p.z));
  return (float)origTopY(x, z) + .5f;
}

static void setupWorm() {
  V hole{objs[OBJ_HOLE][0], objs[OBJ_HOLE][1] - 1.f, objs[OBJ_HOLE][2]};
  worm.head = objs[OBJ_WORM][3] ? V{objs[OBJ_WORM][0], objs[OBJ_WORM][1], objs[OBJ_WORM][2]} : V{2, 8, 2};
  rngState = (unsigned)(worm.head.x * 73 + worm.head.z * 151 + hole.x * 31 + hole.z * 17 + 7) + 7919u * (unsigned)runCount++;
  const int K = 8;
  // Random order and heights; the hole is always the final waypoint.
  // Waypoints sit inside the earth so the worm tunnels instead of flying overhead.
  for (int i = 0; i < K; i++) {
    float x = 3 + rnd() * (NX - 6), z = 3 + rnd() * (NZ - 6);
    float top = (float)std::max(3, topY((int)x, (int)z));
    worm.wp[i] = {x, top * (.6f + .4f * rnd()), z};
  }
  for (float& p : worm.ph) p = rnd() * 6.28f;
  worm.wp[K] = {hole.x, 0, hole.z};
  worm.nwp = K + 1;
  worm.cur = 0;
  worm.final_ = false;
  worm.dir = norm(V{worm.wp[0].x - worm.head.x, 0, worm.wp[0].z - worm.head.z});
  worm.head.y = wormSurfaceY(worm.head);
  // Pre-fill the trail so the body starts stretched out behind the head.
  worm.trail.clear();
  for (int i = 40; i >= 1; i--) worm.trail.push_back(worm.head - worm.dir * (.4f * i));
  worm.sinceTrail = 0;
  computeSegs();
  memcpy(segPrev, segCur, sizeof segCur);
}

static void respawnBall(bool penalty) {
  if (penalty) strokes++, penalties++;
  V s = lastRest;
  bool ok = field({s.x, s.y - 1.f, s.z}) > .3f && field(s) < .3f;
  if (!ok) s = {objs[OBJ_TEE][0], objs[OBJ_TEE][1] + BALL_R + .3f, objs[OBJ_TEE][2]};
  ball = s;
  ballV = {0, 0, 0};
  ballResting = false;
  restTimer = 0;
}

static void initGame() {
  V tee{objs[OBJ_TEE][0], objs[OBJ_TEE][1], objs[OBJ_TEE][2]};
  ball = {tee.x, tee.y + BALL_R + .3f, tee.z};
  lastRest = ball;
  ballV = {0, 0, 0};
  ballResting = false;
  ballOnWorm = false;
  restTimer = 0;
  float px = tee.x + 1.5f, pz = tee.z;
  player = {px, (float)topY((int)px, (int)pz) + 2.f, pz};
  playerV = {0, 0, 0};
  mounted = hasJet = false;
  fuel = 3.f;
  gameTime = 0;
  strokes = penalties = 0;
  status = ST_PLAY;
  inMx = inMz = 0;
  inJump = inThrust = false;
  aimOn = 0;
  setupWorm();
  dirty = true;
}

API void dg_start_game() {
  memcpy(orig, vox, sizeof vox);
  initGame();
}
API void dg_restart() {
  memcpy(vox, orig, sizeof vox);
  initGame();
}

static bool carve(V c, float R) {
  bool ch = false;
  int x0 = std::max(0, (int)std::floor(c.x - R)), x1 = std::min(NX - 1, (int)std::ceil(c.x + R));
  int y0 = std::max(1, (int)std::floor(c.y - R)), y1 = std::min(NY - 1, (int)std::ceil(c.y + R));
  int z0 = std::max(0, (int)std::floor(c.z - R)), z1 = std::min(NZ - 1, (int)std::ceil(c.z + R));
  for (int z = z0; z <= z1; z++)
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        uint8_t& cell = vox[vidx(x, y, z)];
        if (!cell) continue;
        float dx = x + .5f - c.x, dy = y + .5f - c.y, dz = z + .5f - c.z;
        if (dx * dx + dy * dy + dz * dz > R * R) continue;
        if (!worm.final_) {
          float hx = x + .5f - objs[OBJ_HOLE][0], hz = z + .5f - objs[OBJ_HOLE][2];
          if (hx * hx + hz * hz < HOLE_PROTECT * HOLE_PROTECT) continue;
        }
        cell = 0;
        ch = true;
      }
  return ch;
}

static void stepWorm(float dt) {
  memcpy(segPrev, segCur, sizeof segCur);
  V target = worm.wp[worm.cur];
  V to{target.x - worm.head.x, 0, target.z - worm.head.z};
  float d = len(to);
  bool last = worm.cur == worm.nwp - 1;
  worm.final_ = last;
  if (last ? d < 1.4f : d < 1.8f) {
    if (last) {
      status = ST_LOST;
      return;
    }
    worm.cur++;
    return;
  }
  // Weave around the straight line to the waypoint until close to it.
  V want = to * (1.f / d);
  if (d > 5.f) {
    float wv = std::min(1.f, (d - 5.f) / 5.f) * .9f;
    want = norm(want + V{std::sin(gameTime * 1.3f + worm.ph[0]), 0, std::cos(gameTime * 1.1f + worm.ph[2])} * wv);
  }
  float turn = std::min(1.f, 2.2f * dt);
  worm.dir = norm(worm.dir + (want - worm.dir) * turn);
  worm.dir.y = 0;
  worm.dir = norm(worm.dir);
  float dist = params[1] * dt;
  worm.head = worm.head + worm.dir * dist;
  worm.head.x = std::max(1.f, std::min(NX - 1.f, worm.head.x));
  worm.head.z = std::max(1.f, std::min(NZ - 1.f, worm.head.z));
  worm.head.y += (wormSurfaceY(worm.head) - worm.head.y) * std::min(1.f, 6.f * dt);
  worm.sinceTrail += dist;
  if (worm.sinceTrail >= .4f) {
    worm.sinceTrail = 0;
    worm.trail.push_back(worm.head);
    if (worm.trail.size() > 200) worm.trail.erase(worm.trail.begin(), worm.trail.begin() + 100);
  }
  computeSegs();
  if (carve(worm.head, WORM_R)) dirty = true;
  for (int t = OBJ_CART; t <= OBJ_JET; t++)
    if (objs[t][3] && len(V{objs[t][0], objs[t][1], objs[t][2]} - worm.head) < WORM_R + .5f) {
      objs[t][3] = 0;
      if (t == OBJ_CART) mounted = false;
    }
  // Anything resting on the worm's back travels with it.
  auto carry = [&](V& p, float rad) {
    int best = -1;
    float bestGap = .12f;
    for (int k = 0; k < SEGS; k++) {
      float gap = len(p - segCur[k]) - (segRadius(k) + rad);
      if (p.y > segCur[k].y + .3f * segRadius(k) && gap < bestGap) best = k, bestGap = gap;
    }
    if (best >= 0) p = p + (segCur[best] - segPrev[best]);
  };
  carry(ball, BALL_R);
  carry(player, PLAYER_R);
  // Only the mouth is deadly: the player dies, the ball goes back to its last safe spot.
  V mouth = worm.head + worm.dir * .9f;
  if (len(player - mouth) < .9f) status = ST_LOST;
  if (len(ball - mouth) < .8f) respawnBall(true);
}

// Pushes a sphere out of the worm's body. Returns true if standing on its back.
static bool resolveWorm(V& p, V& v, float rad, float rest, float minNy) {
  bool on = false;
  for (int k = 0; k < SEGS; k++) {
    V d = p - segCur[k];
    float r = segRadius(k) + rad, l = len(d);
    if (l >= r) continue;
    V n = l < 1e-4f ? V{0, 1, 0} : d * (1.f / l);
    // On top: lift straight up so standing objects don't creep down the curved back.
    if (n.y > minNy) p.y = segCur[k].y + std::sqrt(std::max(0.f, r * r - d.x * d.x - d.z * d.z));
    else p = segCur[k] + n * r;
    float vn = dot(v, n);
    if (vn < 0) v = v - n * (vn * (1.f + (-vn < 1.5f ? 0.f : rest)));
    if (n.y > minNy) on = true;
  }
  return on;
}

static void stepBall(float dt) {
  ballV.y -= GRAV * dt;
  ball = ball + ballV * dt;
  V n{0, 1, 0};
  int mat = 0;
  float vyBefore = ballV.y;
  if (resolve(ball, ballV, BALL_R, .45f, &n, &mat) && n.y > .5f) {
    // Landing bites into the ground, killing some sideways speed.
    if (vyBefore < -2.f) {
      float bite = mat == MAT_GREEN ? .8f : .6f;
      ballV.x *= bite;
      ballV.z *= bite;
    }
    float drag = mat == MAT_GREEN ? .9f : 2.6f;
    float k = std::exp(-drag * dt);
    ballV.x *= k;
    ballV.z *= k;
  }
  ballOnWorm = resolveWorm(ball, ballV, BALL_R, .45f, .5f);
  if (ballOnWorm) {
    float k = std::exp(-6.f * dt);
    ballV.x *= k;
    ballV.z *= k;
    ballResting = false;
  }
  float sp = len(ballV);
  if (sp < .35f && !ballOnWorm) restTimer += dt;
  else restTimer = 0;
  if (ballResting) {
    if (sp > .8f) ballResting = false;
    else ballV = {0, std::min(0.f, ballV.y), 0};
  } else if (restTimer > .3f) {
    ballResting = true;
    ballV = {0, 0, 0};
    lastRest = ball;
  }
  if (ball.y < -3 || ball.x < -3 || ball.x > NX + 3 || ball.z < -3 || ball.z > NZ + 3) respawnBall(true);
  // Cup check: slow enough and low enough over the hole.
  if (objs[OBJ_HOLE][3]) {
    float dx = ball.x - objs[OBJ_HOLE][0], dz = ball.z - objs[OBJ_HOLE][2];
    if (dx * dx + dz * dz < .75f * .75f && ball.y < objs[OBJ_HOLE][1] + .9f && sp < 9.f) status = ST_WON;
  }
}

static void stepPlayer(float dt) {
  float speed = mounted ? 10.f : 4.5f;
  V want{inMx * speed, 0, inMz * speed};
  if (grounded) {
    playerV.x = want.x;
    playerV.z = want.z;
    if (inJump) playerV.y = 7.5f;
  } else {
    float k = std::min(1.f, 12.f * dt);
    playerV.x += (want.x - playerV.x) * k;
    playerV.z += (want.z - playerV.z) * k;
  }
  playerV.y -= GRAV * dt;
  if (hasJet && inThrust && fuel > 0) {
    playerV.y += 42.f * dt;
    playerV.y = std::min(playerV.y, 8.f);
    fuel -= dt;
  } else if (grounded) {
    fuel = std::min(3.f, fuel + 1.5f * dt);
  }
  player = player + playerV * dt;
  V n{0, 1, 0};
  grounded = resolve(player, playerV, PLAYER_R, 0.f, &n, nullptr) && n.y > .55f;
  grounded = resolveWorm(player, playerV, PLAYER_R, 0.f, .55f) || grounded;
  player.x = std::max(-2.f, std::min(NX + 2.f, player.x));
  player.z = std::max(-2.f, std::min(NZ + 2.f, player.z));
  if (player.y < -3) {
    player = {ball.x, ball.y + 2.f, ball.z};
    playerV = {0, 0, 0};
    strokes++, penalties++;
  }
}

API void dg_step(float dt) {
  dt = std::min(dt, .1f);
  static float acc = 0;
  acc += dt;
  while (acc >= SUB_DT && status == ST_PLAY) {
    acc -= SUB_DT;
    gameTime += SUB_DT;
    stepWorm(SUB_DT);
    if (status != ST_PLAY) break;
    stepBall(SUB_DT);
    stepPlayer(SUB_DT);
  }
  if (status != ST_PLAY) acc = 0;
}

API void dg_input(float mx, float mz, int jump, int thrust) {
  inMx = mx;
  inMz = mz;
  inJump = jump;
  inThrust = thrust;
}
static std::vector<V> aimPts;
static V shotVel(float dx, float dz, float power, int club) {
  static const float loft[3] = {.06f, .5f, .95f}, mult[3] = {.6f, .95f, 1.f};
  float l = std::sqrt(dx * dx + dz * dz);
  dx /= l;
  dz /= l;
  club = std::max(0, std::min(2, club));
  float sp = (4.f + std::max(0.f, std::min(1.f, power)) * 22.f) * mult[club];
  return {dx * std::cos(loft[club]) * sp, std::sin(loft[club]) * sp, dz * std::cos(loft[club]) * sp};
}
// Traces the flight up to the first landing so the aimer can draw an arc.
API void dg_set_aim(int on, float dx, float dz, float power, int club) {
  aimOn = (float)on;
  aimPts.clear();
  if (!on || dx * dx + dz * dz < 1e-12f) return;
  V p = ball, v = shotVel(dx, dz, power, club);
  p.y += .05f;
  // Same step size and gravity as the real ball; stops at first touchdown.
  const float dt = SUB_DT;
  for (int i = 0; i < 1200; i++) {
    v.y -= GRAV * dt;
    p = p + v * dt;
    V n{0, 1, 0};
    if (v.y < 0 && resolve(p, v, BALL_R, 0.f, &n, nullptr)) {
      aimPts.push_back(p);
      break;
    }
    if (i % 4 == 0) aimPts.push_back(p);
    if (p.y < -3 || p.x < -3 || p.x > NX + 3 || p.z < -3 || p.z > NZ + 3) break;
  }
}

API int dg_can_shoot() {
  return status == ST_PLAY && (ballResting || ballOnWorm) && len(player - ball) < 3.5f;
}
// club 0 putter, 1 iron, 2 wedge. power 0..1.
API int dg_shoot(float dx, float dz, float power, int club) {
  if (!dg_can_shoot()) return 0;
  if (dx * dx + dz * dz < 1e-12f) return 0;
  ballV = shotVel(dx, dz, power, club);
  ball.y += .05f;
  ballResting = false;
  restTimer = 0;
  strokes++;
  return 1;
}

// Context action: mount/dismount cart, pick up jetpack.
API void dg_interact() {
  if (status != ST_PLAY) return;
  if (mounted) {
    mounted = false;
    objs[OBJ_CART][0] = player.x;
    objs[OBJ_CART][1] = player.y - PLAYER_R;
    objs[OBJ_CART][2] = player.z;
    return;
  }
  V c{objs[OBJ_CART][0], objs[OBJ_CART][1], objs[OBJ_CART][2]};
  V j{objs[OBJ_JET][0], objs[OBJ_JET][1], objs[OBJ_JET][2]};
  if (objs[OBJ_CART][3] && len(c - player) < 2.5f) mounted = true;
  else if (objs[OBJ_JET][3] && len(j - player) < 2.5f) {
    hasJet = true;
    objs[OBJ_JET][3] = 0;
  }
}

API int dg_status() { return status; }
API int dg_strokes() { return strokes; }
API float dg_time() { return gameTime; }
API int dg_mounted() { return mounted; }
API int dg_has_jet() { return hasJet; }
API float dg_fuel() { return fuel; }
API int dg_worm_final() { return worm.final_; }
API float dg_worm_hole_dist() {
  return len(worm.head - V{objs[OBJ_HOLE][0], objs[OBJ_HOLE][1], objs[OBJ_HOLE][2]});
}
// Rough seconds until the worm reaches the hole, following remaining waypoints.
API float dg_worm_eta() {
  float d = 0;
  V p = worm.head;
  for (int i = worm.cur; i < worm.nwp; i++) d += len(worm.wp[i] - p), p = worm.wp[i];
  return d / std::max(.1f, params[1]);
}
// Worm segments as x, y, z, radius (used by tests and tooling).
API float* dg_worm_segs() {
  static float o[SEGS * 4];
  for (int k = 0; k < SEGS; k++) o[k * 4] = segCur[k].x, o[k * 4 + 1] = segCur[k].y, o[k * 4 + 2] = segCur[k].z, o[k * 4 + 3] = segRadius(k);
  return o;
}
API void dg_teleport(int what, float x, float y, float z) {
  V* t = what ? &ball : &player;
  *t = {x, y, z};
  (what ? ballV : playerV) = {0, 0, 0};
  if (what) ballResting = false;
}
API float* dg_ball() { static float b[12]; b[0]=ball.x; b[1]=ball.y; b[2]=ball.z; b[3]=player.x; b[4]=player.y; b[5]=player.z; b[6]=ballResting; b[7]=worm.head.x; b[8]=worm.head.y; b[9]=worm.head.z; return b; }

// ---------------------------------------------------------------- meshing
static std::vector<float> tmesh, emesh;
static const int FC[6][4][3] = {
    {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}, {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},
    {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}, {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}, {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}};
static const int FD[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
static const float SHADE[6] = {.82f, .7f, 1.f, .5f, .76f, .64f};

static void pushV(std::vector<float>& m, float x, float y, float z, float r, float g, float b) {
  m.push_back(x); m.push_back(y); m.push_back(z);
  m.push_back(r); m.push_back(g); m.push_back(b);
}
static void quad(std::vector<float>& m, const float p[4][3], float r, float g, float b) {
  for (int i : {0, 1, 2, 0, 2, 3}) pushV(m, p[i][0], p[i][1], p[i][2], r, g, b);
}
static void box(std::vector<float>& m, float cx, float cy, float cz, float hx, float hy, float hz, float r, float g, float b) {
  for (int f = 0; f < 6; f++) {
    float p[4][3];
    for (int i = 0; i < 4; i++) {
      p[i][0] = cx + (FC[f][i][0] * 2 - 1) * hx;
      p[i][1] = cy + (FC[f][i][1] * 2 - 1) * hy;
      p[i][2] = cz + (FC[f][i][2] * 2 - 1) * hz;
    }
    quad(m, p, r * SHADE[f], g * SHADE[f], b * SHADE[f]);
  }
}

static void sphere(std::vector<float>& m, float cx, float cy, float cz, float r, float cr, float cg, float cb) {
  const int LAT = 8, LON = 12;
  auto pt = [&](int i, int j, float* o, float* sh) {
    float a = (float)M_PI * i / LAT, b = 2.f * (float)M_PI * j / LON;
    float nx = std::sin(a) * std::cos(b), ny = std::cos(a), nz = std::sin(a) * std::sin(b);
    o[0] = cx + nx * r; o[1] = cy + ny * r; o[2] = cz + nz * r;
    *sh = .6f + .4f * (nx * .35f + ny * .8f + nz * .45f > 0 ? nx * .35f + ny * .8f + nz * .45f : 0.f) + .1f;
  };
  for (int i = 0; i < LAT; i++)
    for (int j = 0; j < LON; j++) {
      float p[4][3], sh[4];
      pt(i, j, p[0], &sh[0]); pt(i + 1, j, p[1], &sh[1]); pt(i + 1, j + 1, p[2], &sh[2]); pt(i, j + 1, p[3], &sh[3]);
      for (int k : {0, 1, 2, 0, 2, 3}) pushV(m, p[k][0], p[k][1], p[k][2], cr * sh[k], cg * sh[k], cb * sh[k]);
    }
}

API int dg_terrain_mesh() {
  tmesh.clear();
  for (int y = 0; y < NY; y++)
    for (int z = 0; z < NZ; z++)
      for (int x = 0; x < NX; x++) {
        uint8_t m = vox[vidx(x, y, z)];
        if (!m) continue;
        float jit = ((x + y + z) & 1) ? 1.f : .94f;
        for (int f = 0; f < 6; f++) {
          if (at(x + FD[f][0], y + FD[f][1], z + FD[f][2])) continue;
          float r, g, b;
          if (f == 2) {
            if (m == MAT_GREEN) r = .42f, g = .86f, b = .36f;
            else r = .27f, g = .58f, b = .2f;
          } else r = .5f, g = .34f, b = .2f;
          float s = SHADE[f] * jit;
          float p[4][3];
          for (int i = 0; i < 4; i++) {
            p[i][0] = (float)(x + FC[f][i][0]);
            p[i][1] = (float)(y + FC[f][i][1]);
            p[i][2] = (float)(z + FC[f][i][2]);
          }
          quad(tmesh, p, r * s, g * s, b * s);
        }
      }
  return (int)(tmesh.size() / 6);
}
API float* dg_terrain_ptr() { return tmesh.data(); }

static float curOn, curX, curY, curZ, curR;
API void dg_set_cursor(int on, float x, float y, float z, float r) {
  curOn = (float)on; curX = x; curY = y; curZ = z; curR = r;
}

static void flag(std::vector<float>& m, const float* o, float r, float g, float b) {
  box(m, o[0], o[1] + .01f, o[2], .5f, .02f, .5f, .05f, .05f, .05f);
  box(m, o[0], o[1] + 1.2f, o[2], .05f, 1.2f, .05f, .95f, .95f, .95f);
  box(m, o[0] + .4f, o[1] + 2.0f, o[2], .4f, .25f, .03f, r, g, b);
}

API int dg_entities_mesh(int editor) {
  std::vector<float>& m = emesh;
  m.clear();
  const float* t = objs[OBJ_TEE];
  const float* h = objs[OBJ_HOLE];
  const float* c = objs[OBJ_CART];
  const float* j = objs[OBJ_JET];
  const float* w = objs[OBJ_WORM];
  if (t[3]) box(m, t[0], t[1] + .1f, t[2], .25f, .1f, .25f, 1.f, 1.f, 1.f);
  if (h[3]) flag(m, h, .9f, .15f, .15f);
  if (c[3] && !(!editor && mounted)) box(m, c[0], c[1] + .4f, c[2], .9f, .35f, .6f, .95f, .8f, .1f);
  if (j[3]) box(m, j[0], j[1] + .4f, j[2], .3f, .4f, .2f, .9f, .25f, .25f);
  if (editor) {
    if (w[3]) box(m, w[0], w[1] + .7f, w[2], .7f, .7f, .7f, .95f, .5f, .6f);
    if (curOn) {
      const float s[3] = {1.f, .9f, .1f};
      for (int a = 0; a < 3; a++)
        for (int sg = -1; sg <= 1; sg += 2) {
          float o[3] = {curX, curY, curZ};
          o[a] += sg * curR;
          box(m, o[0], o[1], o[2], .12f, .12f, .12f, s[0], s[1], s[2]);
        }
    }
    return (int)(m.size() / 6);
  }
  sphere(m, ball.x, ball.y, ball.z, BALL_R, 1.f, 1.f, 1.f);
  float py = player.y;
  box(m, player.x, py - .05f, player.z, .3f, .4f, .2f, .2f, .35f, .85f);
  box(m, player.x, py + .5f, player.z, .2f, .15f, .2f, .95f, .8f, .65f);
  if (mounted) box(m, player.x, py - .5f, player.z, .9f, .25f, .6f, .95f, .8f, .1f);
  if (hasJet) box(m, player.x - .0f, py + .05f, player.z + .0f, .1f, .3f, .35f, .9f, .25f, .25f);
  if (aimOn)
    for (size_t i = 0; i < aimPts.size(); i++) {
      bool end = i + 1 == aimPts.size();
      float s = end ? .4f : .17f;
      box(m, aimPts[i].x, aimPts[i].y, aimPts[i].z, s, s, s, 1.f, end ? .4f : .9f, .1f);
    }
  for (int k = SEGS - 1; k >= 0; k--) {
    V s = segPos(k);
    float r = segRadius(k);
    float q = k % 2 ? .85f : 1.f;
    if (k == 0) box(m, s.x, s.y, s.z, r, r, r, .95f, .35f, .45f);
    else box(m, s.x, s.y, s.z, r * .95f, r * .95f, r * .95f, .85f * q, .45f * q, .55f * q);
  }
  return (int)(m.size() / 6);
}
API float* dg_entities_ptr() { return emesh.data(); }

API void dg_init() { dg_default_level(); }
