#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>
}

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

std::string ffmpegError(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    return text;
}

struct VideoFrame {
    CVPixelBufferRef pixel_buffer = nullptr;
    double pts_seconds = 0.0;
    int64_t ordinal = -1;

    VideoFrame() = default;
    VideoFrame(CVPixelBufferRef buffer, double pts, int64_t index)
        : pixel_buffer(buffer), pts_seconds(pts), ordinal(index) {
        if (pixel_buffer) CVPixelBufferRetain(pixel_buffer);
    }
    VideoFrame(const VideoFrame& other)
        : VideoFrame(other.pixel_buffer, other.pts_seconds, other.ordinal) {}
    VideoFrame(VideoFrame&& other) noexcept
        : pixel_buffer(other.pixel_buffer), pts_seconds(other.pts_seconds),
          ordinal(other.ordinal) {
        other.pixel_buffer = nullptr;
    }
    VideoFrame& operator=(VideoFrame other) noexcept {
        std::swap(pixel_buffer, other.pixel_buffer);
        std::swap(pts_seconds, other.pts_seconds);
        std::swap(ordinal, other.ordinal);
        return *this;
    }
    ~VideoFrame() {
        if (pixel_buffer) CVPixelBufferRelease(pixel_buffer);
    }
};

class VideoDecoder {
  public:
    VideoDecoder(std::string path, size_t queue_capacity, bool realtime)
        : path_(std::move(path)), queue_capacity_(queue_capacity),
          realtime_(realtime) {}
    ~VideoDecoder() { stop(); close(); }

