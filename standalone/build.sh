#!/bin/bash
set -eu

SELF="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SELF/.." && pwd)"

IMG=${DK3DTEST_IMAGE:-devkitpro/devkita64:latest}

exec docker run --rm \
  -v "$ROOT":/work/dk3dtest \
  -w /work/dk3dtest/standalone \
  --entrypoint bash \
  "$IMG" \
  -c "make $*"
