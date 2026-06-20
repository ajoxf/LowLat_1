# Build and run the NSE HFT system in a container.
#
# This is for development, testing and BACKTESTING only -- a container shares the
# host kernel and cannot do kernel-bypass or core isolation, so it is NOT a
# production trading target. Production runs on bare-metal colocation hardware.
#
#   docker build -t nse-hft .
#   docker run --rm nse-hft                       # runs the synthetic session
#   docker run --rm nse-hft ctest --test-dir build --output-on-failure
#   docker run --rm nse-hft ./build/tools/backtest
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# -march=native is disabled so the image is portable across host CPUs.
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHFT_NATIVE_ARCH=OFF \
    && cmake --build build -j

# Default: run the synthetic trading session.
CMD ["./build/nse_hft", "config/nse_cm_symbols.csv"]
