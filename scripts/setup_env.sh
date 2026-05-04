#!/bin/bash
set -e

PROJECT_DIR="${GITHUB_WORKSPACE:-$(pwd)}"
RUNTIME_DIR="$PROJECT_DIR/runtime"
RUNTIME_BUILD_DIR="$PROJECT_DIR/build/perf-ci/runtime"

echo "Installing system dependencies..."
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    curl \
    python3 \
    python3-pip \
    qemu-user \
    qemu-user-static \
    clang \
    binutils-riscv64-linux-gnu \
    gcc-riscv64-linux-gnu \
    g++-riscv64-linux-gnu \
    libc6-dev-riscv64-cross

if ! command -v xmake >/dev/null 2>&1; then
    echo "Installing xmake..."
    curl -fsSL https://xmake.io/shget.text | bash
    if [ -f "$HOME/.local/bin/xmake" ]; then
        export PATH="$HOME/.local/bin:$PATH"
    fi
fi

mkdir -p "$RUNTIME_BUILD_DIR"

echo "Building RISC-V SysY runtime..."
if [ -f "$RUNTIME_DIR/CMakeLists.txt" ]; then
    cmake -S "$RUNTIME_DIR" -B "$RUNTIME_BUILD_DIR/cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=riscv64-linux-gnu-gcc \
        -DCMAKE_AR=riscv64-linux-gnu-ar \
        -DCMAKE_RANLIB=riscv64-linux-gnu-ranlib
    cmake --build "$RUNTIME_BUILD_DIR/cmake" -j"$(nproc)"

    RUNTIME_LIB_CANDIDATE=$(find "$RUNTIME_BUILD_DIR/cmake" -type f \( -name "libsysy.a" -o -name "libsysy_riscv.a" \) | head -n 1 || true)
    if [ -z "$RUNTIME_LIB_CANDIDATE" ]; then
        echo "Error: runtime CMake build finished, but no libsysy.a/libsysy_riscv.a was found."
        exit 1
    fi
    cp "$RUNTIME_LIB_CANDIDATE" "$RUNTIME_DIR/libsysy.a"
elif [ -f "$RUNTIME_DIR/sylib.c" ] && [ -f "$RUNTIME_DIR/sylib.h" ]; then
    riscv64-linux-gnu-gcc -O2 -I"$RUNTIME_DIR" -c "$RUNTIME_DIR/sylib.c" -o "$RUNTIME_BUILD_DIR/sylib.o"
    riscv64-linux-gnu-ar rcs "$RUNTIME_BUILD_DIR/libsysy.a" "$RUNTIME_BUILD_DIR/sylib.o"
    cp "$RUNTIME_BUILD_DIR/libsysy.a" "$RUNTIME_DIR/libsysy.a"
else
    echo "Error: cannot build runtime. Expected runtime/CMakeLists.txt or runtime/sylib.c + runtime/sylib.h."
    exit 1
fi

if [ ! -f "$RUNTIME_DIR/libsysy.a" ]; then
    echo "Error: runtime build failed, $RUNTIME_DIR/libsysy.a not found."
    exit 1
fi

echo "Runtime ready: $RUNTIME_DIR/libsysy.a"
echo "Setup Complete!"
