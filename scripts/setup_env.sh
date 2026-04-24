#!/bin/bash
set -e

PROJECT_DIR=$(pwd)

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

echo "Setup Complete!"
