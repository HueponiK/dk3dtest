# dk3dtest

Spinning cube rendered with deko3d on Nintendo Switch, available as a
standalone NRO and as a libretro core for RetroArch.

## Prerequisites

- Docker (builds inside `devkitpro/devkita64:latest`)

## Build

```
# Standalone NRO
./build.sh standalone

# Libretro core only (static library)
./build.sh core

# Libretro bundled NRO (core + RetroArch)
./build.sh bundle

# Clean all
./build.sh clean

# Use a custom Docker image
DK3DTEST_IMAGE=myimage:tag ./build.sh standalone
```

## Outputs

| Target | File |
|--------|------|
| Standalone | `standalone/dk3dtest_standalone.nro` |
| Libretro core | `libretro/dk3dtest_libretro.a` |
| Libretro bundle | `libretro/dk3dtest_libretro_libnx.nro` |

## How it works

Renders a 320×240 spinning cube into an offscreen DkImage, then presents
it to screen (standalone) or hands it to RetroArch via the deko3d HW
render interface (libretro).
