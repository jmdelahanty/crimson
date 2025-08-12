#!/bin/bash

# OpenCV Build Script for Crimson Project
# Installs to /opt/crimson with full CUDA and SFM support
# February 2025

# ANSI color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Build directories (temporary)
BUILD_DIR="/tmp/opencv-crimson-build-$$"
OPENCV_BUILD="${BUILD_DIR}/opencv"
OPENCV_CONTRIB_BUILD="${BUILD_DIR}/opencv_contrib"

# Final installation directories
INSTALL_DIR="/opt/crimson"
OPENCV_DIR="${INSTALL_DIR}/lib/opencv"

# OpenCV version
OPENCV_VERSION="4.10.0"

# Function to exit with an error message
error_exit() {
    echo -e "${RED}Error: $1${NC}" >&2
    if [ -d "$BUILD_DIR" ]; then
        echo -e "${YELLOW}Build directory preserved at: $BUILD_DIR${NC}"
    fi
    exit 1
}

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    error_exit "This script must be run as root. Please use sudo."
fi

# Check CUDA availability
echo -e "${BLUE}Checking CUDA installation...${NC}"
if ! command -v nvcc &> /dev/null; then
    echo -e "${YELLOW}CUDA toolkit not found! Disabling CUDA support...${NC}"
    CUDA_ENABLED=false
else
    CUDA_VERSION=$(nvcc --version | grep "release" | awk '{print $6}' | cut -c2-)
    echo -e "${GREEN}CUDA toolkit found (version $CUDA_VERSION). Enabling CUDA support...${NC}"
    CUDA_ENABLED=true
fi

# Install required packages (including SFM dependencies)
echo -e "${YELLOW}Installing required packages...${NC}"
apt-get update
apt-get install -y build-essential cmake pkg-config \
    libjpeg-dev libpng-dev libtiff-dev \
    libavcodec-dev libavformat-dev libswscale-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libxvidcore-dev x264 libx264-dev libfaac-dev libmp3lame-dev libtheora-dev \
    libfaac-dev libmp3lame-dev libvorbis-dev \
    libopencore-amrnb-dev libopencore-amrwb-dev \
    libatlas-base-dev gfortran libeigen3-dev \
    python3-dev python3-numpy python3-pip \
    libtbb-dev \
    libgflags-dev libgoogle-glog-dev \
    libsuitesparse-dev \
    libceres-dev \
    libprotobuf-dev protobuf-compiler \
    libhdf5-dev \
    qtbase5-dev qt5-qmake qtchooser

# Install gcc-12 if not present (for CUDA compatibility)
if [ ! -x /usr/bin/gcc-12 ]; then
    echo -e "${YELLOW}Installing GCC 12 for CUDA compatibility...${NC}"
    apt-get install -y gcc-12 g++-12
fi

# Create all required directories
echo -e "${BLUE}Creating directories...${NC}"
rm -rf "$BUILD_DIR"  # Clean up any existing directory
mkdir -p "$BUILD_DIR" || error_exit "Failed to create build directory"
mkdir -p "$OPENCV_BUILD" || error_exit "Failed to create OpenCV build directory"
mkdir -p "$OPENCV_CONTRIB_BUILD" || error_exit "Failed to create OpenCV contrib directory"
mkdir -p "$OPENCV_DIR" || error_exit "Failed to create installation directory"

# Set username and group for correct ownership
USER_NAME=${SUDO_USER:-$(logname)}
USER_GROUP=$(id -gn "$USER_NAME")

# Set ownership for build directory
chown -R "$USER_NAME":"$USER_GROUP" "$BUILD_DIR" || error_exit "Failed to change ownership of build directory"

# Download OpenCV and OpenCV contrib
echo -e "${BLUE}Downloading OpenCV ${OPENCV_VERSION}...${NC}"
cd "$BUILD_DIR" || error_exit "Failed to enter build directory"
sudo -u $SUDO_USER git clone --depth 1 --branch ${OPENCV_VERSION} https://github.com/opencv/opencv.git "$OPENCV_BUILD" || error_exit "Failed to clone OpenCV"

echo -e "${BLUE}Downloading OpenCV Contrib ${OPENCV_VERSION}...${NC}"
sudo -u $SUDO_USER git clone --depth 1 --branch ${OPENCV_VERSION} https://github.com/opencv/opencv_contrib.git "$OPENCV_CONTRIB_BUILD" || error_exit "Failed to clone OpenCV Contrib"

# Configure and build OpenCV
echo -e "${YELLOW}Configuring OpenCV...${NC}"
cd "$OPENCV_BUILD" || error_exit "Failed to change to OpenCV directory"
mkdir -p build
cd build

# Create build directory with proper permissions
chown -R $USER_NAME:$USER_GROUP .