    bool open() {
        int status = avformat_open_input(&format_, path_.c_str(), nullptr, nullptr);
        if (status < 0) return fail("avformat_open_input", status);
        status = avformat_find_stream_info(format_, nullptr);
        if (status < 0) return fail("avformat_find_stream_info", status);
        stream_index_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1,
                                            nullptr, 0);
        if (stream_index_ < 0) return fail("av_find_best_stream", stream_index_);
        stream_ = format_->streams[stream_index_];
        const AVCodec* codec = avcodec_find_decoder(stream_->codecpar->codec_id);
        if (!codec) {
            std::fprintf(stderr, "No decoder for codec %d\n",
                         stream_->codecpar->codec_id);
            return false;
        }
        codec_ = avcodec_alloc_context3(codec);
        if (!codec_) return false;
        status = avcodec_parameters_to_context(codec_, stream_->codecpar);
        if (status < 0) return fail("avcodec_parameters_to_context", status);
        status = av_hwdevice_ctx_create(&hardware_device_,
                                        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                        nullptr, nullptr, 0);
        if (status < 0) return fail("VideoToolbox device creation", status);
        codec_->hw_device_ctx = av_buffer_ref(hardware_device_);
        if (realtime_) {
            // Do not request output for non-reference pictures. Reference
            // pictures are still decoded because later pictures depend on
            // them. This is an FFmpeg decoder-side playback experiment, not
            // renderer-side frame dropping.
            codec_->skip_frame = AVDISCARD_NONREF;
        }
        codec_->opaque = this;
        codec_->get_format = [](AVCodecContext*, const AVPixelFormat* formats) {
            for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
                if (*f == AV_PIX_FMT_VIDEOTOOLBOX) return *f;
            }
            return AV_PIX_FMT_NONE;
        };
        status = avcodec_open2(codec_, codec, nullptr);
        if (status < 0) return fail("avcodec_open2", status);
        duration_seconds_ = format_->duration > 0
                                ? format_->duration / static_cast<double>(AV_TIME_BASE)
                                : 0.0;
        const AVRational fps = av_guess_frame_rate(format_, stream_, nullptr);
        fps_ = fps.den ? av_q2d(fps) : 0.0;
        std::printf("Input: %dx%d %.3f fps duration %.3f s codec %s\n",
                    codec_->width, codec_->height, fps_, duration_seconds_,
                    codec->name);
        std::printf("Decoder output requested: videotoolbox hardware frames (%s mode)\n",
                    realtime_ ? "realtime/non-reference output omitted"
                              : "complete/every-frame");
        return true;
    }

    void start() {
        stopping_ = false;
        worker_ = std::thread([this] { decodeLoop(); });
    }
    void stop() {
        stopping_ = true;
        queue_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    void requestSeek(double seconds) {
        pending_seek_.store(std::max(0.0, std::min(seconds, duration_seconds_)));
        seek_pending_ = true;
        queue_cv_.notify_all();
    }
    bool takeFrameForTime(double target, VideoFrame& selected,
                          uint64_t& discarded) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        bool found = false;
        while (!queue_.empty() && queue_.front().pts_seconds <= target) {
            if (found) ++discarded;
            selected = std::move(queue_.front());
            queue_.pop_front();
            found = true;
        }
        queue_cv_.notify_one();
        return found;
    }
    bool takeFirstFrame(VideoFrame& selected) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.empty()) return false;
        selected = std::move(queue_.front());
        queue_.pop_front();
        queue_cv_.notify_one();
        return true;
    }
    size_t queued() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return queue_.size();
    }
    uint64_t decoded() const { return decoded_.load(); }
    uint64_t generation() const { return generation_.load(); }
    bool failed() const { return failed_.load(); }
    bool eof() const { return eof_.load(); }
    double duration() const { return duration_seconds_; }
    double fps() const { return fps_; }

  private:
    bool fail(const char* operation, int status) {
        std::fprintf(stderr, "%s failed: %s\n", operation,
                     ffmpegError(status).c_str());
        failed_ = true;
        return false;
    }
    void clearQueue() {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.clear();
    }
    void performSeek(double seconds) {
        const int64_t timestamp = av_rescale_q(
            static_cast<int64_t>(seconds * AV_TIME_BASE), AV_TIME_BASE_Q,
            stream_->time_base);
        const auto begin = Clock::now();
        const int status = av_seek_frame(format_, stream_index_, timestamp,
                                         AVSEEK_FLAG_BACKWARD);
        if (status < 0) {
            fail("av_seek_frame", status);
            return;
        }
        avcodec_flush_buffers(codec_);
        clearQueue();
        ordinal_ = -1;
        eof_ = false;
        generation_++;
        last_seek_started_ = begin;
        discard_before_pts_ = seconds;
        measuring_seek_ = true;
    }
    void pushFrame(AVFrame* frame) {
        if (frame->format != AV_PIX_FMT_VIDEOTOOLBOX || !frame->data[3]) {
            std::fprintf(stderr, "Unexpected decoder format: %s\n",
                         av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)));
            failed_ = true;
            return;
        }
        const int64_t pts = frame->best_effort_timestamp;
        const double seconds = pts == AV_NOPTS_VALUE
                                   ? (fps_ > 0.0 ? decoded_.load() / fps_ : 0.0)
                                   : pts * av_q2d(stream_->time_base);
        if (discard_before_pts_ >= 0.0 && seconds + 0.000001 < discard_before_pts_) {
            return;
        }
        discard_before_pts_ = -1.0;
        if (measuring_seek_) {
            const double latency =
                std::chrono::duration<double>(Clock::now() - last_seek_started_)
                    .count();
            std::printf("Seek first frame: requested %.3f s, received %.3f s, "
                        "latency %.3f s\n",
                        pending_seek_.load(), seconds, latency);
            measuring_seek_ = false;
        }
        VideoFrame output(reinterpret_cast<CVPixelBufferRef>(frame->data[3]),
                          seconds, ++ordinal_);
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [&] {
            return stopping_.load() || seek_pending_.load() ||
                   queue_.size() < queue_capacity_;
        });
        if (stopping_ || seek_pending_) return;
        queue_.push_back(std::move(output));
        decoded_++;
    }
    void drainDecoder(AVFrame* frame) {
        while (!stopping_ && !seek_pending_) {
            const int status = avcodec_receive_frame(codec_, frame);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
            if (status < 0) {
                fail("avcodec_receive_frame", status);
                return;
            }
            pushFrame(frame);
            av_frame_unref(frame);
        }
    }
    void decodeLoop() {
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        while (!stopping_ && !failed_) {
            if (seek_pending_.exchange(false)) performSeek(pending_seek_.load());
            const int status = av_read_frame(format_, packet);
            if (status == AVERROR_EOF) {
                avcodec_send_packet(codec_, nullptr);
                drainDecoder(frame);
                eof_ = true;
                break;
            }
            if (status < 0) {
                fail("av_read_frame", status);
                break;
            }
            if (packet->stream_index == stream_index_) {
                const int send_status = avcodec_send_packet(codec_, packet);
                if (send_status < 0 && send_status != AVERROR(EAGAIN)) {
                    fail("avcodec_send_packet", send_status);
                } else {
                    drainDecoder(frame);
                }
            }
            av_packet_unref(packet);
        }
        av_frame_free(&frame);
        av_packet_free(&packet);
    }
    void close() {
        clearQueue();
        avcodec_free_context(&codec_);
        av_buffer_unref(&hardware_device_);
        avformat_close_input(&format_);
    }

    std::string path_;
    size_t queue_capacity_;
    bool realtime_ = false;
    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVBufferRef* hardware_device_ = nullptr;
    AVStream* stream_ = nullptr;
    int stream_index_ = -1;
    double duration_seconds_ = 0.0;
    double fps_ = 0.0;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<VideoFrame> queue_;
    std::thread worker_;
    std::atomic<bool> stopping_{false}, failed_{false}, eof_{false};
    std::atomic<bool> seek_pending_{false};
    std::atomic<double> pending_seek_{0.0};
    std::atomic<uint64_t> decoded_{0}, generation_{0};
    int64_t ordinal_ = -1;
    Clock::time_point last_seek_started_{};
    double discard_before_pts_ = -1.0;
    bool measuring_seek_ = false;
};

