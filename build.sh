#!/bin/bash

set -e  # Exit immediately if any command fails

# Move to build directory (create if it doesn't exist)
BUILD_DIR="build"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Creating build directory..."
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

echo "Running CMake configuration..."
cmake -DDU_SPLIT_TYPE=SPLIT_7_2 \
      -DENABLE_DPDK=True \
      -DASSERT_LEVEL=MINIMAL \
      ../

echo "Building project..."
make -j$(nproc)

echo "Build completed successfully!"
