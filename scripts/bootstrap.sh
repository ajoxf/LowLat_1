#!/usr/bin/env bash
# Bootstrap a fresh Linux VM as a development / research / backtesting box for
# the NSE HFT system. This is the RESEARCH plane (see docs/deployment.md): it is
# fine in the cloud and is NOT a production trading target.
#
# Usage:   ./scripts/bootstrap.sh
# Idempotent: safe to re-run.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "==> NSE HFT bootstrap (research/dev box)"

# 1. Install the toolchain (Debian/Ubuntu). Other distros: install the
#    equivalents of build-essential, cmake>=3.26, git, ninja by hand.
if command -v apt-get >/dev/null 2>&1; then
  echo "==> Installing build dependencies via apt"
  SUDO=""
  if [ "$(id -u)" -ne 0 ]; then SUDO="sudo"; fi
  $SUDO apt-get update -y
  $SUDO apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates ninja-build
else
  echo "!! apt-get not found -- install build-essential, cmake>=3.26, git, ninja manually"
fi

# 2. Show what we have.
echo "==> Toolchain:"
g++ --version | head -1 || true
cmake --version | head -1 || true

# 3. Configure + build (Release). -march=native is ON by default; turn it off
#    here so the same build works if the VM is later resized/migrated.
echo "==> Configuring (Release, portable arch)"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHFT_NATIVE_ARCH=OFF

echo "==> Building"
cmake --build build -j"$(nproc)"

# 4. Run the test suite.
echo "==> Running tests"
ctest --test-dir build --output-on-failure -j"$(nproc)"

# 5. Smoke-test the backtest harness end to end.
echo "==> Generating a synthetic capture and backtesting it"
./build/tools/mtbt_gen /tmp/session.mtbt 100000
./build/tools/backtest /tmp/session.mtbt

cat <<'EOF'

==> Done. Useful next commands:
    ./build/nse_hft config/nse_cm_symbols.csv      # synthetic trading session
    ./build/tools/backtest                          # backtest (self-generates data)
    ./build/benchmarks/bench_pipeline               # tick-to-trade benchmark
    ./build/tools/latency_histogram                 # ASCII latency histogram

Remember: this VM is the RESEARCH plane. The live feed/order path runs only on
bare-metal NSE colocation hardware (see docs/deployment.md).
EOF