struct ProbeOptions {
    std::string video_path;
    std::string metrics_path = "macos_video_probe_metrics.csv";
    size_t buffer_frames = 12;
    double run_seconds = 0.0;
    bool realtime = false;
};

}  // namespace

@interface ProbeRenderer : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView*)view
                      decoder:(VideoDecoder*)decoder
                  metricsPath:(const std::string&)metricsPath
                   runSeconds:(double)runSeconds;
- (void)togglePause;
- (void)seekBy:(double)seconds;
@end

@implementation ProbeRenderer {
    MTKView* _view;
    VideoDecoder* _decoder;
    id<MTLCommandQueue> _commands;
    id<MTLRenderPipelineState> _pipeline;
    CVMetalTextureCacheRef _textureCache;
    VideoFrame _current;
    bool _started;
    bool _paused;
    double _mediaOrigin;
    Clock::time_point _hostOrigin;
    Clock::time_point _launchTime;
    Clock::time_point _lastReport;
    double _runSeconds;
    uint64_t _presented;
    uint64_t _discarded;
    uint64_t _repeated;
    uint64_t _lastDecoded;
    uint64_t _lastPresented;
    uint64_t _seenGeneration;
    std::ofstream _metrics;
}

- (instancetype)initWithView:(MTKView*)view
                      decoder:(VideoDecoder*)decoder
                  metricsPath:(const std::string&)metricsPath
                   runSeconds:(double)runSeconds {
    self = [super init];
    if (!self) return nil;
    _view = view;
    _decoder = decoder;
    _runSeconds = runSeconds;
    _commands = [_view.device newCommandQueue];
    _textureCache = nullptr;
    CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, _view.device,
                              nullptr, &_textureCache);
    NSError* error = nil;
    NSString* shaderPath = [NSString stringWithUTF8String:CRIMSON_PROBE_SHADER_PATH];
    NSString* source = [NSString stringWithContentsOfFile:shaderPath
                                                  encoding:NSUTF8StringEncoding
                                                     error:&error];
    if (!source) {
        std::fprintf(stderr, "Cannot read Metal shader: %s\n",
                     error.localizedDescription.UTF8String);
        return nil;
    }
    id<MTLLibrary> library = [_view.device newLibraryWithSource:source
                                                        options:nil error:&error];
    if (!library) {
        std::fprintf(stderr, "Metal shader compilation failed: %s\n",
                     error.localizedDescription.UTF8String);
        return nil;
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"fullscreenVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"nv12Fragment"];
    descriptor.colorAttachments[0].pixelFormat = _view.colorPixelFormat;
    _pipeline = [_view.device newRenderPipelineStateWithDescriptor:descriptor
                                                             error:&error];
    if (!_pipeline) {
        std::fprintf(stderr, "Metal pipeline creation failed: %s\n",
                     error.localizedDescription.UTF8String);
        return nil;
    }
    _metrics.open(metricsPath);
    _metrics << "wall_seconds,target_pts,presented_pts,lag_ms,decoded,presented,"
                "discarded_for_presentation,repeated,queued\n";
    _lastReport = Clock::now();
    _launchTime = _lastReport;
    return self;
}

- (void)dealloc {
    if (_textureCache) CFRelease(_textureCache);
}

- (double)targetTime {
    if (!_started) return 0.0;
    if (_paused) return _mediaOrigin;
    return _mediaOrigin + std::chrono::duration<double>(Clock::now() - _hostOrigin).count();
}

