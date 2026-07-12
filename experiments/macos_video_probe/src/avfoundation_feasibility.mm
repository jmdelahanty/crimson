#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct AssetInfo {
    std::string label;
    std::string path;
    AVURLAsset* asset = nil;
    AVAssetTrack* track = nil;
    double duration = 0.0;
    double fps = 0.0;
    std::vector<double> sync_sample_pts;
};

struct SampleResult {
    bool ok = false;
    double requested = 0.0;
    double actual = 0.0;
    double latency = 0.0;
    int width = 0;
    int height = 0;
    bool used_preroll = false;
    std::string error;
};

NSDictionary* pixelBufferSettings() {
    return @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
}

bool loadAsset(AssetInfo& info) {
    NSString* filePath = [NSString stringWithUTF8String:info.path.c_str()];
    BOOL isDirectory = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:filePath
                                              isDirectory:&isDirectory] ||
        isDirectory) {
        std::printf("[FAIL] asset=%s file does not exist or is not a regular "
                    "file path=%s\n",
                    info.label.c_str(), info.path.c_str());
        return false;
    }
    if (![[NSFileManager defaultManager] isReadableFileAtPath:filePath]) {
        std::printf("[FAIL] asset=%s file is not readable path=%s\n",
                    info.label.c_str(), info.path.c_str());
        return false;
    }
    NSURL* url = [NSURL fileURLWithPath:filePath];
    info.asset = [AVURLAsset URLAssetWithURL:url options:nil];
    __block NSArray<AVAssetTrack*>* tracks = nil;
    __block NSError* trackError = nil;
    dispatch_semaphore_t trackReady = dispatch_semaphore_create(0);
    [info.asset loadTracksWithMediaType:AVMediaTypeVideo
                     completionHandler:^(NSArray<AVAssetTrack*>* loadedTracks,
                                         NSError* error) {
        tracks = loadedTracks;
        trackError = error;
        dispatch_semaphore_signal(trackReady);
    }];
    const long waitStatus = dispatch_semaphore_wait(
        trackReady, dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC));
    if (waitStatus != 0) {
        std::printf("[FAIL] asset=%s timed out loading video tracks\n",
                    info.label.c_str());
        return false;
    }
    if (trackError) {
        std::printf("[FAIL] asset=%s track loading error=%s\n",
                    info.label.c_str(),
                    trackError.localizedDescription.UTF8String);
        return false;
    }
    if (tracks.count == 0) {
        std::printf("[FAIL] asset=%s has no video track\n", info.label.c_str());
        return false;
    }
    info.track = tracks.firstObject;
    __block NSError* durationError = nil;
    dispatch_semaphore_t durationReady = dispatch_semaphore_create(0);
    [info.asset loadValuesAsynchronouslyForKeys:@[@"duration"]
                              completionHandler:^{
        NSError* error = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const AVKeyValueStatus status =
            [info.asset statusOfValueForKey:@"duration" error:&error];
#pragma clang diagnostic pop
        if (status != AVKeyValueStatusLoaded) durationError = error;
        dispatch_semaphore_signal(durationReady);
    }];
    const long durationWaitStatus = dispatch_semaphore_wait(
        durationReady, dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC));
    if (durationWaitStatus != 0 || durationError) {
        std::printf("[FAIL] asset=%s duration loading %s%s\n",
                    info.label.c_str(),
                    durationWaitStatus != 0 ? "timed out" : "failed",
                    durationError
                        ? durationError.localizedDescription.UTF8String
                        : "");
        return false;
    }
    const CMTime duration = info.asset.duration;
    info.duration = CMTimeGetSeconds(duration);
    info.fps = info.track.nominalFrameRate;
    if (!std::isfinite(info.duration) || info.duration <= 0.0 || info.fps <= 0.0) {
        std::printf("[FAIL] asset=%s invalid duration/fps duration=%.6f fps=%.6f\n",
                    info.label.c_str(), info.duration, info.fps);
        return false;
    }
    NSError* indexError = nil;
    AVAssetReader* indexReader =
        [[AVAssetReader alloc] initWithAsset:info.asset error:&indexError];
    AVAssetReaderTrackOutput* compressedOutput =
        [[AVAssetReaderTrackOutput alloc] initWithTrack:info.track
                                        outputSettings:nil];
    compressedOutput.alwaysCopiesSampleData = NO;
    if (!indexReader || ![indexReader canAddOutput:compressedOutput]) {
        std::printf("[FAIL] asset=%s cannot create compressed keyframe index: %s\n",
                    info.label.c_str(),
                    indexError.localizedDescription.UTF8String ?: "unsupported output");
        return false;
    }
    [indexReader addOutput:compressedOutput];
    if (![indexReader startReading]) {
        std::printf("[FAIL] asset=%s cannot start keyframe index: %s\n",
                    info.label.c_str(),
                    indexReader.error.localizedDescription.UTF8String ?: "unknown");
        return false;
    }
    CMSampleBufferRef compressedSample = nullptr;
    while ((compressedSample = [compressedOutput copyNextSampleBuffer])) {
        bool isSync = true;
        CFArrayRef attachments =
            CMSampleBufferGetSampleAttachmentsArray(compressedSample, false);
        if (attachments && CFArrayGetCount(attachments) > 0) {
            CFDictionaryRef values = static_cast<CFDictionaryRef>(
                CFArrayGetValueAtIndex(attachments, 0));
            CFBooleanRef notSync = static_cast<CFBooleanRef>(
                CFDictionaryGetValue(values, kCMSampleAttachmentKey_NotSync));
            if (notSync && CFBooleanGetValue(notSync)) isSync = false;
        }
        if (isSync) {
            const double pts = CMTimeGetSeconds(
                CMSampleBufferGetPresentationTimeStamp(compressedSample));
            if (std::isfinite(pts)) info.sync_sample_pts.push_back(pts);
        }
        CFRelease(compressedSample);
        compressedSample = nullptr;
    }
    if (info.sync_sample_pts.empty()) {
        std::printf("[FAIL] asset=%s compressed track reported no sync samples\n",
                    info.label.c_str());
        return false;
    }
    const CGSize size = info.track.naturalSize;
    double maxGop = 0.0;
    for (size_t i = 1; i < info.sync_sample_pts.size(); ++i) {
        maxGop = std::max(maxGop,
                          info.sync_sample_pts[i] - info.sync_sample_pts[i - 1]);
    }
    std::printf("[ASSET] label=%s duration=%.6f fps=%.6f dimensions=%.0fx%.0f "
                "sync_samples=%zu max_gop_s=%.6f path=%s\n",
                info.label.c_str(), info.duration, info.fps,
                std::fabs(size.width), std::fabs(size.height),
                info.sync_sample_pts.size(), maxGop, info.path.c_str());
    return true;
}

