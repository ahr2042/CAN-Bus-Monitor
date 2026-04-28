#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build.sh -- Convenience wrapper around CMake build
#
# Copyright (C) 2026  Ahmad Rashed
#
# Usage:
#   ./scripts/build.sh [debug|release] [clean]
#
# Examples:
#   ./scripts/build.sh           # Debug build
#   ./scripts/build.sh release   # Optimised release build
#   ./scripts/build.sh debug clean  # Clean rebuild
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_TYPE="${1:-Debug}"
CLEAN="${2:-}"

# Normalise build type capitalisation
case "${BUILD_TYPE,,}" in
  debug)   BUILD_TYPE="Debug" ;;
  release) BUILD_TYPE="Release" ;;
  *)       echo "Unknown build type: $BUILD_TYPE" >&2; exit 1 ;;
esac

BUILD_DIR="$REPO_ROOT/build"

if [[ "$CLEAN" == "clean" ]]; then
  echo "Cleaning build directory..."
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "Configuring ($BUILD_TYPE)..."
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo "Building..."
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo ""
echo "Build complete: $BUILD_DIR/canbus_monitor"
echo ""
echo "Run tests with:  cmake --build $BUILD_DIR --target test"
echo "         or:     cd $BUILD_DIR && ctest --output-on-failure"
