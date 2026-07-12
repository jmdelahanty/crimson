#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct MappingRow {
    int64_t stimulus = -1;
    bool interpolated = false;
    bool valid = false;
};

struct Options {
    std::string main_video, crop_video, stimulus_video, alignment;
    std::string metrics = "aligned_playback_metrics.csv";
    double duration = 0.0;
};

std::vector<MappingRow> loadMapping(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open alignment CSV: " + path);
    std::string line;
    std::getline(input, line);
    std::vector<MappingRow> rows;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::stringstream parser(line);
        std::string field;
        int64_t camera = -1;
        MappingRow row;
        std::getline(parser, field, ','); camera = std::stoll(field);
        std::getline(parser, field, ','); row.stimulus = std::stoll(field);
        std::getline(parser, field, ','); row.interpolated = std::stoi(field) != 0;
        std::getline(parser, field, ','); row.valid = std::stoi(field) != 0;
        if (camera != static_cast<int64_t>(rows.size()))
            throw std::runtime_error("alignment CSV is not dense");
        rows.push_back(row);
    }
    return rows;
}

NSDictionary* outputAttributes() {
    return @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
}

struct Stream {
    std::string name;
    AVPlayer* player = nil;
    AVPlayerItemVideoOutput* output = nil;
    CVPixelBufferRef pixel = nullptr;
    double presented_pts = NAN;
    double fps = 0.0;
    uint64_t new_frames = 0;

    ~Stream() { if (pixel) CVPixelBufferRelease(pixel); }
};

bool fileExists(const std::string& path) {
    return [[NSFileManager defaultManager]
        isReadableFileAtPath:[NSString stringWithUTF8String:path.c_str()]];
}

bool configureStream(Stream& stream, const std::string& path) {
    if (!fileExists(path)) {
        std::fprintf(stderr, "%s video is missing/unreadable: %s\n",
                     stream.name.c_str(), path.c_str());
        return false;
    }
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    __block NSArray<AVAssetTrack*>* tracks = nil;
    __block NSError* loadError = nil;
    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:AVMediaTypeVideo
                completionHandler:^(NSArray<AVAssetTrack*>* loaded, NSError* error) {
        tracks = loaded; loadError = error; dispatch_semaphore_signal(ready);
    }];
    if (dispatch_semaphore_wait(
            ready, dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC)) != 0 ||
        loadError || tracks.count == 0) {
        std::fprintf(stderr, "%s track load failed: %s\n", stream.name.c_str(),
                     loadError.localizedDescription.UTF8String ?: "timeout/no track");
        return false;
    }
    AVPlayerItem* item = [AVPlayerItem playerItemWithAsset:asset];
    stream.output = [[AVPlayerItemVideoOutput alloc]
        initWithPixelBufferAttributes:outputAttributes()];
    [item addOutput:stream.output];
    stream.player = [AVPlayer playerWithPlayerItem:item];
    stream.player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
    stream.player.automaticallyWaitsToMinimizeStalling = YES;
    stream.fps = tracks.firstObject.nominalFrameRate;
    return stream.fps > 0.0;
}
}

@interface AlignedRenderer : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView*)view options:(const Options&)options;
- (void)togglePause;
- (void)seekSeconds:(double)seconds;
- (void)stepFrames:(int)frames;
- (void)seekToFrame:(int64_t)frame resume:(BOOL)resume;
@end

@implementation AlignedRenderer {
    MTKView* _view;
    id<MTLCommandQueue> _commands;
    id<MTLRenderPipelineState> _pipeline;
    CVMetalTextureCacheRef _textureCache;
    Stream _main, _crop, _stimulus;
    std::vector<MappingRow> _mapping;
    bool _paused;
    int64_t _logicalFrame;
    Clock::time_point _started, _lastReport;
    double _runSeconds;
    std::ofstream _metrics;
}

