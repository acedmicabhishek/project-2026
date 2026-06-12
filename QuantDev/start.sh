#!/usr/bin/env bash
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"

echo "=== QuantSim Build & Run ==="

# ── Build ─────────────────────────────────────────────────────────────────────
cmake -S "$ROOT" -B "$BUILD" -DBUILD_PYTHON_BINDINGS=ON \
      -DBUILD_TESTS=OFF --log-level=WARNING -q 2>/dev/null || \
cmake -S "$ROOT" -B "$BUILD" -DBUILD_PYTHON_BINDINGS=ON \
      -DBUILD_TESTS=OFF

# quantsim = desktop app; live_trader = C++-native Binance live (Go Live button).
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" \
      --target quantsim quantsim_core live_trader

# ── Copy C++ Python module to python/ so `import quantsim_core` works ─────────
SO=$(find "$BUILD" -name "quantsim_core*.so" | head -1)
if [ -n "$SO" ]; then
    cp "$SO" "$ROOT/python/"
fi

echo "=== Build OK — launching QuantSim (ImGui) ==="
# Run from repo root so relative paths resolve (examples/, strategies/*.dylib,
# and the spawned ./build/live_trader for crypto-live sessions).
cd "$ROOT"
exec "$BUILD/quantsim"