- (void)togglePause {
    if (!_started) return;
    if (!_paused) {
        _mediaOrigin = [self targetTime];
        _paused = true;
    } else {
        _hostOrigin = Clock::now();
        _paused = false;
    }
    std::printf("Playback %s at %.3f s\n", _paused ? "paused" : "resumed",
                _mediaOrigin);
}

- (void)seekBy:(double)seconds {
    const double target = std::max(0.0, std::min([self targetTime] + seconds,
                                                 _decoder->duration()));
    _decoder->requestSeek(target);
    _started = false;
    _mediaOrigin = target;
    _current = VideoFrame{};
    std::printf("Seek requested: %.3f s\n", target);
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    (void)size;
}

- (void)drawInMTKView:(MTKView*)view {
    @autoreleasepool {
        if (_seenGeneration != _decoder->generation()) {
            _seenGeneration = _decoder->generation();
            _started = false;
            _current = VideoFrame{};
        }
        if (!_started) {
            if (!_decoder->takeFirstFrame(_current)) return;
            _mediaOrigin = _current.pts_seconds;
            _hostOrigin = Clock::now();
            _started = true;
            const double startupLatency =
                std::chrono::duration<double>(_hostOrigin - _launchTime).count();
            std::printf("First frame ready at PTS %.3f s after %.3f s\n",
                        _mediaOrigin, startupLatency);
        }
        const double target = [self targetTime];
        VideoFrame selected;
        uint64_t discardedNow = 0;
        if (_decoder->takeFrameForTime(target, selected, discardedNow)) {
            _current = std::move(selected);
            _discarded += discardedNow;
        } else {
            ++_repeated;
        }
        if (!_current.pixel_buffer) return;

        const size_t planes = CVPixelBufferGetPlaneCount(_current.pixel_buffer);
        if (planes != 2) {
            std::fprintf(stderr, "Expected NV12 pixel buffer, got %zu planes, format 0x%08x\n",
                         planes, CVPixelBufferGetPixelFormatType(_current.pixel_buffer));
            [NSApp terminate:nil];
            return;
        }
        CVMetalTextureRef yRef = nullptr, uvRef = nullptr;
        const size_t yw = CVPixelBufferGetWidthOfPlane(_current.pixel_buffer, 0);
        const size_t yh = CVPixelBufferGetHeightOfPlane(_current.pixel_buffer, 0);
        const size_t uvw = CVPixelBufferGetWidthOfPlane(_current.pixel_buffer, 1);
        const size_t uvh = CVPixelBufferGetHeightOfPlane(_current.pixel_buffer, 1);
        CVReturn yStatus = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, _textureCache, _current.pixel_buffer, nullptr,
            MTLPixelFormatR8Unorm, yw, yh, 0, &yRef);
        CVReturn uvStatus = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, _textureCache, _current.pixel_buffer, nullptr,
            MTLPixelFormatRG8Unorm, uvw, uvh, 1, &uvRef);
        if (yStatus != kCVReturnSuccess || uvStatus != kCVReturnSuccess) {
            std::fprintf(stderr, "CVMetalTexture creation failed: %d/%d\n",
                         yStatus, uvStatus);
            if (yRef) CFRelease(yRef);
            if (uvRef) CFRelease(uvRef);
            return;
        }
        id<CAMetalDrawable> drawable = view.currentDrawable;
        MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
        if (drawable && pass) {
            id<MTLCommandBuffer> command = [_commands commandBuffer];
            id<MTLRenderCommandEncoder> encoder =
                [command renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:_pipeline];
            [encoder setFragmentTexture:CVMetalTextureGetTexture(yRef) atIndex:0];
            [encoder setFragmentTexture:CVMetalTextureGetTexture(uvRef) atIndex:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            [command presentDrawable:drawable];
            [command commit];
            ++_presented;
        }
        CFRelease(yRef);
        CFRelease(uvRef);

        const auto now = Clock::now();
        if (std::chrono::duration<double>(now - _lastReport).count() >= 1.0) {
            const double wall = std::chrono::duration<double>(now - _hostOrigin).count();
            const double lagMs = (target - _current.pts_seconds) * 1000.0;
            const uint64_t decoded = _decoder->decoded();
            const double decodeRate = decoded - _lastDecoded;
            const double presentRate = _presented - _lastPresented;
            std::printf("target=%8.3f frame=%8.3f lag=%7.1fms decode=%5.0ffps "
                        "present=%4.0ffps discard=%llu repeat=%llu queue=%zu\n",
                        target, _current.pts_seconds, lagMs, decodeRate,
                        presentRate, _discarded, _repeated, _decoder->queued());
            _metrics << std::fixed << std::setprecision(3) << wall << ',' << target
                     << ',' << _current.pts_seconds << ',' << lagMs << ','
                     << decoded << ',' << _presented << ',' << _discarded << ','
                     << _repeated << ',' << _decoder->queued() << '\n';
            _metrics.flush();
            _lastDecoded = decoded;
            _lastPresented = _presented;
            _lastReport = now;
            if (_runSeconds > 0.0 && wall >= _runSeconds) [NSApp terminate:nil];
        }
    }
}
@end

