#include "apple_video_frame_provider.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

std::string errorText(NSError* error, const char* fallback) {
    if (error != nil && error.localizedDescription.UTF8String != nullptr) {
        return error.localizedDescription.UTF8String;
    }
    return fallback;
}

void assignError(std::string* destination, const std::string& value) {
    if (destination != nullptr) {
        *destination = value;
    }
}

double seconds(CMTime time) {
    if (!CMTIME_IS_NUMERIC(time)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return CMTimeGetSeconds(time);
}

bool formatDescriptionIsFullRange(CMFormatDescriptionRef description) {
    if (description == nullptr) {
        return false;
    }
    CFTypeRef value = CMFormatDescriptionGetExtension(
        description, kCMFormatDescriptionExtension_FullRangeVideo);
    return value != nullptr && CFGetTypeID(value) == CFBooleanGetTypeID() &&
           CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}

int colorMatrixFromValue(CFTypeRef value) {
    if (value == nullptr) {
        return ColorSpaceStandard_BT709;
    }
    if (CFEqual(value, kCVImageBufferYCbCrMatrix_ITU_R_601_4)) {
        return ColorSpaceStandard_BT601;
    }
    if (CFEqual(value, kCVImageBufferYCbCrMatrix_SMPTE_240M_1995)) {
        return ColorSpaceStandard_SMPTE240M;
    }
    if (CFEqual(value, kCVImageBufferYCbCrMatrix_ITU_R_2020)) {
        return ColorSpaceStandard_BT2020;
    }
    return ColorSpaceStandard_BT709;
}

int colorMatrixFromFormatDescription(CMFormatDescriptionRef description) {
    if (description == nullptr) {
        return ColorSpaceStandard_BT709;
    }
    return colorMatrixFromValue(CMFormatDescriptionGetExtension(
        description, kCMFormatDescriptionExtension_YCbCrMatrix));
}

int colorMatrixFromPixelBuffer(CVPixelBufferRef pixel_buffer,
                               int fallback) {
    CFTypeRef value = CVBufferCopyAttachment(
        pixel_buffer, kCVImageBufferYCbCrMatrixKey, nullptr);
    const int matrix = value == nullptr ? fallback : colorMatrixFromValue(value);
    if (value != nullptr) {
        CFRelease(value);
    }
    return matrix;
}

int colorRangeFromPixelFormat(OSType format) {
    if (format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        return ColorRange_JPEG;
    }
    if (format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        return ColorRange_MPEG;
    }
    return ColorRange_Unspecified;
}

class ApplePixelBufferFrameSurface final : public FrameSurface {
  public:
    ApplePixelBufferFrameSurface(CVPixelBufferRef pixel_buffer,
                                 const FrameSurfaceDescriptor& descriptor)
        : pixel_buffer_(CVPixelBufferRetain(pixel_buffer)),
          descriptor_(descriptor) {}

    ~ApplePixelBufferFrameSurface() override {
        if (pixel_buffer_ != nullptr) {
            CVPixelBufferRelease(pixel_buffer_);
        }
    }

    const FrameSurfaceDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    uintptr_t nativeHandle(size_t plane_index) const noexcept override {
        if (pixel_buffer_ == nullptr || plane_index >= descriptor_.plane_count) {
            return 0;
        }
        return reinterpret_cast<uintptr_t>(pixel_buffer_);
    }

  private:
    CVPixelBufferRef pixel_buffer_ = nullptr;
    FrameSurfaceDescriptor descriptor_;
};

NSDictionary* pixelBufferSettings(bool full_range) {
    const OSType format =
        full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                   : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    return @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(format),
        (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
}

}  // namespace

struct AppleVideoFrameProvider::Impl {
    AppleVideoAssetInfo info;
    AVURLAsset* asset = nil;
    AVAssetTrack* track = nil;
    AVAssetReader* reader = nil;
    AVAssetReaderTrackOutput* output = nil;
    std::optional<AppleDecodedVideoFrame> pending_frame;
    CMTime track_start = kCMTimeZero;
    CMTime track_duration = kCMTimeInvalid;
    CMTime frame_duration = kCMTimeInvalid;
    int64_t last_decoded_frame = -1;
    bool source_full_range = false;

    void close() {
        suspendDecoding();
        track = nil;
        asset = nil;
        info = {};
        track_start = kCMTimeZero;
        track_duration = kCMTimeInvalid;
        frame_duration = kCMTimeInvalid;
        source_full_range = false;
    }

    void suspendDecoding() {
        if (reader != nil) {
            [reader cancelReading];
        }
        pending_frame.reset();
        output = nil;
        reader = nil;
        last_decoded_frame = -1;
    }

    bool loadTrack(std::string* error) {
        __block NSArray<AVAssetTrack*>* tracks = nil;
        __block NSError* load_error = nil;
        dispatch_semaphore_t ready = dispatch_semaphore_create(0);
        [asset loadTracksWithMediaType:AVMediaTypeVideo
                     completionHandler:^(NSArray<AVAssetTrack*>* loaded_tracks,
                                         NSError* block_error) {
            tracks = loaded_tracks;
            load_error = block_error;
            dispatch_semaphore_signal(ready);
        }];
        const long wait_status = dispatch_semaphore_wait(
            ready, dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC));
        if (wait_status != 0) {
            assignError(error, "timed out loading video track");
            return false;
        }
        if (load_error != nil || tracks.count == 0) {
            assignError(error, errorText(load_error, "asset has no video track"));
            return false;
        }
        track = tracks.firstObject;
        return true;
    }

    int64_t frameIndexForPts(CMTime pts) const {
        const double relative_seconds = seconds(CMTimeSubtract(pts, track_start));
        const double frame_seconds = seconds(frame_duration);
        if (!std::isfinite(relative_seconds) || !std::isfinite(frame_seconds) ||
            frame_seconds <= 0.0) {
            return -1;
        }
        return static_cast<int64_t>(
            std::llround(relative_seconds / frame_seconds));
    }

    CMTime timeForFrame(int64_t frame_number) const {
        if (frame_number <= std::numeric_limits<int32_t>::max()) {
            return CMTimeAdd(
                track_start,
                CMTimeMultiply(frame_duration,
                               static_cast<int32_t>(frame_number)));
        }
        return CMTimeAdd(
            track_start,
            CMTimeMakeWithSeconds(frame_number * seconds(frame_duration),
                                  60000));
    }

    bool startReaderAtFrame(int64_t target_frame, double preroll_seconds,
                            std::string* error) {
        if (reader != nil) {
            [reader cancelReading];
        }
        pending_frame.reset();
        output = nil;
        reader = nil;

        NSError* reader_error = nil;
        reader = [[AVAssetReader alloc] initWithAsset:asset error:&reader_error];
        output = [[AVAssetReaderTrackOutput alloc]
            initWithTrack:track
            outputSettings:pixelBufferSettings(source_full_range)];
        output.alwaysCopiesSampleData = NO;
        if (reader == nil || ![reader canAddOutput:output]) {
            assignError(error,
                        errorText(reader_error, "cannot create decoded output"));
            return false;
        }
        [reader addOutput:output];
        const CMTime target = timeForFrame(target_frame);
        CMTime start = CMTimeSubtract(
            target, CMTimeMakeWithSeconds(preroll_seconds, 60000));
        if (CMTimeCompare(start, track_start) < 0) {
            start = track_start;
        }
        const CMTime end = CMTimeAdd(track_start, track_duration);
        const CMTime duration = CMTimeSubtract(end, start);
        reader.timeRange = CMTimeRangeMake(start, duration);
        if (![reader startReading]) {
            assignError(error,
                        errorText(reader.error, "cannot start decoded output"));
            return false;
        }
        last_decoded_frame = frameIndexForPts(start) - 1;
        return true;
    }

    std::optional<AppleDecodedVideoFrame> decodeNext(std::string* error) {
        if (output == nil) {
            assignError(error, "video reader is not positioned");
            return std::nullopt;
        }
        while (true) {
        CMSampleBufferRef sample = [output copyNextSampleBuffer];
        if (sample == nullptr) {
            if (reader.status == AVAssetReaderStatusFailed) {
                assignError(error,
                            errorText(reader.error, "video decode failed"));
            }
            return std::nullopt;
        }

        CVPixelBufferRef pixel_buffer = CMSampleBufferGetImageBuffer(sample);
        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        if (pixel_buffer == nullptr || !CMTIME_IS_NUMERIC(pts)) {
            CFRelease(sample);
            assignError(error, "decoded sample has no pixel buffer or PTS");
            return std::nullopt;
        }

        const OSType format = CVPixelBufferGetPixelFormatType(pixel_buffer);
        const size_t plane_count = CVPixelBufferGetPlaneCount(pixel_buffer);
        if ((format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
             format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) ||
            plane_count != 2) {
            CFRelease(sample);
            assignError(error, "AVFoundation did not return two-plane NV12");
            return std::nullopt;
        }

        AppleDecodedVideoFrame frame;
        DecodedFrameMetadata& metadata = frame.metadata;
        metadata.stream_id = info.stream_id;
        metadata.frame_number = static_cast<int>(frameIndexForPts(pts));
        if (metadata.frame_number <= last_decoded_frame ||
            metadata.frame_number < 0 ||
            metadata.frame_number >= info.frame_count) {
            CFRelease(sample);
            continue;
        }
        metadata.local_frame_number = metadata.frame_number;
        metadata.frame_pts = pts.value;
        metadata.time_base = {1, pts.timescale};
        metadata.frame_source_code = 2;
        metadata.width = static_cast<int>(CVPixelBufferGetWidth(pixel_buffer));
        metadata.height = static_cast<int>(CVPixelBufferGetHeight(pixel_buffer));
        metadata.pixel_format = FramePixelFormat::NV12;
        metadata.plane_count = 2;
        size_t logical_offset = 0;
        for (size_t plane = 0; plane < plane_count; ++plane) {
            FramePlaneLayout& layout = metadata.planes[plane];
            layout.offset_bytes = logical_offset;
            layout.row_stride_bytes = static_cast<int>(
                CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, plane));
            layout.width_pixels = static_cast<int>(
                CVPixelBufferGetWidthOfPlane(pixel_buffer, plane));
            layout.height_pixels = static_cast<int>(
                CVPixelBufferGetHeightOfPlane(pixel_buffer, plane));
            layout.bytes_per_element = plane == 0 ? 1 : 2;
            logical_offset +=
                static_cast<size_t>(layout.row_stride_bytes) *
                static_cast<size_t>(layout.height_pixels);
        }
        metadata.pitch_bytes = metadata.planes[0].row_stride_bytes;
        metadata.frame_bytes = logical_offset;
        metadata.color_matrix =
            colorMatrixFromPixelBuffer(pixel_buffer, info.color_matrix);
        metadata.color_range = colorRangeFromPixelFormat(format);
        metadata.surface_backend = FrameSurfaceBackend::AppleVideoToolbox;
        metadata.ownership = FrameSurfaceOwnership::ReferenceCounted;
        metadata.lifetime = FrameSurfaceLifetime::ReferenceCounted;
        frame.surface = std::make_shared<ApplePixelBufferFrameSurface>(
            pixel_buffer, frameSurfaceDescriptorFromMetadata(metadata));
        last_decoded_frame = metadata.frame_number;
        CFRelease(sample);
        return frame;
        }
    }
};

