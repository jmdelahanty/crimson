#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;

struct Options {
    std::string video;
    std::string metrics = "avfoundation_metal_metrics.csv";
    double duration = 0.0;
};
}

@interface AVFRenderer : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView*)view
                          url:(NSURL*)url
                  metricsPath:(const std::string&)metricsPath
                   runSeconds:(double)runSeconds;
- (void)togglePause;
- (void)seekBy:(double)seconds;
@end

@implementation AVFRenderer {
    MTKView* _view;
    AVPlayer* _player;
    AVPlayerItemVideoOutput* _output;
    id<MTLCommandQueue> _commands;
    id<MTLRenderPipelineState> _pipeline;
    CVMetalTextureCacheRef _textureCache;
    CVPixelBufferRef _currentBuffer;
    CMTime _currentDisplayTime;
    Clock::time_point _launchTime;
    Clock::time_point _firstFrameTime;
    Clock::time_point _lastReport;
    double _runSeconds;
    uint64_t _newFrames;
    uint64_t _repeated;
    uint64_t _lastNewFrames;
    uint64_t _lastRepeated;
    std::ofstream _metrics;
}

- (instancetype)initWithView:(MTKView*)view
                          url:(NSURL*)url
                  metricsPath:(const std::string&)metricsPath
                   runSeconds:(double)runSeconds {
    self = [super init];
    if (!self) return nil;
    _view = view;
    _runSeconds = runSeconds;
    _currentDisplayTime = kCMTimeInvalid;
    _commands = [_view.device newCommandQueue];
    CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, _view.device,
                              nullptr, &_textureCache);

    NSString* shaderPath = [NSString stringWithUTF8String:CRIMSON_PROBE_SHADER_PATH];
    NSError* error = nil;
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

    NSDictionary* attributes = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    _output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attributes];
    AVPlayerItem* item = [AVPlayerItem playerItemWithURL:url];
    [item addOutput:_output];
    _player = [AVPlayer playerWithPlayerItem:item];
    _player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
    _player.automaticallyWaitsToMinimizeStalling = YES;

    _metrics.open(metricsPath);
    _metrics << "wall_seconds,player_seconds,display_seconds,lag_ms,"
                "new_frames,repeated_refreshes,player_rate\n";
    _launchTime = Clock::now();
    _lastReport = _launchTime;
    [_player play];
    std::printf("Backend: AVFoundation AVPlayerItemVideoOutput -> CVPixelBuffer -> Metal\n");
    return self;
}

- (void)dealloc {
    [_player pause];
    if (_currentBuffer) CVPixelBufferRelease(_currentBuffer);
    if (_textureCache) CFRelease(_textureCache);
}

- (void)togglePause {
    if (_player.rate == 0.0f) {
        [_player play];
        std::printf("Playback resumed at %.3f s\n",
                    CMTimeGetSeconds(_player.currentTime));
    } else {
        [_player pause];
        std::printf("Playback paused at %.3f s\n",
                    CMTimeGetSeconds(_player.currentTime));
    }
}

- (void)seekBy:(double)seconds {
    const double current = CMTimeGetSeconds(_player.currentTime);
    const double target = std::max(0.0, current + seconds);
    const BOOL resume = _player.rate != 0.0f;
    [_player pause];
    const auto start = Clock::now();
    [_player seekToTime:CMTimeMakeWithSeconds(target, 600)
         toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero
       completionHandler:^(BOOL finished) {
        const double elapsed =
            std::chrono::duration<double>(Clock::now() - start).count();
        std::printf("AVFoundation exact seek: target %.3f s finished=%s latency %.3f s\n",
                    target, finished ? "true" : "false", elapsed);
        if (resume) [_player play];
    }];
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    (void)size;
}

