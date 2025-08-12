// check_opencv_cuda.cpp - Check OpenCV installation and CUDA support
#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/core/cuda.hpp>

int main() {
    std::cout << "OpenCV Configuration Check" << std::endl;
    std::cout << "===========================" << std::endl;
    
    // Basic OpenCV info
    std::cout << "\nOpenCV Version: " << CV_VERSION << std::endl;
    std::cout << "OpenCV Major.Minor: " << CV_MAJOR_VERSION << "." << CV_MINOR_VERSION << std::endl;
    
    // Check build information
    std::cout << "\nBuild Information:" << std::endl;
    std::cout << cv::getBuildInformation() << std::endl;
    
    // Check CUDA support
    std::cout << "\n===========================" << std::endl;
    std::cout << "CUDA Support Check:" << std::endl;
    std::cout << "===========================" << std::endl;
    
    int cuda_device_count = cv::cuda::getCudaEnabledDeviceCount();
    
    if (cuda_device_count > 0) {
        std::cout << "CUDA is ENABLED in this OpenCV build" << std::endl;
        std::cout << "Number of CUDA devices: " << cuda_device_count << std::endl;
        
        for (int i = 0; i < cuda_device_count; i++) {
            cv::cuda::DeviceInfo info(i);
            std::cout << "\nDevice " << i << ": " << info.name() << std::endl;
            std::cout << "  Compute Capability: " << info.majorVersion() << "." << info.minorVersion() << std::endl;
            std::cout << "  Total Memory: " << (info.totalMemory() / 1024 / 1024) << " MB" << std::endl;
            std::cout << "  Free Memory: " << (info.freeMemory() / 1024 / 1024) << " MB" << std::endl;
        }
    } else {
        std::cout << "CUDA is NOT enabled in this OpenCV build" << std::endl;
        std::cout << "This OpenCV installation was built WITHOUT CUDA support" << std::endl;
        std::cout << "\nThis is actually GOOD for our case - it means no CUDA version conflicts!" << std::endl;
    }
    
    return 0;
}