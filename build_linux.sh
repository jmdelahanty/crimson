#!/bin/bash

echo "Building with system OpenCV (avoiding /opt/orange/lib/opencv)..."
echo "================================================================"

# Clean any previous builds
echo "Cleaning previous build..."
rm -rf build
rm -f CMakeCache.txt

# Create build directory
mkdir -p build
cd build

# Configure with explicit paths to avoid the orange OpenCV
echo "Configuring CMake..."
cmake \
    -DOpenCV_DIR=/usr/lib/x86_64-linux-gnu/cmake/opencv4 \
    -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu \
    -DCMAKE_IGNORE_PATH=/opt/orange \
    ..

# Check if configuration was successful
if [ $? -eq 0 ]; then
    echo "Configuration successful!"
    echo "Building..."
    make -j$(nproc)
    
    if [ $? -eq 0 ]; then
        echo ""
        echo "================================================================"
        echo "Build complete!"
        echo "Executable location: ../release/redgui"
        echo ""
        echo "To verify OpenCV linkage, run:"
        echo "  ldd ../release/redgui | grep opencv"
        echo ""
        echo "To run the program:"
        echo "  ../release/redgui"
        echo "================================================================"
    else
        echo "Build failed!"
        exit 1
    fi
else
    echo "CMake configuration failed!"
    echo ""
    echo "Troubleshooting:"
    echo "1. Check that system OpenCV is installed:"
    echo "   dpkg -l | grep libopencv"
    echo ""
    echo "2. If not installed, install it:"
    echo "   sudo apt-get install libopencv-dev"
    echo ""
    echo "3. Clear CMake cache and try again:"
    echo "   rm -rf build CMakeCache.txt"
    exit 1
fi