double precedingSyncSample(const AssetInfo& info, double target) {
    auto it = std::upper_bound(info.sync_sample_pts.begin(),
                               info.sync_sample_pts.end(), target + 0.000001);
    if (it == info.sync_sample_pts.begin()) return 0.0;
    return *std::prev(it);
}

SampleResult readFirstFrameAtInternal(const AssetInfo& info, double seconds,
                                      double prerollSeconds) {
    SampleResult result;
    result.requested = seconds;
    NSError* error = nil;
    AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:info.asset
                                                          error:&error];
    if (!reader) {
        result.error = error.localizedDescription.UTF8String ?: "reader creation failed";
        return result;
    }
    AVAssetReaderTrackOutput* output =
        [[AVAssetReaderTrackOutput alloc] initWithTrack:info.track
                                        outputSettings:pixelBufferSettings()];
    output.alwaysCopiesSampleData = NO;
    if (![reader canAddOutput:output]) {
        result.error = "cannot add video output";
        return result;
    }
    [reader addOutput:output];
    const double readerStart = std::max(0.0, seconds - prerollSeconds);
    reader.timeRange = CMTimeRangeMake(
        CMTimeMakeWithSeconds(readerStart, 60000),
        CMTimeMakeWithSeconds(prerollSeconds + 2.0, 60000));
    const auto begin = Clock::now();
    if (![reader startReading]) {
        result.error = reader.error.localizedDescription.UTF8String ?: "startReading failed";
        return result;
    }
    CMSampleBufferRef sample = nullptr;
    const double halfFrame = 0.5 / info.fps;
    while ((sample = [output copyNextSampleBuffer])) {
        const double pts = CMTimeGetSeconds(
            CMSampleBufferGetPresentationTimeStamp(sample));
        if (pts + halfFrame >= seconds) break;
        CFRelease(sample);
        sample = nullptr;
    }
    result.latency = std::chrono::duration<double>(Clock::now() - begin).count();
    if (!sample) {
        result.error = reader.error.localizedDescription.UTF8String ?: "no sample returned";
        return result;
    }
    result.actual = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
    CVPixelBufferRef image = CMSampleBufferGetImageBuffer(sample);
    if (image) {
        result.width = static_cast<int>(CVPixelBufferGetWidth(image));
        result.height = static_cast<int>(CVPixelBufferGetHeight(image));
    }
    result.ok = image != nullptr && std::isfinite(result.actual);
    result.used_preroll = prerollSeconds > 0.0;
    if (!result.ok) result.error = "sample did not contain a valid pixel buffer/PTS";
    CFRelease(sample);
    [reader cancelReading];
    return result;
}

