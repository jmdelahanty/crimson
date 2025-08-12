#!/bin/bash

echo "Searching for OpenCV installations..."
echo "======================================="

# Check pkg-config
echo -e "\n1. Checking pkg-config:"
if pkg-config --exists opencv4; then
    echo "   OpenCV4 found via pkg-config"
    echo "   Version: $(pkg-config --modversion opencv4)"
    echo "   Cflags: $(pkg-config --cflags opencv4)"
    echo "   Libs: $(pkg-config --libs opencv4)"
elif pkg-config --exists opencv; then
    echo "   OpenCV found via pkg-config"
    echo "   Version: $(pkg-config --modversion opencv)"
fi

# Check common installation paths
echo -e "\n2. Checking common OpenCV cmake paths:"
OPENCV_CMAKE_PATHS=(
    "/usr/lib/x86_64-linux-gnu/cmake/opencv4"
    "/usr/lib/x86_64-linux-gnu/cmake/OpenCV"
    "/usr/share/opencv4/cmake"
    "/usr/share/OpenCV"
    "/usr/local/lib/cmake/opencv4"
    "/usr/local/share/opencv4"
    "/opt/opencv/lib/cmake/opencv4"
)

for path in "${OPENCV_CMAKE_PATHS[@]}"; do
    if [ -d "$path" ]; then
        echo "   Found: $path"
        if [ -f "$path/OpenCVConfig.cmake" ]; then
            echo "          Contains OpenCVConfig.cmake ✓"
        fi
    fi
done

# Check for OpenCV libraries
echo -e "\n3. Checking for OpenCV libraries:"
OPENCV_LIB_PATHS=(
    "/usr/lib/x86_64-linux-gnu"
    "/usr/local/lib"
    "/usr/lib"
)

for libpath in "${OPENCV_LIB_PATHS[@]}"; do
    if ls $libpath/libopencv_core.so* 2>/dev/null | head -1 > /dev/null; then
        echo "   Found OpenCV libraries in: $libpath"
        ls $libpath/libopencv_core.so* | head -1
    fi
done

# Check OpenCV headers
echo -e "\n4. Checking for OpenCV headers:"
OPENCV_INCLUDE_PATHS=(
    "/usr/include/opencv4"
    "/usr/include/opencv2"
    "/usr/local/include/opencv4"
    "/usr/local/include/opencv2"
)

for incpath in "${OPENCV_INCLUDE_PATHS[@]}"; do
    if [ -d "$incpath" ]; then
        echo "   Found: $incpath"
    fi
done

# Try to run opencv_version command
echo -e "\n5. OpenCV version command:"
if command -v opencv_version &> /dev/null; then
    echo "   opencv_version: $(opencv_version)"
else
    echo "   opencv_version command not found"
fi

# Create a simple test program
echo -e "\n6. Creating and testing a simple OpenCV program:"
cat > /tmp/test_opencv.cpp << 'EOF'
#include <iostream>
#include <opencv2/opencv.hpp>

int main() {
    std::cout << "OpenCV version: " << CV_VERSION << std::endl;
    std::cout << "OpenCV major: " << CV_MAJOR_VERSION << std::endl;
    std::cout << "OpenCV minor: " << CV_MINOR_VERSION << std::endl;
    return 0;
}
EOF

# Try to compile with pkg-config
if pkg-config --exists opencv4; then
    echo "   Compiling with pkg-config opencv4..."
    if g++ /tmp/test_opencv.cpp $(pkg-config --cflags --libs opencv4) -o /tmp/test_opencv 2>/dev/null; then
        echo "   Compilation successful!"
        /tmp/test_opencv
    else
        echo "   Compilation failed with opencv4"
    fi
elif pkg-config --exists opencv; then
    echo "   Compiling with pkg-config opencv..."
    if g++ /tmp/test_opencv.cpp $(pkg-config --cflags --libs opencv) -o /tmp/test_opencv 2>/dev/null; then
        echo "   Compilation successful!"
        /tmp/test_opencv
    else
        echo "   Compilation failed"
    fi
fi

# Clean up
rm -f /tmp/test_opencv.cpp /tmp/test_opencv

echo -e "\n======================================="
echo "Recommended CMake configuration:"
echo ""

# Provide recommendation based on findings
if [ -d "/usr/lib/x86_64-linux-gnu/cmake/opencv4" ]; then
    echo "set(OpenCV_DIR /usr/lib/x86_64-linux-gnu/cmake/opencv4)"
elif [ -d "/usr/lib/x86_64-linux-gnu/cmake/OpenCV" ]; then
    echo "set(OpenCV_DIR /usr/lib/x86_64-linux-gnu/cmake/OpenCV)"
elif [ -d "/usr/share/opencv4/cmake" ]; then
    echo "set(OpenCV_DIR /usr/share/opencv4/cmake)"
elif [ -d "/usr/local/lib/cmake/opencv4" ]; then
    echo "set(OpenCV_DIR /usr/local/lib/cmake/opencv4)"
else
    echo "# Let CMake find OpenCV automatically"
    echo "find_package(OpenCV REQUIRED)"
fi

echo ""
echo "To use this in your CMakeLists.txt, copy the line above before find_package(OpenCV REQUIRED)"