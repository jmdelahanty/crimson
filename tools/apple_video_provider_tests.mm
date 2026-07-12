#include "platform/macos/apple_video_frame_provider.h"
#include "platform/macos/apple_video_playback_buffer.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef CRIMSON_SOURCE_DIR
#define CRIMSON_SOURCE_DIR "."
#endif

namespace {

struct TestFailure {
    std::string message;
};

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            throw TestFailure{std::string("CHECK failed: ") + #condition +    \
                              " at " + __FILE__ + ":" +                      \
                              std::to_string(__LINE__)};                       \
        }                                                                      \
    } while (false)

std::string makeFixturePath() {
    NSString* name = [NSString
        stringWithFormat:@"crimson-apple-video-provider-%@.mov",
                         NSUUID.UUID.UUIDString];
    return std::string(
        [NSTemporaryDirectory() stringByAppendingPathComponent:name]
            .fileSystemRepresentation);
}

bool writeFixture(const std::string& path, std::string* error) {
    constexpr int width = 64;
    constexpr int height = 48;
    constexpr int frame_count = 12;
    constexpr int time_scale = 10;

    NSURL* url = [NSURL fileURLWithPath:
        [NSString stringWithUTF8String:path.c_str()]];
    NSError* writer_error = nil;
    AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url
                                                      fileType:AVFileTypeQuickTimeMovie
                                                         error:&writer_error];
    NSDictionary* settings = @{
        AVVideoCodecKey: AVVideoCodecTypeAppleProRes422LT,
        AVVideoWidthKey: @(width),
        AVVideoHeightKey: @(height),
        AVVideoColorPropertiesKey: @{
            AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
            AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
            AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
        },
    };
    AVAssetWriterInput* input =
        [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                           outputSettings:settings];
    input.expectsMediaDataInRealTime = NO;
    NSDictionary* attributes = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (NSString*)kCVPixelBufferWidthKey: @(width),
        (NSString*)kCVPixelBufferHeightKey: @(height),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    AVAssetWriterInputPixelBufferAdaptor* adaptor =
        [AVAssetWriterInputPixelBufferAdaptor
            assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                                       sourcePixelBufferAttributes:attributes];
    if (writer == nil || ![writer canAddInput:input]) {
        *error = "construct: " + std::string(
            writer_error.localizedDescription.UTF8String ?:
                "cannot construct fixture writer");
        return false;
    }
    [writer addInput:input];
    if (![writer startWriting]) {
        *error = "start: " + std::string(
            writer.error.localizedDescription.UTF8String ?:
                "cannot start fixture writer");
        return false;
    }
    [writer startSessionAtSourceTime:kCMTimeZero];

    for (int frame = 0; frame < frame_count; ++frame) {
        while (!input.readyForMoreMediaData) {
            [NSThread sleepForTimeInterval:0.001];
        }
        CVPixelBufferRef pixel_buffer = nullptr;
        const CVReturn create_status = CVPixelBufferPoolCreatePixelBuffer(
            kCFAllocatorDefault, adaptor.pixelBufferPool, &pixel_buffer);
        if (create_status != kCVReturnSuccess || pixel_buffer == nullptr) {
            *error = "cannot allocate fixture pixel buffer";
            [writer cancelWriting];
            return false;
        }
        CVPixelBufferLockBaseAddress(pixel_buffer, 0);
        const size_t y_stride =
            CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
        const size_t uv_stride =
            CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
        std::memset(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0),
                    24 + frame * 12, y_stride * height);
        std::memset(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1), 128,
                    uv_stride * ((height + 1) / 2));
        CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
        CVBufferSetAttachment(pixel_buffer, kCVImageBufferYCbCrMatrixKey,
                              kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        const bool appended = [adaptor appendPixelBuffer:pixel_buffer
                                    withPresentationTime:CMTimeMake(frame,
                                                                    time_scale)];
        CVPixelBufferRelease(pixel_buffer);
        if (!appended) {
            *error = "append frame " + std::to_string(frame) + ": " +
                     std::string(writer.error.localizedDescription.UTF8String ?:
                                     "cannot append fixture frame");
            [writer cancelWriting];
            return false;
        }
    }

    [input markAsFinished];
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    [writer finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(finished);
    }];
    if (dispatch_semaphore_wait(
            finished, dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC)) !=
        0) {
        *error = "fixture writer timed out";
        return false;
    }
    if (writer.status != AVAssetWriterStatusCompleted) {
        *error = "finish: " + std::string(
            writer.error.localizedDescription.UTF8String ?:
                "fixture writer failed");
        return false;
    }
    return true;
}