# Set CUDA options based on availability
if [ "$CUDA_ENABLED" = true ]; then
    # CUDA compute capabilities for A6000 (8.6) and general Ampere (8.0)
    CUDA_ARCH="8.0,8.6"
    
    # Check if CUDNN is installed
    if [ -f "/usr/local/cuda/include/cudnn.h" ] || [ -f "/usr/include/cudnn.h" ]; then
        echo -e "${GREEN}cuDNN found. Enabling cuDNN support...${NC}"
        CUDNN_OPTIONS="-D WITH_CUDNN=ON -D OPENCV_DNN_CUDA=ON"
    else
        echo -e "${YELLOW}cuDNN not found. Disabling cuDNN support...${NC}"
        CUDNN_OPTIONS="-D WITH_CUDNN=OFF -D OPENCV_DNN_CUDA=OFF"
    fi
    
    CUDA_OPTIONS="-D WITH_CUDA=ON \
    -D CUDA_FAST_MATH=ON \
    -D WITH_CUBLAS=ON \
    -D CUDA_ARCH_BIN=$CUDA_ARCH \
    -D BUILD_opencv_cudacodec=ON \
    -D BUILD_opencv_cudaarithm=ON \
    -D BUILD_opencv_cudabgsegm=ON \
    -D BUILD_opencv_cudafeatures2d=ON \
    -D BUILD_opencv_cudafilters=ON \
    -D BUILD_opencv_cudaimgproc=ON \
    -D BUILD_opencv_cudalegacy=ON \
    -D BUILD_opencv_cudaobjdetect=ON \
    -D BUILD_opencv_cudaoptflow=ON \
    -D BUILD_opencv_cudastereo=ON \
    -D BUILD_opencv_cudawarping=ON \
    -D BUILD_opencv_cudev=ON \
    $CUDNN_OPTIONS"
else
    CUDA_OPTIONS="-D WITH_CUDA=OFF -D OPENCV_DNN_CUDA=OFF"
fi

# Use GCC 12 for CUDA builds
if [ -x /usr/bin/gcc-12 ]; then
    export CC=/usr/bin/gcc-12
    export CXX=/usr/bin/g++-12
    HOST_COMPILER_OPTION="-D CUDA_HOST_COMPILER=/usr/bin/gcc-12"
    echo -e "${GREEN}Using GCC 12 for CUDA compatibility${NC}"
else
    echo -e "${YELLOW}GCC 12 not found, CUDA build may fail${NC}"
    HOST_COMPILER_OPTION=""
fi

# Configure with CMake
echo -e "${YELLOW}Running CMake...${NC}"
sudo -u $SUDO_USER cmake \
    -D CMAKE_BUILD_TYPE=RELEASE \
    -D CMAKE_INSTALL_PREFIX="$OPENCV_DIR" \
    -D OPENCV_PC_FILE_NAME=opencv4.pc \
    -D INSTALL_PYTHON_EXAMPLES=OFF \
    -D INSTALL_C_EXAMPLES=OFF \
    -D BUILD_EXAMPLES=OFF \
    -D BUILD_TESTS=OFF \
    -D BUILD_PERF_TESTS=OFF \
    -D WITH_TBB=ON \
    -D WITH_V4L=ON \
    -D WITH_QT=ON \
    -D WITH_GTK=ON \
    -D WITH_OPENGL=ON \
    -D WITH_GTK_2_X=OFF \
    -D WITH_GSTREAMER=ON \
    -D WITH_FFMPEG=ON \
    -D ENABLE_FAST_MATH=ON \
    -D OPENCV_ENABLE_NONFREE=ON \
    -D OPENCV_GENERATE_PKGCONFIG=ON \
    -D OPENCV_EXTRA_MODULES_PATH="$OPENCV_CONTRIB_BUILD/modules" \
    -D BUILD_opencv_sfm=ON \
    -D BUILD_opencv_viz=ON \
    -D BUILD_opencv_rgbd=ON \
    -D BUILD_opencv_surface_matching=ON \
    -D BUILD_opencv_structured_light=ON \
    -D BUILD_opencv_aruco=ON \
    -D BUILD_opencv_alphamat=ON \
    -D BUILD_opencv_bgsegm=ON \
    -D BUILD_opencv_bioinspired=ON \
    -D BUILD_opencv_ccalib=ON \
    -D BUILD_opencv_dnn_objdetect=ON \
    -D BUILD_opencv_dnn_superres=ON \
    -D BUILD_opencv_dpm=ON \
    -D BUILD_opencv_face=ON \
    -D BUILD_opencv_fuzzy=ON \
    -D BUILD_opencv_hdf=ON \
    -D BUILD_opencv_hfs=ON \
    -D BUILD_opencv_img_hash=ON \
    -D BUILD_opencv_intensity_transform=ON \
    -D BUILD_opencv_line_descriptor=ON \
    -D BUILD_opencv_mcc=ON \
    -D BUILD_opencv_optflow=ON \
    -D BUILD_opencv_phase_unwrapping=ON \
    -D BUILD_opencv_plot=ON \
    -D BUILD_opencv_quality=ON \
    -D BUILD_opencv_rapid=ON \
    -D BUILD_opencv_reg=ON \
    -D BUILD_opencv_saliency=ON \
    -D BUILD_opencv_shape=ON \
    -D BUILD_opencv_stereo=ON \
    -D BUILD_opencv_superres=ON \
    -D BUILD_opencv_text=ON \
    -D BUILD_opencv_tracking=ON \
    -D BUILD_opencv_videostab=ON \
    -D BUILD_opencv_wechat_qrcode=ON \
    -D BUILD_opencv_xfeatures2d=ON \
    -D BUILD_opencv_ximgproc=ON \
    -D BUILD_opencv_xobjdetect=ON \
    -D BUILD_opencv_xphoto=ON \
    $CUDA_OPTIONS \
    $HOST_COMPILER_OPTION \
    .. || error_exit "CMake configuration failed"