@interface ProbeWindow : NSWindow
@property(nonatomic, weak) ProbeRenderer* probeRenderer;
@end

@implementation ProbeWindow
- (void)keyDown:(NSEvent*)event {
    NSString* key = event.charactersIgnoringModifiers;
    if ([key isEqualToString:@" "]) [self.probeRenderer togglePause];
    else if (event.keyCode == 123) [self.probeRenderer seekBy:-10.0];
    else if (event.keyCode == 124) [self.probeRenderer seekBy:10.0];
    else if ([key isEqualToString:@"q"] || [key isEqualToString:@"Q"])
        [NSApp terminate:nil];
    else [super keyDown:event];
}
@end

@interface ProbeAppDelegate : NSObject <NSApplicationDelegate>
- (instancetype)initWithOptions:(const ProbeOptions&)options;
@end

@implementation ProbeAppDelegate {
    ProbeOptions _options;
    std::unique_ptr<VideoDecoder> _decoder;
    ProbeWindow* _window;
    ProbeRenderer* _renderer;
}
- (instancetype)initWithOptions:(const ProbeOptions&)options {
    self = [super init];
    if (self) _options = options;
    return self;
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    _decoder = std::make_unique<VideoDecoder>(
        _options.video_path, _options.buffer_frames, _options.realtime);
    if (!_decoder->open()) {
        [NSApp terminate:nil];
        return;
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        std::fprintf(stderr, "No Metal device available\n");
        [NSApp terminate:nil];
        return;
    }
    NSRect frame = NSMakeRect(0, 0, 900, 900);
    _window = [[ProbeWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"Crimson VideoToolbox → Metal Probe";
    MTKView* view = [[MTKView alloc] initWithFrame:frame device:device];
    view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    view.preferredFramesPerSecond = 60;
    view.enableSetNeedsDisplay = NO;
    view.paused = NO;
    _renderer = [[ProbeRenderer alloc] initWithView:view decoder:_decoder.get()
                                        metricsPath:_options.metrics_path
                                         runSeconds:_options.run_seconds];
    if (!_renderer) {
        [NSApp terminate:nil];
        return;
    }
    view.delegate = _renderer;
    _window.probeRenderer = _renderer;
    _window.contentView = view;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    std::printf("Controls: Space pause/resume, Left/Right seek 10 s, Q quit\n");
    std::printf("Metrics: %s\n", _options.metrics_path.c_str());
    _decoder->start();
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}
- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    if (_decoder) _decoder->stop();
}
@end

static void usage(const char* program) {
    std::fprintf(stderr,
        "Usage: %s VIDEO [--mode complete|realtime] [--buffer-frames N] "
        "[--duration SECONDS] [--metrics FILE]\n",
        program);
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) { usage(argv[0]); return 2; }
        ProbeOptions options;
        options.video_path = argv[1];
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--buffer-frames" && i + 1 < argc)
                options.buffer_frames = std::max<size_t>(2, std::strtoul(argv[++i], nullptr, 10));
            else if (arg == "--mode" && i + 1 < argc) {
                const std::string mode = argv[++i];
                if (mode == "complete") options.realtime = false;
                else if (mode == "realtime") options.realtime = true;
                else { usage(argv[0]); return 2; }
            }
            else if (arg == "--duration" && i + 1 < argc)
                options.run_seconds = std::strtod(argv[++i], nullptr);
            else if (arg == "--metrics" && i + 1 < argc)
                options.metrics_path = argv[++i];
            else { usage(argv[0]); return 2; }
        }
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        ProbeAppDelegate* delegate = [[ProbeAppDelegate alloc] initWithOptions:options];
        app.delegate = delegate;
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
