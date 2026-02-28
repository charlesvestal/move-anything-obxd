#!/usr/bin/env bash
# Build OB-Xd module for Move Anything (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== OB-Xd Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
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

# Copy files to dist (use cat to avoid ExtFS deallocation issues with Docker)
echo "Packaging..."
cat src/module.json > dist/obxd/module.json
[ -f src/help.json ] && cat src/help.json > dist/obxd/help.json
cat src/ui.js > dist/obxd/ui.js
cat build/dsp.so > dist/obxd/dsp.so
chmod +x dist/obxd/dsp.so

# Copy presets
if [ -d "src/presets" ]; then
    mkdir -p dist/obxd/presets
    for f in src/presets/*.fxb; do
        cat "$f" > "dist/obxd/presets/$(basename "$f")"
    done
fi

# Create tarball for release
cd dist
tar -czvf obxd-module.tar.gz obxd/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/obxd/"
echo "Tarball: dist/obxd-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