# Build OpenCV
echo -e "${YELLOW}Building OpenCV (this will take 20-30 minutes)...${NC}"
sudo -u $SUDO_USER make -j$(nproc) || error_exit "OpenCV compilation failed"

# Install OpenCV
echo -e "${YELLOW}Installing OpenCV to ${OPENCV_DIR}...${NC}"
make install || error_exit "OpenCV installation failed"

# Set up environment variables
echo -e "${BLUE}Setting up environment variables...${NC}"
cat > /etc/profile.d/opencv-crimson.sh << EOF
# OpenCV configuration for Crimson project
export CRIMSON_ROOT=${INSTALL_DIR}
export OPENCV_HOME=\${CRIMSON_ROOT}/lib/opencv
export PATH=\$OPENCV_HOME/bin:\$PATH
export LD_LIBRARY_PATH=\$OPENCV_HOME/lib:\$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=\$OPENCV_HOME/lib/pkgconfig:\$PKG_CONFIG_PATH
export OpenCV_DIR=\$OPENCV_HOME/lib/cmake/opencv4

# TensorRT (if available)
if [ -d "/usr/local/TensorRT-10.0.1.6" ]; then
    export TENSORRT_ROOT=/usr/local/TensorRT-10.0.1.6
    export LD_LIBRARY_PATH=\$TENSORRT_ROOT/lib:\$LD_LIBRARY_PATH
fi

# FFmpeg (using orange installation for now)
if [ -d "/opt/orange/lib/ffmpeg-nvidia" ]; then
    export FFMPEG_ROOT=/opt/orange/lib/ffmpeg-nvidia
    export LD_LIBRARY_PATH=\$FFMPEG_ROOT/lib:\$LD_LIBRARY_PATH
fi
EOF

# Update library cache
ldconfig

# Set permissions
chmod 644 /etc/profile.d/opencv-crimson.sh
chown -R root:root "$INSTALL_DIR"
chmod -R 755 "$INSTALL_DIR"

# Create a simple test program
echo -e "${BLUE}Creating test program...${NC}"
cat > "${INSTALL_DIR}/test_opencv.cpp" << 'EOF'
#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/sfm.hpp>
#include <opencv2/core/cuda.hpp>

int main() {
    std::cout << "OpenCV version: " << CV_VERSION << std::endl;
    std::cout << "OpenCV build info:\n" << cv::getBuildInformation() << std::endl;
    
    // Test SFM module
    cv::Mat K = cv::Mat::eye(3, 3, CV_64F);
    cv::Mat R = cv::Mat::eye(3, 3, CV_64F);
    cv::Mat t = cv::Mat::zeros(3, 1, CV_64F);
    cv::Mat P;
    cv::sfm::projectionFromKRt(K, R, t, P);
    std::cout << "SFM module: OK" << std::endl;
    
    // Test CUDA
    int cuda_count = cv::cuda::getCudaEnabledDeviceCount();
    std::cout << "CUDA devices: " << cuda_count << std::endl;
    
    return 0;
}
EOF

# Clean up build directory
echo -e "${BLUE}Cleaning up...${NC}"
rm -rf "$BUILD_DIR"

echo -e "${GREEN}OpenCV installation completed successfully!${NC}"
echo "OpenCV installed in: ${OPENCV_DIR}"
echo "Environment configured in: /etc/profile.d/opencv-crimson.sh"
echo ""
echo -e "${YELLOW}To use this installation:${NC}"
echo "1. Source the environment: source /etc/profile.d/opencv-crimson.sh"
echo "2. Test the installation:"
echo "   cd ${INSTALL_DIR}"
echo "   g++ test_opencv.cpp -o test_opencv \$(pkg-config --cflags --libs opencv4)"
echo "   ./test_opencv"
echo ""
echo -e "${BLUE}Verification Information:${NC}"
echo "Installation Location: ${OPENCV_DIR}"
echo "Environment File: /etc/profile.d/opencv-crimson.sh"
if [ "$CUDA_ENABLED" = true ]; then
    echo "CUDA Support: Enabled (Compute Capability: $CUDA_ARCH)"
else
    echo "CUDA Support: Disabled"
fi

exit 0