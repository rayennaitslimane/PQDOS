#!/usr/bin/env bash

set -euo pipefail

MODE=${1:-build}

if [[ "$MODE" == "clean" || "$MODE" == "reset" ]]; then
  echo "==> Resetting build directory..."
  rm -rf build
  [[ "$MODE" == "reset" ]] && {
    echo "Reset completed."
    exit 0
  }
fi

echo "==> Installing dependencies with Conan..."
conan install . \
  --build=missing \
  -s build_type=Release \
  -s compiler.cppstd=20

echo "==> Configuring project with CMake..."
cmake --preset conan-release

echo "==> Building project..."
cmake --build --preset conan-release

echo "Build completed successfully."