void testExactSeekStepAndLifetime(const std::string& path) {
    AppleVideoFrameProvider provider;
    std::string error;
    CHECK(provider.open(path, "fixture-main", &error));
    CHECK(provider.isOpen());
    CHECK(provider.info().width == 64);
    CHECK(provider.info().height == 48);
    CHECK(provider.info().frame_count == 12);
    CHECK(std::fabs(provider.info().nominal_frame_rate - 10.0) < 0.01);

    auto frame_seven = provider.readFrame(7, &error);
    CHECK(frame_seven.has_value());
    CHECK(frame_seven->metadata.stream_id == "fixture-main");
    CHECK(frame_seven->metadata.frame_number == 7);
    CHECK(frame_seven->metadata.local_frame_number == 7);
    CHECK(frame_seven->metadata.frame_source_code == 1);
    CHECK(frame_seven->metadata.pixel_format == FramePixelFormat::NV12);
    CHECK(frame_seven->metadata.plane_count == 2);
    CHECK(frame_seven->metadata.color_matrix == ColorSpaceStandard_BT709);
    CHECK(frame_seven->metadata.color_range == ColorRange_MPEG);
    CHECK(frame_seven->metadata.surface_backend ==
          FrameSurfaceBackend::AppleVideoToolbox);
    CHECK(frame_seven->metadata.ownership ==
          FrameSurfaceOwnership::ReferenceCounted);
    CHECK(frame_seven->metadata.lifetime ==
          FrameSurfaceLifetime::ReferenceCounted);
    CHECK(frameMetadataHasValidLayout(frame_seven->metadata));
    CHECK(frame_seven->surface->nativeHandle(0) != 0);
    CHECK(frame_seven->surface->nativeHandle(1) ==
          frame_seven->surface->nativeHandle(0));
    CHECK(frame_seven->surface->nativeHandle(2) == 0);

    CHECK(provider.seekToFrame(4, &error));
    for (int expected = 4; expected <= 6; ++expected) {
        auto frame = provider.readNext(&error);
        CHECK(frame.has_value());
        CHECK(frame->metadata.frame_number == expected);
        if (expected == 4) {
            CHECK(frame->metadata.frame_source_code == 1);
        }
    }

    std::shared_ptr<const FrameSurface> retained = frame_seven->surface;
    std::weak_ptr<const FrameSurface> weak = retained;
    frame_seven.reset();
    provider.close();
    CHECK(!provider.isOpen());
    CHECK(!weak.expired());
    CHECK(retained->nativeHandle() != 0);
    retained.reset();
    CHECK(weak.expired());
}

void testBoundedPlaybackBuffer(const std::string& path) {
    AppleVideoPlaybackBuffer playback;
    std::string error;
    CHECK(playback.open(path, "fixture-buffered", 4, &error));
    CHECK(playback.capacity() == 4);
    CHECK(playback.waitForFrame(0, std::chrono::seconds(5)));
    auto first = playback.frameForTarget(0, true);
    CHECK(first.has_value());
    CHECK(first->metadata.frame_number == 0);

    playback.setPlaybackState(0, true, 10.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    const AppleVideoPlaybackBufferMetrics stalled_metrics = playback.metrics();
    CHECK(stalled_metrics.last_decoded_frame >= 5);
    CHECK(stalled_metrics.buffered_frames <= 4);
    CHECK(stalled_metrics.peak_buffered_frames <= 4);
    playback.setPlaybackState(stalled_metrics.last_decoded_frame, false, 10.0);

    CHECK(playback.requestSeek(6, &error));
    CHECK(playback.waitForFrame(6, std::chrono::seconds(5)));
    auto sought = playback.frameForTarget(6, true);
    CHECK(sought.has_value());
    CHECK(sought->metadata.frame_number == 6);
    playback.setTargetFrame(7);
    CHECK(playback.waitForFrame(7, std::chrono::seconds(5)));
    auto stepped = playback.frameForTarget(7, true);
    CHECK(stepped.has_value());
    CHECK(stepped->metadata.frame_number == 7);

    const AppleVideoPlaybackBufferMetrics metrics = playback.metrics();
    CHECK(metrics.buffered_frames <= 4);
    CHECK(metrics.peak_buffered_frames <= 4);
    CHECK(metrics.decoded_frames >= 3);
    CHECK(metrics.last_seek_ms >= 0.0);
    CHECK(metrics.last_error.empty());

    playback.suspend();
    CHECK(playback.isOpen());
    CHECK(playback.metrics().buffered_frames == 0);
    CHECK(playback.requestSeek(3, &error));
    CHECK(playback.waitForFrame(3, std::chrono::seconds(5)));
    auto resumed_exact = playback.frameForTarget(3, true);
    CHECK(resumed_exact.has_value());
    CHECK(resumed_exact->metadata.frame_number == 3);
    playback.close();
}

struct GoldenFrame {
    int64_t frame_number = -1;
    int64_t frame_pts = -1;
    int time_scale = 0;
    int width = 0;
    int height = 0;
    int64_t frame_count = 0;
    double nominal_fps = 0.0;
};

std::vector<GoldenFrame> loadGoldenFrames(const std::string& path) {
    std::ifstream input(path);
    CHECK(input.is_open());
    std::vector<GoldenFrame> rows;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#' ||
            line.rfind("frame_number,", 0) == 0) {
            continue;
        }
        std::stringstream parser(line);
        std::string field;
        GoldenFrame row;
        std::getline(parser, field, ',');
        row.frame_number = std::stoll(field);
        std::getline(parser, field, ',');
        row.frame_pts = std::stoll(field);
        std::getline(parser, field, ',');
        row.time_scale = std::stoi(field);
        std::getline(parser, field, ',');
        row.width = std::stoi(field);
        std::getline(parser, field, ',');
        row.height = std::stoi(field);
        std::getline(parser, field, ',');
        row.frame_count = std::stoll(field);
        std::getline(parser, field, ',');
        row.nominal_fps = std::stod(field);
        rows.push_back(row);
    }
    CHECK(!rows.empty());
    return rows;
}

