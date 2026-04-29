# winvnc

A Windows VNC server built on [neatvnc](https://github.com/any1/neatvnc) and
[aml](https://github.com/any1/aml). Captures the desktop with DXGI Desktop
Duplication, encodes via Tight/JPEG, and injects mouse and keyboard input
through `SendInput`.

Used by [Raspberry Pi Connect](https://www.raspberrypi.com/software/connect/)
on Windows; spawned by `connectd.exe` over stdin/stdout (`--stdio` mode) so no
TCP port is exposed.

## Layout

```
.                           ← repo root
├── src/, include/, test/, meson.build   ← winvnc source
├── build-win.sh, win64-cross.ini        ← MinGW cross-build orchestrator
├── neatvnc/                             ← submodule (ghollingworth/neatvnc, windows branch)
├── aml/                                 ← submodule (ghollingworth/aml,     windows branch)
└── deps/{libjpeg-turbo,pixman,zlib}/    ← submodules (upstream, pinned commits)
```

## Building (cross-compile from Linux)

Requires MinGW-w64 (`x86_64-w64-mingw32-gcc`) and Meson.

```sh
git clone --recurse-submodules https://github.com/ghollingworth/winvnc.git
cd winvnc

# Build the static dependencies into ./prefix (one-time)
# … see deps/ subdirectories for instructions per library …

# Build winvnc.exe
./build-win.sh
```

The result lands at `build/winvnc.exe`.

## CLI

```
winvnc [-p PORT] [-a ADDR] [--stdio]

  -p PORT     Listen port (default: 5900)
  -a ADDR     Listen address (default: 127.0.0.1)
  --stdio     Bridge VNC protocol to stdin/stdout for embedding in a parent
              process. No TCP port is exposed.
```

## Licence

ISC. The bundled-via-submodule projects are licensed individually:

- neatvnc — ISC
- aml — ISC
- pixman — MIT
- libjpeg-turbo — BSD-style
- zlib — zlib licence