SampleResult readFirstFrameAt(const AssetInfo& info, double seconds) {
    SampleResult exact = readFirstFrameAtInternal(info, seconds, 0.0);
    if (exact.ok) return exact;
    const double syncPts = precedingSyncSample(info, seconds);
    SampleResult preroll = readFirstFrameAtInternal(
        info, seconds, std::max(0.0, seconds - syncPts));
    if (!preroll.ok && !exact.error.empty()) {
        preroll.error = "exact-start=" + exact.error + "; preroll=" +
                        preroll.error;
    }
    return preroll;
}

bool exactSeekSuite(const AssetInfo& info) {
    const double fractions[] = {0.10, 0.25, 0.50, 0.75, 0.90};
    bool passed = true;
    std::printf("\n[TEST] exact_random_access asset=%s\n", info.label.c_str());
    for (double fraction : fractions) {
        const int64_t frame = static_cast<int64_t>(
            std::floor(info.duration * fraction * info.fps));
        const double target = frame / info.fps;
        const SampleResult sample = readFirstFrameAt(info, target);
        const double errorFrames = sample.ok
            ? std::fabs(sample.actual - target) * info.fps
            : INFINITY;
        const bool samplePass = sample.ok && errorFrames <= 0.51;
        passed = passed && samplePass;
        std::printf("[%s] target_frame=%lld requested_pts=%.6f actual_pts=%.6f "
                    "error_frames=%.3f latency_ms=%.1f dimensions=%dx%d "
                    "path=%s%s%s\n",
                    samplePass ? "PASS" : "FAIL",
                    static_cast<long long>(frame), target, sample.actual,
                    errorFrames, sample.latency * 1000.0, sample.width,
                    sample.height, sample.used_preroll ? "preroll" : "exact",
                    sample.error.empty() ? "" : " error=",
                    sample.error.c_str());
    }
    return passed;
}