void testRepresentativeAsset(const std::string& path,
                             const std::string& golden_path) {
    using Clock = std::chrono::steady_clock;
    const std::vector<GoldenFrame> golden = loadGoldenFrames(golden_path);
    AppleVideoFrameProvider provider;
    std::string error;
    const auto open_start = Clock::now();
    CHECK(provider.open(path, "representative-main", &error));
    const double startup_ms = std::chrono::duration<double, std::milli>(
                                  Clock::now() - open_start)
                                  .count();
    const AppleVideoAssetInfo& info = provider.info();
    CHECK(info.width == golden.front().width);
    CHECK(info.height == golden.front().height);
    CHECK(info.frame_count == golden.front().frame_count);
    CHECK(std::fabs(info.nominal_frame_rate - golden.front().nominal_fps) <
          0.001);
    std::cout << "[AppleVideoRepresentative] asset=" << info.width << 'x'
              << info.height << " frames=" << info.frame_count
              << " fps=" << info.nominal_frame_rate
              << " startup_ms=" << startup_ms << std::endl;

    for (const GoldenFrame& expected : golden) {
        const auto seek_start = Clock::now();
        auto frame = provider.readFrame(expected.frame_number, &error);
        const double seek_ms = std::chrono::duration<double, std::milli>(
                                   Clock::now() - seek_start)
                                   .count();
        CHECK(frame.has_value());
        CHECK(frame->metadata.frame_number == expected.frame_number);
        CHECK(frame->metadata.frame_pts == expected.frame_pts);
        CHECK(frame->metadata.time_base.numerator == 1);
        CHECK(frame->metadata.time_base.denominator == expected.time_scale);
        CHECK(frame->metadata.width == expected.width);
        CHECK(frame->metadata.height == expected.height);
        CHECK(frame->metadata.pixel_format == FramePixelFormat::NV12);
        CHECK(frame->surface->nativeHandle() != 0);
        std::cout << "[AppleVideoRepresentative] target="
                  << expected.frame_number
                  << " pts=" << frame->metadata.frame_pts
                  << " timescale=" << frame->metadata.time_base.denominator
                  << " seek_ms=" << seek_ms << std::endl;
    }

    const int64_t step_start = info.frame_count / 3;
    CHECK(provider.seekToFrame(step_start, &error));
    for (int offset = 0; offset < 8; ++offset) {
        auto frame = provider.readNext(&error);
        CHECK(frame.has_value());
        CHECK(frame->metadata.frame_number == step_start + offset);
    }
}

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc == 3 && std::string(argv[1]) == "--write-fixture") {
            const std::string output_path = argv[2];
            [[NSFileManager defaultManager]
                removeItemAtPath:
                    [NSString stringWithUTF8String:output_path.c_str()]
                          error:nil];
            std::string fixture_error;
            if (!writeFixture(output_path, &fixture_error)) {
                std::cerr << "fixture creation failed: " << fixture_error
                          << std::endl;
                return 1;
            }
            std::cout << "apple_video_provider_tests: fixture=" << output_path
                      << std::endl;
            return 0;
        }
        const std::string fixture = makeFixturePath();
        std::string error;
        try {
            CHECK(writeFixture(fixture, &error));
            testExactSeekStepAndLifetime(fixture);
            testBoundedPlaybackBuffer(fixture);
            if ((argc == 3 || argc == 5) &&
                std::string(argv[1]) == "--asset") {
                std::string golden_path =
                    CRIMSON_SOURCE_DIR
                    "/tests/fixtures/macos_main_camera_frame_identity.csv";
                if (argc == 5 && std::string(argv[3]) == "--golden") {
                    golden_path = argv[4];
                } else if (argc != 3) {
                    throw TestFailure{
                        "usage: apple_video_provider_tests [--asset VIDEO "
                        "[--golden CSV]]"};
                }
                testRepresentativeAsset(argv[2], golden_path);
            } else if (argc != 1) {
                throw TestFailure{
                    "usage: apple_video_provider_tests [--asset VIDEO "
                    "[--golden CSV]]"};
            }
        } catch (const TestFailure& failure) {
            std::cerr << failure.message;
            if (!error.empty()) {
                std::cerr << " error=" << error;
            }
            std::cerr << std::endl;
            [[NSFileManager defaultManager]
                removeItemAtPath:[NSString stringWithUTF8String:fixture.c_str()]
                            error:nil];
            return 1;
        }
        [[NSFileManager defaultManager]
            removeItemAtPath:[NSString stringWithUTF8String:fixture.c_str()]
                        error:nil];
        std::cout << "apple_video_provider_tests: PASS" << std::endl;
        return 0;
    }
}
