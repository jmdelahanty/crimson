#!/bin/bash

echo "Compiling OpenCV CUDA check program..."

# Use the system OpenCV explicitly
g++ check_opencv_cuda.cpp \
    -I/usr/include/opencv4 \
    -L/usr/lib/x86_64-linux-gnu \
    -lopencv_core \
    -lopencv_imgproc \
    -lopencv_highgui \
    -lopencv_videoio \
    -o check_opencv_cuda

if [ $? -eq 0 ]; then
    echo "Compilation successful!"
    echo "Running check..."
    echo ""
    ./check_opencv_cuda | head -100  # Limit output to first 100 lines
else
    echo "Compilation failed. Trying without CUDA module..."
    # Try compiling without cuda module
    g++ -DSKIP_CUDA check_opencv_cuda.cpp \
        -I/usr/include/opencv4 \
        -L/usr/lib/x86_64-linux-gnu \
        -lopencv_core \
        -o check_opencv_cuda_nocuda
    
    if [ $? -eq 0 ]; then
        echo "Note: System OpenCV appears to be built without CUDA support (which is good - no conflicts!)"
    fi
fi