# RP2350-sketches
RP2350 source code (Pico SDK).

## Sketch Location
- `src/main.c` is the current sketch entry point.

## Build (Pico SDK)
1. Provide Pico SDK in one of these ways:

```bash
# Option A: local checkout inside this repo
git clone https://github.com/raspberrypi/pico-sdk.git --depth=1
git -C pico-sdk submodule update --init

# Option B: global checkout and environment variable
export PICO_SDK_PATH=/absolute/path/to/pico-sdk
```

2. Configure and build:

```bash
cmake -S . -B build -DPICO_BOARD=pico2
cmake --build build -j8
```

Artifacts are generated under `build/`, including UF2 output for flashing.