bool steppingSuite(const AssetInfo& info) {
    constexpr int kSteps = 20;
    const int64_t startFrame = static_cast<int64_t>(
        std::floor(info.duration * 0.33 * info.fps));
    const double target = startFrame / info.fps;
    std::printf("\n[TEST] consecutive_paused_frames asset=%s start_frame=%lld steps=%d\n",
                info.label.c_str(), static_cast<long long>(startFrame), kSteps);

    std::vector<double> framePts;
    bool usedPreroll = false;
    std::string lastError;
    for (int attempt = 0; attempt < 2 && framePts.size() < kSteps; ++attempt) {
        const double preroll = attempt == 0
                                   ? 0.0
                                   : std::max(0.0, target -
                                                       precedingSyncSample(info, target));
        NSError* error = nil;
        AVAssetReader* reader =
            [[AVAssetReader alloc] initWithAsset:info.asset error:&error];
        if (!reader) {
            lastError = error.localizedDescription.UTF8String ?: "reader creation failed";
            continue;
        }
        AVAssetReaderTrackOutput* output =
            [[AVAssetReaderTrackOutput alloc] initWithTrack:info.track
                                            outputSettings:pixelBufferSettings()];
        output.alwaysCopiesSampleData = NO;
        [reader addOutput:output];
        const double readerStart = std::max(0.0, target - preroll);
        reader.timeRange = CMTimeRangeMake(
            CMTimeMakeWithSeconds(readerStart, 60000),
            CMTimeMakeWithSeconds(preroll + 2.0, 60000));
        if (![reader startReading]) {
            lastError = reader.error.localizedDescription.UTF8String ?: "startReading failed";
            continue;
        }
        framePts.clear();
        CMSampleBufferRef sample = nullptr;
        const double halfFrame = 0.5 / info.fps;
        while (framePts.size() < kSteps &&
               (sample = [output copyNextSampleBuffer])) {
            const double pts = CMTimeGetSeconds(
                CMSampleBufferGetPresentationTimeStamp(sample));
            if (pts + halfFrame >= target) framePts.push_back(pts);
            CFRelease(sample);
            sample = nullptr;
        }
        if (framePts.size() < kSteps) {
            lastError = reader.error.localizedDescription.UTF8String ?:
                        "insufficient decoded samples";
            framePts.clear();
        } else {
            usedPreroll = preroll > 0.0;
        }
        [reader cancelReading];
    }
    if (framePts.size() < kSteps) {
        std::printf("[FAIL] unable to decode consecutive frames: %s\n",
                    lastError.c_str());
        return false;
    }
    std::printf("[INFO] decode_path=%s\n", usedPreroll ? "preroll" : "exact");
    bool passed = true;
    double previous = -1.0;
    for (int step = 0; step < kSteps; ++step) {
        const double pts = framePts[step];
        const double expected = target + step / info.fps;
        const double errorFrames = std::fabs(pts - expected) * info.fps;
        const double deltaFrames = previous >= 0.0 ? (pts - previous) * info.fps : 1.0;
        const bool stepPass = std::isfinite(pts) && errorFrames <= 0.51 &&
                              (step == 0 || std::fabs(deltaFrames - 1.0) <= 0.10);
        passed = passed && stepPass;
        std::printf("[%s] step=%d expected_pts=%.6f actual_pts=%.6f "
                    "error_frames=%.3f delta_frames=%.3f\n",
                    stepPass ? "PASS" : "FAIL", step, expected, pts,
                    errorFrames, deltaFrames);
        previous = pts;
    }
    return passed;
}