- (void)drawInMTKView:(MTKView*)view {
    @autoreleasepool {
        const CFTimeInterval host = CACurrentMediaTime();
        const CMTime requested = [_output itemTimeForHostTime:host];
        if ([_output hasNewPixelBufferForItemTime:requested]) {
            CMTime displayTime = kCMTimeInvalid;
            CVPixelBufferRef next =
                [_output copyPixelBufferForItemTime:requested
                                 itemTimeForDisplay:&displayTime];
            if (next) {
                if (_currentBuffer) CVPixelBufferRelease(_currentBuffer);
                _currentBuffer = next;
                _currentDisplayTime = displayTime;
                ++_newFrames;
                if (_newFrames == 1) {
                    _firstFrameTime = Clock::now();
                    const double latency = std::chrono::duration<double>(
                        _firstFrameTime - _launchTime).count();
                    std::printf("First AVFoundation frame after %.3f s at PTS %.3f s\n",
                                latency, CMTimeGetSeconds(displayTime));
                }
            }
        } else if (_currentBuffer) {
            ++_repeated;
        }
        if (!_currentBuffer) return;

        const size_t planes = CVPixelBufferGetPlaneCount(_currentBuffer);
        if (planes != 2) {
            std::fprintf(stderr, "Expected NV12 output; received %zu planes, format 0x%08x\n",
                         planes, CVPixelBufferGetPixelFormatType(_currentBuffer));
            [NSApp terminate:nil];
            return;
        }
        CVMetalTextureRef yRef = nullptr, uvRef = nullptr;
        const CVReturn ys = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, _textureCache, _currentBuffer, nullptr,
            MTLPixelFormatR8Unorm, CVPixelBufferGetWidthOfPlane(_currentBuffer, 0),
            CVPixelBufferGetHeightOfPlane(_currentBuffer, 0), 0, &yRef);
        const CVReturn uvs = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, _textureCache, _currentBuffer, nullptr,
            MTLPixelFormatRG8Unorm, CVPixelBufferGetWidthOfPlane(_currentBuffer, 1),
            CVPixelBufferGetHeightOfPlane(_currentBuffer, 1), 1, &uvRef);
        if (ys != kCVReturnSuccess || uvs != kCVReturnSuccess) {
            std::fprintf(stderr, "CVMetalTexture creation failed: %d/%d\n", ys, uvs);
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
        }
        CFRelease(yRef);
        CFRelease(uvRef);

        const auto now = Clock::now();
        if (std::chrono::duration<double>(now - _lastReport).count() >= 1.0) {
            const double wall = std::chrono::duration<double>(now - _firstFrameTime).count();
            const double playerSeconds = CMTimeGetSeconds(_player.currentTime);
            const double displaySeconds = CMTIME_IS_VALID(_currentDisplayTime)
                                              ? CMTimeGetSeconds(_currentDisplayTime)
                                              : 0.0;
            const double lagMs = (playerSeconds - displaySeconds) * 1000.0;
            const uint64_t newRate = _newFrames - _lastNewFrames;
            const uint64_t repeatRate = _repeated - _lastRepeated;
            std::printf("player=%8.3f frame=%8.3f lag=%7.1fms new=%3llufps "
                        "repeat=%3llu rate=%.2f\n",
                        playerSeconds, displaySeconds, lagMs, newRate,
                        repeatRate, _player.rate);
            _metrics << std::fixed << std::setprecision(3) << wall << ','
                     << playerSeconds << ',' << displaySeconds << ',' << lagMs
                     << ',' << _newFrames << ',' << _repeated << ','
                     << _player.rate << '\n';
            _metrics.flush();
            _lastNewFrames = _newFrames;
            _lastRepeated = _repeated;
            _lastReport = now;
            if (_runSeconds > 0.0 && wall >= _runSeconds) [NSApp terminate:nil];
        }
    }
}
@end

@interface AVFWindow : NSWindow
@property(nonatomic, weak) AVFRenderer* probeRenderer;
@end
@implementation AVFWindow
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

@interface AVFAppDelegate : NSObject <NSApplicationDelegate>
- (instancetype)initWithOptions:(const Options&)options;
@end
@implementation AVFAppDelegate {
    Options _options;
    AVFWindow* _window;
    AVFRenderer* _renderer;
}
- (instancetype)initWithOptions:(const Options&)options {
    self = [super init];
    if (self) _options = options;
    return self;
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { std::fprintf(stderr, "No Metal device available\n"); [NSApp terminate:nil]; return; }
    NSRect frame = NSMakeRect(0, 0, 900, 900);
    _window = [[AVFWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"Crimson AVFoundation → Metal Probe";
    MTKView* view = [[MTKView alloc] initWithFrame:frame device:device];
    view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    view.preferredFramesPerSecond = 60;
    view.enableSetNeedsDisplay = NO;
    view.paused = NO;
    NSURL* url = [NSURL fileURLWithPath:
        [NSString stringWithUTF8String:_options.video.c_str()]];
    _renderer = [[AVFRenderer alloc] initWithView:view url:url
                                      metricsPath:_options.metrics
                                       runSeconds:_options.duration];
    if (!_renderer) { [NSApp terminate:nil]; return; }
    view.delegate = _renderer;
    _window.probeRenderer = _renderer;
    _window.contentView = view;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    std::printf("Controls: Space pause/resume, Left/Right exact seek 10 s, Q quit\n");
    std::printf("Metrics: %s\n", _options.metrics.c_str());
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}
@end

static void usage(const char* program) {
    std::fprintf(stderr, "Usage: %s VIDEO [--duration SECONDS] [--metrics FILE]\n",
                 program);
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) { usage(argv[0]); return 2; }
        Options options;
        options.video = argv[1];
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--duration" && i + 1 < argc)
                options.duration = std::strtod(argv[++i], nullptr);
            else if (arg == "--metrics" && i + 1 < argc)
                options.metrics = argv[++i];
            else { usage(argv[0]); return 2; }
        }
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AVFAppDelegate* delegate = [[AVFAppDelegate alloc] initWithOptions:options];
        app.delegate = delegate;
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
