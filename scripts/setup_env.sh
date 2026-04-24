#!/bin/bash
set -e

PROJECT_DIR=$(pwd)

echo "Installing system dependencies..."
sudo apt-get update
sudo apt-get install -y build-essential cmake curl python3 python3-pip

if ! command -v xmake >/dev/null 2>&1; then
    echo "Installing xmake..."
    curl -fsSL https://xmake.io/shget.text | bash
    if [ -f "$HOME/.local/bin/xmake" ]; then
        export PATH="$HOME/.local/bin:$PATH"
    fi
fi

INSTALL_DIR="$HOME/.local/opt/compiler-dev"
mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

echo "Building sysy-runtime-lib for RISC-V..."
if [ -d "$PROJECT_DIR/sysy-runtime-lib" ]; then
    cd "$PROJECT_DIR/sysy-runtime-lib"
    mkdir -p build && cd build
    # Build for RISC-V cross compiling
    rm -rf *
    cmake -DCMAKE_C_COMPILER=riscv64-linux-gnu-gcc .. && make
    cp libsysy.a "$INSTALL_DIR/lib/libsysy.a"
    cd "$PROJECT_DIR"
else
    echo "sysy-runtime-lib not found in $PROJECT_DIR, skip runtime-lib build."
fi

echo "Setup Complete!"
echo "Add the following to your ~/.bashrc or ~/.zshrc:"
echo "export CDE_LIBRARY_PATH=$INSTALL_DIR/lib"
echo "export CDE_INCLUDE_PATH=$INSTALL_DIR/include"
