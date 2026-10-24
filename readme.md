# deathgolf

Golf on a voxel cube of earth while a giant worm devours it. The worm runs on a timer, ignores your stroke count, and must eat the hole last.

## Layout
- `src/core.cpp` – C++ core (voxel world, sculpting, meshing, ball/player physics, worm). Built to WASM.
- `web/index.html` – the game. `web/builder.html` – the level builder.
- `tests/core.test.js` – headless checks (sculpt, a winning shot exists, worm eats hole last).

## Run
    ./build.sh                    # needs emsdk (em++) on PATH
    node tests/core.test.js
    cd web && python3 -m http.server 8123

Open `builder.html`, sculpt, place tee/hole/cart/jetpack/worm start, then "Save & Play test".

--

serve-

 gnome-terminal -- /bin/sh -c 'bun server.ts; exec bash'