- (instancetype)initWithView:(MTKView*)view options:(const Options&)options {
    self = [super init];
    if (!self) return nil;
    _view = view;
    _main.name = "main"; _crop.name = "crop"; _stimulus.name = "stimulus";
    try { _mapping = loadMapping(options.alignment); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Alignment error: %s\n", error.what()); return nil;
    }
    if (!configureStream(_main, options.main_video) ||
        !configureStream(_crop, options.crop_video) ||
        !configureStream(_stimulus, options.stimulus_video)) return nil;

    _commands = [_view.device newCommandQueue];
    CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, _view.device,
                              nullptr, &_textureCache);
    NSError* error = nil;
    NSString* shaderPath = [NSString stringWithUTF8String:CRIMSON_PROBE_SHADER_PATH];
    NSString* source = [NSString stringWithContentsOfFile:shaderPath
                                                  encoding:NSUTF8StringEncoding
                                                     error:&error];
    id<MTLLibrary> library = source
        ? [_view.device newLibraryWithSource:source options:nil error:&error] : nil;
    if (!library) {
        std::fprintf(stderr, "Metal shader error: %s\n",
                     error.localizedDescription.UTF8String); return nil;
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"fullscreenVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"nv12Fragment"];
    descriptor.colorAttachments[0].pixelFormat = _view.colorPixelFormat;
    _pipeline = [_view.device newRenderPipelineStateWithDescriptor:descriptor
                                                             error:&error];
    if (!_pipeline) return nil;

    auto first = std::find_if(_mapping.begin(), _mapping.end(),
                              [](const MappingRow& row) { return row.valid; });
    if (first == _mapping.end()) return nil;
    _logicalFrame = std::distance(_mapping.begin(), first);
    _runSeconds = options.duration;
    _metrics.open(options.metrics);
    _metrics << "wall_s,camera_frame,mapped_stimulus_frame,interpolated,"
                "main_pts,crop_pts,stimulus_pts,main_error_frames,"
                "crop_error_frames,stimulus_error_frames,crop_clock_ms,"
                "stimulus_clock_ms\n";
    [self seekToFrame:_logicalFrame resume:YES];
    _started = _lastReport = Clock::now();
    std::printf("Aligned playback: main %.6f fps, crop %.6f fps, stimulus %.6f fps, "
                "mapping rows %zu, start camera frame %lld\n",
                _main.fps, _crop.fps, _stimulus.fps, _mapping.size(),
                static_cast<long long>(_logicalFrame));
    return self;
}

- (void)dealloc { if (_textureCache) CFRelease(_textureCache); }

- (void)seekToFrame:(int64_t)frame resume:(BOOL)resume {
    frame = std::clamp<int64_t>(frame, 0, static_cast<int64_t>(_mapping.size()) - 1);
    while (frame < static_cast<int64_t>(_mapping.size()) && !_mapping[frame].valid) ++frame;
    if (frame >= static_cast<int64_t>(_mapping.size())) return;
    _logicalFrame = frame;
    const double cameraSeconds = frame / _main.fps;
    const double cropSeconds = frame / _crop.fps;
    const double stimulusSeconds = _mapping[frame].stimulus / _stimulus.fps;
    [_main.player pause]; [_crop.player pause]; [_stimulus.player pause];
    dispatch_group_t seeks = dispatch_group_create();
    dispatch_group_enter(seeks);
    [_main.player seekToTime:CMTimeMakeWithSeconds(cameraSeconds, 60000)
              toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero
            completionHandler:^(BOOL finished) {
        (void)finished; dispatch_group_leave(seeks);
    }];
    dispatch_group_enter(seeks);
    [_crop.player seekToTime:CMTimeMakeWithSeconds(cropSeconds, 60000)
              toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero
            completionHandler:^(BOOL finished) {
        (void)finished; dispatch_group_leave(seeks);
    }];
    dispatch_group_enter(seeks);
    [_stimulus.player seekToTime:CMTimeMakeWithSeconds(stimulusSeconds, 60000)
              toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero
            completionHandler:^(BOOL finished) {
        (void)finished; dispatch_group_leave(seeks);
    }];
    dispatch_group_notify(seeks, dispatch_get_main_queue(), ^{
        if (!resume) return;
        const CMTime hostStart = CMTimeAdd(
            CMClockGetTime(CMClockGetHostTimeClock()),
            CMTimeMakeWithSeconds(0.10, 60000));
        [_main.player setRate:1.0f
                         time:CMTimeMakeWithSeconds(cameraSeconds, 60000)
                   atHostTime:hostStart];
        [_crop.player setRate:1.0f
                         time:CMTimeMakeWithSeconds(cropSeconds, 60000)
                   atHostTime:hostStart];
        [_stimulus.player setRate:1.0f
                         time:CMTimeMakeWithSeconds(stimulusSeconds, 60000)
                   atHostTime:hostStart];
    });
    std::printf("Seek camera=%lld stimulus=%lld\n",
                static_cast<long long>(frame),
                static_cast<long long>(_mapping[frame].stimulus));
}

- (void)togglePause {
    _paused = !_paused;
    if (_paused) { [_main.player pause]; [_crop.player pause]; [_stimulus.player pause]; }
    else {
        const CMTime hostStart = CMTimeAdd(
            CMClockGetTime(CMClockGetHostTimeClock()),
            CMTimeMakeWithSeconds(0.10, 60000));
        [_main.player setRate:1.0f
                         time:CMTimeMakeWithSeconds(_logicalFrame / _main.fps, 60000)
                   atHostTime:hostStart];
        [_crop.player setRate:1.0f
                         time:CMTimeMakeWithSeconds(_logicalFrame / _crop.fps, 60000)
                   atHostTime:hostStart];
        [_stimulus.player setRate:1.0f
                         time:CMTimeMakeWithSeconds(
                                  _mapping[_logicalFrame].stimulus / _stimulus.fps,
                                  60000)
                   atHostTime:hostStart];
    }
}
- (void)seekSeconds:(double)seconds {
    [self seekToFrame:_logicalFrame + llround(seconds * _main.fps)
               resume:!_paused];
}
- (void)stepFrames:(int)frames {
    if (!_paused) [self togglePause];
    [self seekToFrame:_logicalFrame + frames resume:NO];
}
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    (void)view; (void)size;
}

