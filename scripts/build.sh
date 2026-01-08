#!/bin/bash
# Build OB-Xd module for Move Anything
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building OB-Xd Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build
mkdir -p dist/obxd

# Compile DSP plugin
echo "Compiling DSP plugin..."
${CROSS_PREFIX}g++ -g -O3 -shared -fPIC -std=c++14 \
    src/dsp/obxd_plugin.cpp \
    -o build/dsp.so \
    -Isrc/dsp \
    -lm

# Copy files to dist
echo "Packaging..."
cp src/module.json dist/obxd/
cp src/ui.js dist/obxd/
cp build/dsp.so dist/obxd/

# Copy patches if they exist
if [ -d "patches" ]; then
    cp -r patches dist/obxd/
fi

echo ""
echo "=== Build Complete ==="
echo "Output: dist/obxd/"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
