#!/bin/bash
# Install OB-Xd module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/obxd" ]; then
    echo "Error: dist/obxd not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing OB-Xd Module ==="

# Deploy to Move
echo "Copying to Move..."
scp -r dist/obxd ableton@move.local:/data/UserData/move-anything/modules/

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/obxd/"
echo ""
echo "Restart Move Anything to load the new module."
