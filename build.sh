#!/bin/bash
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
IMG=${DK3DTEST_IMAGE:-devkitpro/devkita64:latest}

usage() {
  echo "Usage: $0 {standalone|core|bundle|clean}" >&2
  exit 1
}

[ $# -ge 1 ] || usage

TARGET="$1"; shift

case "$TARGET" in
  standalone)
    exec docker run --rm \
      -v "$ROOT":/work/dk3dtest \
      -w /work/dk3dtest/standalone \
      "$IMG" \
      bash -c "make $*"
    ;;
  core)
    exec docker run --rm \
      -v "$ROOT":/work/dk3dtest \
      -w /work/dk3dtest/libretro \
      "$IMG" \
      bash -c "make core $*"
    ;;
  bundle)
    exec docker run --rm \
      -v "$ROOT":/work/dk3dtest \
      -w /work/dk3dtest/libretro \
      "$IMG" \
      bash -c "make bundle $*"
    ;;
  clean)
    exec docker run --rm \
      -v "$ROOT":/work/dk3dtest \
      -w /work/dk3dtest \
      "$IMG" \
      bash -c "make -C standalone clean; make -C libretro clean"
    ;;
  *)
    usage
    ;;
esac