AppleVideoFrameProvider::AppleVideoFrameProvider()
    : impl_(std::make_unique<Impl>()) {}

AppleVideoFrameProvider::~AppleVideoFrameProvider() = default;

AppleVideoFrameProvider::AppleVideoFrameProvider(
    AppleVideoFrameProvider&&) noexcept = default;

AppleVideoFrameProvider& AppleVideoFrameProvider::operator=(
    AppleVideoFrameProvider&&) noexcept = default;

bool AppleVideoFrameProvider::open(const std::string& path,
                                   const std::string& stream_id,
                                   std::string* error) {
    impl_->close();
    NSString* file_path = [NSString stringWithUTF8String:path.c_str()];
    BOOL is_directory = NO;
    if (file_path == nil ||
        ![[NSFileManager defaultManager] fileExistsAtPath:file_path
                                               isDirectory:&is_directory] ||
        is_directory) {
        assignError(error, "video path is not a readable regular file");
        return false;
    }
    impl_->asset = [AVURLAsset
        URLAssetWithURL:[NSURL fileURLWithPath:file_path]
                options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
    if (!impl_->loadTrack(error)) {
        impl_->close();
        return false;
    }

    CMFormatDescriptionRef format_description = nullptr;
    if (impl_->track.formatDescriptions.count > 0) {
        format_description = (__bridge CMFormatDescriptionRef)
            impl_->track.formatDescriptions.firstObject;
    }
    impl_->source_full_range =
        formatDescriptionIsFullRange(format_description);
    impl_->track_start = impl_->track.timeRange.start;
    impl_->track_duration = impl_->track.timeRange.duration;
    impl_->frame_duration = impl_->track.minFrameDuration;
    if (!CMTIME_IS_NUMERIC(impl_->frame_duration) ||
        CMTimeCompare(impl_->frame_duration, kCMTimeZero) <= 0) {
        const double nominal_fps = impl_->track.nominalFrameRate;
        if (nominal_fps > 0.0) {
            impl_->frame_duration =
                CMTimeMakeWithSeconds(1.0 / nominal_fps, 60000);
        }
    }
    const CGSize dimensions = impl_->track.naturalSize;
    impl_->info.path = path;
    impl_->info.stream_id = stream_id.empty() ? "camera-main" : stream_id;
    impl_->info.width = static_cast<int>(std::llround(std::fabs(dimensions.width)));
    impl_->info.height = static_cast<int>(std::llround(std::fabs(dimensions.height)));
    impl_->info.nominal_frame_rate = impl_->track.nominalFrameRate;
    impl_->info.duration_seconds = seconds(impl_->track_duration);
    const double frame_seconds = seconds(impl_->frame_duration);
    impl_->info.frame_count =
        std::isfinite(frame_seconds) && frame_seconds > 0.0
            ? static_cast<int64_t>(
                  std::llround(impl_->info.duration_seconds / frame_seconds))
            : 0;
    impl_->info.color_matrix =
        colorMatrixFromFormatDescription(format_description);
    impl_->info.color_range = impl_->source_full_range ? ColorRange_JPEG
                                                       : ColorRange_MPEG;
    if (impl_->info.width <= 0 || impl_->info.height <= 0 ||
        impl_->info.frame_count <= 0 ||
        !std::isfinite(impl_->info.duration_seconds)) {
        assignError(error, "video track has invalid dimensions, duration, or samples");
        impl_->close();
        return false;
    }
    return seekToFrame(0, error);
}

void AppleVideoFrameProvider::close() { impl_->close(); }

void AppleVideoFrameProvider::suspendDecoding() { impl_->suspendDecoding(); }

bool AppleVideoFrameProvider::isOpen() const {
    return impl_ != nullptr && impl_->asset != nil && impl_->track != nil &&
           impl_->info.frame_count > 0;
}

const AppleVideoAssetInfo& AppleVideoFrameProvider::info() const {
    return impl_->info;
}

bool AppleVideoFrameProvider::seekToFrame(int64_t frame_number,
                                          std::string* error) {
    if (!isOpen()) {
        assignError(error, "video provider is not open");
        return false;
    }
    if (frame_number < 0 || frame_number >= impl_->info.frame_count) {
        assignError(error, "requested frame is outside the video sample range");
        return false;
    }
    const double preroll_attempts[] = {0.0, 2.0, 4.0, 8.0, 16.0, 32.0};
    std::string last_error;
    for (const double preroll : preroll_attempts) {
        if (!impl_->startReaderAtFrame(frame_number, preroll, &last_error)) {
            continue;
        }
        while (true) {
            auto frame = impl_->decodeNext(&last_error);
            if (!frame || frame->metadata.frame_number > frame_number) {
                break;
            }
            if (frame->metadata.frame_number < frame_number) {
                continue;
            }
            frame->metadata.frame_source_code = 1;
            impl_->pending_frame = std::move(frame);
            if (error != nullptr) {
                error->clear();
            }
            return true;
        }
    }
    assignError(error, last_error.empty()
                           ? "exact frame was not returned after bounded preroll"
                           : last_error);
    return false;
}

std::optional<AppleDecodedVideoFrame>
AppleVideoFrameProvider::readNext(std::string* error) {
    if (!isOpen()) {
        assignError(error, "video provider is not open");
        return std::nullopt;
    }
    if (impl_->pending_frame) {
        auto frame = std::move(impl_->pending_frame);
        impl_->pending_frame.reset();
        return frame;
    }
    return impl_->decodeNext(error);
}

std::optional<AppleDecodedVideoFrame>
AppleVideoFrameProvider::readFrame(int64_t frame_number, std::string* error) {
    if (!seekToFrame(frame_number, error)) {
        return std::nullopt;
    }
    return readNext(error);
}