bool synchronizationSuite(const std::vector<AssetInfo>& assets) {
    if (assets.size() < 2) {
        std::printf("\n[SKIP] multistream_common_time: supply --secondary LABEL=VIDEO\n");
        return true;
    }
    double commonDuration = assets.front().duration;
    for (const auto& asset : assets) commonDuration = std::min(commonDuration, asset.duration);
    const double target = commonDuration * 0.40;
    std::printf("\n[TEST] multistream_common_time target=%.6f streams=%zu\n",
                target, assets.size());
    bool passed = true;
    double minResidual = INFINITY, maxResidual = -INFINITY;
    for (const auto& asset : assets) {
        const int64_t frame = static_cast<int64_t>(std::llround(target * asset.fps));
        const double streamTarget = frame / asset.fps;
        const SampleResult sample = readFirstFrameAt(asset, streamTarget);
        const double residual = sample.actual - streamTarget;
        const double errorFrames = sample.ok ? std::fabs(residual) * asset.fps : INFINITY;
        const bool streamPass = sample.ok && errorFrames <= 0.51;
        passed = passed && streamPass;
        if (sample.ok) {
            minResidual = std::min(minResidual, residual);
            maxResidual = std::max(maxResidual, residual);
        }
        std::printf("[%s] stream=%s frame=%lld target_pts=%.6f actual_pts=%.6f "
                    "residual_ms=%.3f latency_ms=%.1f\n",
                    streamPass ? "PASS" : "FAIL", asset.label.c_str(),
                    static_cast<long long>(frame), streamTarget, sample.actual,
                    residual * 1000.0, sample.latency * 1000.0);
    }
    if (std::isfinite(minResidual) && std::isfinite(maxResidual)) {
        std::printf("[INFO] residual_spread_ms=%.3f (capability check only; "
                    "Zarr alignment not applied)\n",
                    (maxResidual - minResidual) * 1000.0);
    }
    return passed;
}

struct AlignmentRow {
    int64_t stimulus_frame = -1;
    bool interpolated = false;
    bool valid = false;
};

std::vector<AlignmentRow> loadAlignmentCsv(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open alignment CSV: " + path);
    std::string line;
    std::getline(input, line);
    if (line != "camera_frame,stimulus_frame,interpolated,valid\r" &&
        line != "camera_frame,stimulus_frame,interpolated,valid") {
        throw std::runtime_error("unexpected alignment CSV header: " + line);
    }
    std::vector<AlignmentRow> rows;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::stringstream parser(line);
        std::string field;
        int64_t cameraFrame = -1;
        AlignmentRow row;
        std::getline(parser, field, ','); cameraFrame = std::stoll(field);
        std::getline(parser, field, ','); row.stimulus_frame = std::stoll(field);
        std::getline(parser, field, ','); row.interpolated = std::stoi(field) != 0;
        std::getline(parser, field, ','); row.valid = std::stoi(field) != 0;
        if (cameraFrame != static_cast<int64_t>(rows.size())) {
            throw std::runtime_error("alignment camera_frame is not dense at row " +
                                     std::to_string(rows.size()));
        }
        rows.push_back(row);
    }
    return rows;
}