- (bool)updateStream:(Stream&)stream desired:(double)seconds {
    const CMTime requested = CMTimeMakeWithSeconds(seconds, 60000);
    if (![stream.output hasNewPixelBufferForItemTime:requested]) return false;
    CMTime display = kCMTimeInvalid;
    CVPixelBufferRef next = [stream.output copyPixelBufferForItemTime:requested
                                                   itemTimeForDisplay:&display];
    if (!next) return false;
    if (stream.pixel) CVPixelBufferRelease(stream.pixel);
    stream.pixel = next;
    stream.presented_pts = CMTimeGetSeconds(display);
    ++stream.new_frames;
    return true;
}

- (void)encodeStream:(Stream&)stream encoder:(id<MTLRenderCommandEncoder>)encoder
             viewport:(MTLViewport)viewport {
    if (!stream.pixel || CVPixelBufferGetPlaneCount(stream.pixel) != 2) return;
    CVMetalTextureRef yRef = nullptr, uvRef = nullptr;
    CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, _textureCache,
        stream.pixel, nullptr, MTLPixelFormatR8Unorm,
        CVPixelBufferGetWidthOfPlane(stream.pixel, 0),
        CVPixelBufferGetHeightOfPlane(stream.pixel, 0), 0, &yRef);
    CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, _textureCache,
        stream.pixel, nullptr, MTLPixelFormatRG8Unorm,
        CVPixelBufferGetWidthOfPlane(stream.pixel, 1),
        CVPixelBufferGetHeightOfPlane(stream.pixel, 1), 1, &uvRef);
    if (yRef && uvRef) {
        [encoder setViewport:viewport];
        [encoder setFragmentTexture:CVMetalTextureGetTexture(yRef) atIndex:0];
        [encoder setFragmentTexture:CVMetalTextureGetTexture(uvRef) atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    if (yRef) CFRelease(yRef); if (uvRef) CFRelease(uvRef);
}

- (void)drawInMTKView:(MTKView*)view {
    @autoreleasepool {
        if (!_paused) {
            const double mainClock = CMTimeGetSeconds(_main.player.currentTime);
            if (std::isfinite(mainClock))
                _logicalFrame = std::clamp<int64_t>(llround(mainClock * _main.fps),
                    0, static_cast<int64_t>(_mapping.size()) - 1);
        }
        if (!_mapping[_logicalFrame].valid) return;
        const int64_t stimulusFrame = _mapping[_logicalFrame].stimulus;
        const double mainTarget = _logicalFrame / _main.fps;
        const double cropTarget = _logicalFrame / _crop.fps;
        const double stimulusTarget = stimulusFrame / _stimulus.fps;
        [self updateStream:_main desired:mainTarget];
        [self updateStream:_crop desired:cropTarget];
        [self updateStream:_stimulus desired:stimulusTarget];

        id<CAMetalDrawable> drawable = view.currentDrawable;
        MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
        if (drawable && pass) {
            const double w = view.drawableSize.width, h = view.drawableSize.height;
            id<MTLCommandBuffer> command = [_commands commandBuffer];
            id<MTLRenderCommandEncoder> encoder =
                [command renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:_pipeline];
            const MTLViewport mainViewport = {0, 0, w * 0.67, h, 0, 1};
            const MTLViewport cropViewport =
                {w * 0.67, h * 0.5, w * 0.33, h * 0.5, 0, 1};
            const MTLViewport stimulusViewport =
                {w * 0.67, 0, w * 0.33, h * 0.5, 0, 1};
            [self encodeStream:_main encoder:encoder viewport:mainViewport];
            [self encodeStream:_crop encoder:encoder viewport:cropViewport];
            [self encodeStream:_stimulus encoder:encoder viewport:stimulusViewport];
            [encoder endEncoding]; [command presentDrawable:drawable]; [command commit];
        }

        const auto now = Clock::now();
        if (std::chrono::duration<double>(now - _lastReport).count() >= 1.0) {
            const double mainError = (_main.presented_pts - mainTarget) * _main.fps;
            const double cropError = (_crop.presented_pts - cropTarget) * _crop.fps;
            const double stimulusError =
                (_stimulus.presented_pts - stimulusTarget) * _stimulus.fps;
            const double cropClock =
                (CMTimeGetSeconds(_crop.player.currentTime) - cropTarget) * 1000.0;
            const double stimulusClock =
                (CMTimeGetSeconds(_stimulus.player.currentTime) - stimulusTarget) * 1000.0;
            std::printf("camera=%lld stimulus=%lld errors_frames=%.2f/%.2f/%.2f "
                        "secondary_clocks_ms=%.1f/%.1f\n",
                        static_cast<long long>(_logicalFrame),
                        static_cast<long long>(stimulusFrame), mainError, cropError,
                        stimulusError, cropClock, stimulusClock);
            const double wall = std::chrono::duration<double>(now - _started).count();
            _metrics << std::fixed << std::setprecision(6) << wall << ','
                << _logicalFrame << ',' << stimulusFrame << ','
                << (_mapping[_logicalFrame].interpolated ? 1 : 0) << ','
                << _main.presented_pts << ',' << _crop.presented_pts << ','
                << _stimulus.presented_pts << ',' << mainError << ',' << cropError
                << ',' << stimulusError << ',' << cropClock << ',' << stimulusClock << '\n';
            _metrics.flush(); _lastReport = now;
            if (_runSeconds > 0.0 && wall >= _runSeconds) [NSApp terminate:nil];
        }
    }
}
@end

@interface AlignedWindow : NSWindow
@property(nonatomic, weak) AlignedRenderer* renderer;
@end
@implementation AlignedWindow
- (void)keyDown:(NSEvent*)event {
    NSString* key = event.charactersIgnoringModifiers;
    if ([key isEqualToString:@" "]) [self.renderer togglePause];
    else if (event.keyCode == 123) [self.renderer seekSeconds:-10];
    else if (event.keyCode == 124) [self.renderer seekSeconds:10];
    else if ([key isEqualToString:@","]) [self.renderer stepFrames:-1];
    else if ([key isEqualToString:@"."]) [self.renderer stepFrames:1];
    else if ([key caseInsensitiveCompare:@"q"] == NSOrderedSame) [NSApp terminate:nil];
    else [super keyDown:event];
}
@end

@interface AlignedDelegate : NSObject <NSApplicationDelegate>
- (instancetype)initWithOptions:(const Options&)options;
@end
@implementation AlignedDelegate { Options _options; AlignedWindow* _window; AlignedRenderer* _renderer; }
- (instancetype)initWithOptions:(const Options&)options { self=[super init]; if(self)_options=options; return self; }
- (void)applicationDidFinishLaunching:(NSNotification*)note {
    (void)note; id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    NSRect frame=NSMakeRect(0,0,1200,800);
    _window=[[AlignedWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    MTKView* view=[[MTKView alloc] initWithFrame:frame device:device];
    view.colorPixelFormat=MTLPixelFormatBGRA8Unorm; view.preferredFramesPerSecond=60;
    _renderer=[[AlignedRenderer alloc] initWithView:view options:_options];
    if(!_renderer){[NSApp terminate:nil];return;} view.delegate=_renderer;
    _window.renderer=_renderer; _window.contentView=view;
    _window.title=@"Crimson Zarr-Aligned AVFoundation → Metal Probe";
    [_window center]; [_window makeKeyAndOrderFront:nil];
    std::printf("Controls: Space pause, arrows seek 10 s, comma/period step one camera frame, Q quit\n");
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { (void)sender; return YES; }
@end

static void usage(const char* p) {
    std::fprintf(stderr,"Usage: %s --main VIDEO --crop VIDEO --stimulus VIDEO --alignment CSV [--duration S] [--metrics CSV]\n",p);
}
int main(int argc,char** argv){@autoreleasepool{
    Options o; for(int i=1;i<argc;++i){std::string a=argv[i];
        if(a=="--main"&&i+1<argc)o.main_video=argv[++i];
        else if(a=="--crop"&&i+1<argc)o.crop_video=argv[++i];
        else if(a=="--stimulus"&&i+1<argc)o.stimulus_video=argv[++i];
        else if(a=="--alignment"&&i+1<argc)o.alignment=argv[++i];
        else if(a=="--duration"&&i+1<argc)o.duration=std::strtod(argv[++i],nullptr);
        else if(a=="--metrics"&&i+1<argc)o.metrics=argv[++i]; else{usage(argv[0]);return 2;}}
    if(o.main_video.empty()||o.crop_video.empty()||o.stimulus_video.empty()||o.alignment.empty()){usage(argv[0]);return 2;}
    NSApplication* app=[NSApplication sharedApplication]; [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    AlignedDelegate* delegate=[[AlignedDelegate alloc] initWithOptions:o]; app.delegate=delegate;
    [app activateIgnoringOtherApps:YES]; [app run]; return 0;
}}
