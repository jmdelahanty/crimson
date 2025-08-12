#!/bin/bash
# Build script
mkdir -p build
cd build
cmake ..
make -j$(nproc)
cd ..
echo "Build complete. Run with: ./release/redgui"