bool mappedAlignmentSuite(const std::vector<AssetInfo>& assets,
                          const std::string& alignmentPath) {
    if (alignmentPath.empty()) return true;
    if (assets.size() < 3) {
        std::printf("\n[FAIL] zarr_alignment requires primary, crop, and stimulus\n");
        return false;
    }
    const AssetInfo* crop = nullptr;
    const AssetInfo* stimulus = nullptr;
    for (const auto& asset : assets) {
        if (asset.label == "crop") crop = &asset;
        if (asset.label == "stimulus") stimulus = &asset;
    }
    if (!crop || !stimulus) {
        std::printf("\n[FAIL] zarr_alignment requires secondary labels crop and stimulus\n");
        return false;
    }
    std::vector<AlignmentRow> rows;
    try {
        rows = loadAlignmentCsv(alignmentPath);
    } catch (const std::exception& error) {
        std::printf("\n[FAIL] zarr_alignment load error=%s\n", error.what());
        return false;
    }
    const AssetInfo& main = assets.front();
    const double fractions[] = {0.10, 0.25, 0.50, 0.75, 0.90};
    bool passed = true;
    std::printf("\n[TEST] zarr_mapped_alignment rows=%zu path=%s\n",
                rows.size(), alignmentPath.c_str());
    for (double fraction : fractions) {
        int64_t cameraFrame = static_cast<int64_t>(rows.size() * fraction);
        while (cameraFrame < static_cast<int64_t>(rows.size()) &&
               !rows[cameraFrame].valid) ++cameraFrame;
        if (cameraFrame >= static_cast<int64_t>(rows.size())) {
            std::printf("[FAIL] no valid mapping near fraction=%.2f\n", fraction);
            passed = false;
            continue;
        }
        const int64_t stimulusFrame = rows[cameraFrame].stimulus_frame;
        const double mainTarget = cameraFrame / main.fps;
        const double cropTarget = cameraFrame / crop->fps;
        const double stimulusTarget = stimulusFrame / stimulus->fps;
        const SampleResult mainSample = readFirstFrameAt(main, mainTarget);
        const SampleResult cropSample = readFirstFrameAt(*crop, cropTarget);
        const SampleResult stimulusSample =
            readFirstFrameAt(*stimulus, stimulusTarget);
        const double mainError = mainSample.ok
            ? std::fabs(mainSample.actual - mainTarget) * main.fps : INFINITY;
        const double cropError = cropSample.ok
            ? std::fabs(cropSample.actual - cropTarget) * crop->fps : INFINITY;
        const double stimulusError = stimulusSample.ok
            ? std::fabs(stimulusSample.actual - stimulusTarget) * stimulus->fps
            : INFINITY;
        const bool rowPass = mainError <= 0.51 && cropError <= 0.51 &&
                             stimulusError <= 0.51;
        passed = passed && rowPass;
        std::printf("[%s] camera_frame=%lld stimulus_frame=%lld interpolated=%d "
                    "main_pts=%.6f crop_pts=%.6f stimulus_pts=%.6f "
                    "errors_frames=%.3f/%.3f/%.3f latency_ms=%.1f/%.1f/%.1f\n",
                    rowPass ? "PASS" : "FAIL",
                    static_cast<long long>(cameraFrame),
                    static_cast<long long>(stimulusFrame),
                    rows[cameraFrame].interpolated ? 1 : 0,
                    mainSample.actual, cropSample.actual, stimulusSample.actual,
                    mainError, cropError, stimulusError,
                    mainSample.latency * 1000.0, cropSample.latency * 1000.0,
                    stimulusSample.latency * 1000.0);
    }
    return passed;
}

void usage(const char* program) {
    std::fprintf(stderr,
        "Usage: %s PRIMARY_VIDEO [--secondary LABEL=VIDEO]... "
        "[--alignment CSV]\n", program);
}

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) { usage(argv[0]); return 2; }
        std::vector<AssetInfo> assets;
        std::string alignmentPath;
        AssetInfo primary;
        primary.label = "primary";
        primary.path = argv[1];
        assets.push_back(std::move(primary));
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--alignment" && i + 1 < argc) {
                alignmentPath = argv[++i];
                continue;
            }
            if (arg != "--secondary" || i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            const std::string value = argv[++i];
            const size_t separator = value.find('=');
            if (separator == std::string::npos || separator == 0 ||
                separator + 1 >= value.size()) {
                usage(argv[0]);
                return 2;
            }
            AssetInfo secondary;
            secondary.label = value.substr(0, separator);
            secondary.path = value.substr(separator + 1);
            assets.push_back(std::move(secondary));
        }

        std::printf("Crimson AVFoundation feasibility tests\n");
        std::printf("Criteria: returned PTS within 0.51 source frames of requested PTS\n");
        bool passed = true;
        for (auto& asset : assets) passed = loadAsset(asset) && passed;
        if (!passed) return 1;
        for (const auto& asset : assets) {
            passed = exactSeekSuite(asset) && passed;
            passed = steppingSuite(asset) && passed;
        }
        passed = synchronizationSuite(assets) && passed;
        passed = mappedAlignmentSuite(assets, alignmentPath) && passed;
        std::printf("\n[SUMMARY] status=%s assets=%zu\n",
                    passed ? "PASS" : "FAIL", assets.size());
        return passed ? 0 : 1;
    }
}
