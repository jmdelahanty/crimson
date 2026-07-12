#include "frame_selection.h"
#include "frame_slot.h"
#include "playback_clock.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TestFailure {
    std::string message;
};

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            throw TestFailure{std::string("CHECK failed: ") + #condition +     \
                              " at " + __FILE__ + ":" +                        \
                              std::to_string(__LINE__)};                       \
        }                                                                      \
    } while (false)

PictureBuffer makeSlot(std::vector<unsigned char>& storage) {
    PictureBuffer slot{};
    slot.frame = storage.data();
    slot.frame_number = -1;
    slot.local_frame_number = -1;
    slot.frame_pts = -1;
    slot.frame_source_code = 0;
    slot.available_to_write = true;
    slot.pitch_bytes = 4;
    slot.frame_bytes = storage.size();
    slot.color_matrix = ColorSpaceStandard_BT709;
    slot.color_range = ColorRange_Unspecified;
    slot.format = FramePixelFormat::RGBA8;
    slot.frame_slot_state = nullptr;
    frameSlotInitialize(slot);
    return slot;
}

FrameSlotMetadata makeMetadata(int frame_number, size_t frame_bytes) {
    FrameSlotMetadata metadata;
    metadata.stream_id = "camera-main";
    metadata.frame_number = frame_number;
    metadata.local_frame_number = frame_number + 1000;
    metadata.frame_pts = static_cast<int64_t>(frame_number) * 10;
    metadata.time_base = {1, 1000};
    metadata.frame_source_code = 2;
    metadata.width = 1;
    metadata.height = static_cast<int>(frame_bytes / 4);
    metadata.pitch_bytes = 4;
    metadata.frame_bytes = frame_bytes;
    metadata.color_matrix = ColorSpaceStandard_BT709;
    metadata.color_range = ColorRange_JPEG;
    metadata.pixel_format = FramePixelFormat::RGBA8;
    metadata.surface_backend = FrameSurfaceBackend::Cpu;
    metadata.ownership = FrameSurfaceOwnership::SlotOwned;
    metadata.lifetime = FrameSurfaceLifetime::UntilReadLeaseReleased;
    return metadata;
}

class MockFrameSurface final : public FrameSurface {
  public:
    MockFrameSurface(const FrameSurfaceDescriptor& descriptor, uintptr_t handle,
                     int* destruction_count)
        : descriptor_(descriptor), handle_(handle),
          destruction_count_(destruction_count) {}

    ~MockFrameSurface() override {
        if (destruction_count_ != nullptr) {
            ++(*destruction_count_);
        }
    }

    const FrameSurfaceDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    uintptr_t nativeHandle(size_t plane_index) const noexcept override {
        if (plane_index >= descriptor_.plane_count) {
            return 0;
        }
        return handle_ + descriptor_.planes[plane_index].offset_bytes;
    }

  private:
    FrameSurfaceDescriptor descriptor_;
    uintptr_t handle_ = 0;
    int* destruction_count_ = nullptr;
};

class MockPresentationTexture final : public PresentationTexture {
  public:
    MockPresentationTexture(PresentationTextureDescriptor descriptor,
                            uintptr_t handle)
        : descriptor_(descriptor), handle_(handle) {}

    const PresentationTextureDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    uintptr_t nativeHandle() const noexcept override { return handle_; }

  private:
    PresentationTextureDescriptor descriptor_;
    uintptr_t handle_ = 0;
};

BufferedFrameCandidate candidate(int slot, int frame) {
    BufferedFrameCandidate value;
    value.slot_index = slot;
    value.metadata = makeMetadata(frame, 16);
    return value;
}

void testPublishReadRelease() {
    std::vector<unsigned char> storage(16, 0);
    PictureBuffer slot = makeSlot(storage);

    CHECK(frameSlotIsWritable(slot));
    CHECK(!frameSlotSnapshotReadable(slot).has_value());

    auto write = frameSlotAcquireWritable(slot);
    CHECK(write.has_value());
    CHECK(!frameSlotAcquireWritable(slot).has_value());

    storage[0] = 42;
    write->publish(makeMetadata(7, storage.size()));
    CHECK(!frameSlotIsWritable(slot));

    auto snapshot = frameSlotSnapshotReadable(slot);
    CHECK(snapshot.has_value());
    CHECK(snapshot->frame_number == 7);
    CHECK(snapshot->local_frame_number == 1007);
    CHECK(snapshot->frame_pts == 70);
    CHECK(snapshot->color_range == ColorRange_JPEG);
    CHECK(snapshot->stream_id == "camera-main");
    CHECK(snapshot->time_base.numerator == 1);
    CHECK(snapshot->time_base.denominator == 1000);
    CHECK(snapshot->pixel_format == FramePixelFormat::RGBA8);
    CHECK(snapshot->plane_count == 1);
    CHECK(frameMetadataHasValidLayout(*snapshot));

    auto read = frameSlotAcquireReadable(slot);
    CHECK(read.has_value());
    CHECK(read->metadata().frame_number == 7);
    CHECK(read->metadata().color_range == ColorRange_JPEG);
    CHECK(read->surface().descriptor().backend == FrameSurfaceBackend::Cpu);
    CHECK(read->surface().nativeHandle() ==
          reinterpret_cast<uintptr_t>(storage.data()));
    CHECK(read->slot().frame[0] == 42);
    CHECK(!frameSlotAcquireWritable(slot).has_value());

    read->release();
    CHECK(!frameSlotAcquireWritable(slot).has_value());

    frameSlotReleaseForReuse(slot);
    CHECK(frameSlotIsWritable(slot));
    CHECK(!frameSlotSnapshotReadable(slot).has_value());

    frameSlotDestroy(slot);
}

void testCancelReturnsWritable() {
    std::vector<unsigned char> storage(8, 0);
    PictureBuffer slot = makeSlot(storage);

    auto write = frameSlotAcquireWritable(slot);
    CHECK(write.has_value());
    storage[0] = 99;
    write->cancel();

    CHECK(frameSlotIsWritable(slot));
    CHECK(!frameSlotSnapshotReadable(slot).has_value());

    auto write_again = frameSlotAcquireWritable(slot);
    CHECK(write_again.has_value());
    write_again->publish(makeMetadata(3, storage.size()));
    CHECK(frameSlotSnapshotReadable(slot)->frame_number == 3);

    frameSlotDestroy(slot);
}

void testMultipleReadersBlockWriterUntilAllReleased() {
    std::vector<unsigned char> storage(8, 0);
    PictureBuffer slot = makeSlot(storage);

    auto write = frameSlotAcquireWritable(slot);
    CHECK(write.has_value());
    write->publish(makeMetadata(11, storage.size()));

    auto read_a = frameSlotAcquireReadable(slot);
    auto read_b = frameSlotAcquireReadable(slot);
    CHECK(read_a.has_value());
    CHECK(read_b.has_value());

    frameSlotReleaseForReuse(slot);
    CHECK(!frameSlotAcquireWritable(slot).has_value());

    read_a->release();
    CHECK(!frameSlotAcquireWritable(slot).has_value());

    read_b->release();
    auto write_after_readers = frameSlotAcquireWritable(slot);
    CHECK(write_after_readers.has_value());
    write_after_readers->cancel();

    frameSlotDestroy(slot);
}

void testConcurrentPublishReadReuseStress() {
    constexpr int kIterations = 5000;
    std::vector<unsigned char> storage(8, 0);
    PictureBuffer slot = makeSlot(storage);

    std::atomic<bool> failed{false};
    std::atomic<int> consumed{0};
    std::string error;

    auto fail = [&](const std::string& message) {
        failed.store(true, std::memory_order_release);
        error = message;
    };

    std::thread writer([&]() {
        for (int frame = 0;
             frame < kIterations && !failed.load(std::memory_order_acquire);
             ++frame) {
            std::optional<FrameSlotWriteLease> write;
            while (!failed.load(std::memory_order_acquire)) {
                write = frameSlotAcquireWritable(slot);
                if (write.has_value()) {
                    break;
                }
                std::this_thread::yield();
            }
            if (!write.has_value()) {
                return;
            }
            const unsigned char value = static_cast<unsigned char>(frame % 251);
            std::memset(write->frame(), value, storage.size());
            write->publish(makeMetadata(frame, storage.size()));
        }
    });

    std::thread reader([&]() {
        int last_frame = -1;
        while (last_frame + 1 < kIterations &&
               !failed.load(std::memory_order_acquire)) {
            auto read = frameSlotAcquireReadable(slot);
            if (!read.has_value()) {
                std::this_thread::yield();
                continue;
            }

            const int frame = read->metadata().frame_number;
            const unsigned char expected =
                static_cast<unsigned char>(frame % 251);
            if (frame <= last_frame) {
                fail("reader observed non-increasing frame number");
            } else if (read->slot().frame[0] != expected ||
                       read->slot().frame[storage.size() - 1] != expected) {
                fail("reader observed pixel payload that did not match "
                     "metadata");
            }
            last_frame = frame;
            consumed.store(frame + 1, std::memory_order_release);
            read->release();
            frameSlotReleaseForReuse(slot);
        }
    });

    writer.join();
    reader.join();

    CHECK(!failed.load(std::memory_order_acquire));
    CHECK(consumed.load(std::memory_order_acquire) == kIterations);

    frameSlotDestroy(slot);
}

void testExplicitPixelFormatsAndPlaneLayouts() {
    FrameSlotMetadata nv12;
    nv12.stream_id = "camera-nv12";
    nv12.frame_number = 21;
    nv12.local_frame_number = 7;
    nv12.frame_pts = 4200;
    nv12.time_base = {1, 100000};
    nv12.width = 6;
    nv12.height = 4;
    nv12.pitch_bytes = 8;
    nv12.frame_bytes = 48;
    nv12.pixel_format = FramePixelFormat::NV12;
    nv12.color_matrix = ColorSpaceStandard_BT709;
    nv12.color_range = ColorRange_MPEG;
    nv12.surface_backend = FrameSurfaceBackend::NvidiaCuda;
    nv12.ownership = FrameSurfaceOwnership::SlotOwned;
    nv12.lifetime = FrameSurfaceLifetime::UntilReadLeaseReleased;
    nv12.planes =
        describeFramePlanes(nv12.pixel_format, nv12.width, nv12.height,
                            nv12.pitch_bytes, &nv12.plane_count);

    CHECK(nv12.plane_count == 2);
    CHECK(nv12.planes[0].offset_bytes == 0);
    CHECK(nv12.planes[0].row_stride_bytes == 8);
    CHECK(nv12.planes[1].offset_bytes == 32);
    CHECK(nv12.planes[1].width_pixels == 3);
    CHECK(nv12.planes[1].height_pixels == 2);
    CHECK(nv12.planes[1].bytes_per_element == 2);
    CHECK(frameMetadataHasValidLayout(nv12));
    FrameSlotMetadata truncated_nv12 = nv12;
    truncated_nv12.frame_bytes = 47;
    CHECK(!frameMetadataHasValidLayout(truncated_nv12));

    FrameSlotMetadata rgba = makeMetadata(1, 16);
    CHECK(rgba.pixel_format == FramePixelFormat::RGBA8);
    rgba.planes =
        describeFramePlanes(rgba.pixel_format, rgba.width, rgba.height,
                            rgba.pitch_bytes, &rgba.plane_count);
    CHECK(frameMetadataHasValidLayout(rgba));

    FrameSlotMetadata bgra = rgba;
    bgra.pixel_format = FramePixelFormat::BGRA8;
    bgra.surface_backend = FrameSurfaceBackend::AppleVideoToolbox;
    bgra.planes =
        describeFramePlanes(bgra.pixel_format, bgra.width, bgra.height,
                            bgra.pitch_bytes, &bgra.plane_count);
    CHECK(frameMetadataHasValidLayout(bgra));

    MockPresentationTexture metal_texture(
        {PresentationBackend::Metal, FramePixelFormat::BGRA8, 1920, 1080},
        0xBEEF);
    CHECK(metal_texture.descriptor().backend == PresentationBackend::Metal);
    CHECK(metal_texture.descriptor().pixel_format == FramePixelFormat::BGRA8);
    CHECK(metal_texture.nativeHandle() == 0xBEEF);

    MockPresentationTexture gl_texture(
        {PresentationBackend::OpenGL, FramePixelFormat::RGBA8, 1920, 1080}, 17);
    CHECK(gl_texture.descriptor().backend == PresentationBackend::OpenGL);
    CHECK(gl_texture.descriptor().pixel_format == FramePixelFormat::RGBA8);
}

void testBackendSurfaceLifetimeFollowsReadLease() {
    std::vector<unsigned char> storage(48, 0);
    PictureBuffer slot = makeSlot(storage);
    slot.pitch_bytes = 8;
    slot.frame_bytes = storage.size();
    slot.format = FramePixelFormat::NV12;

    FrameSlotMetadata metadata;
    metadata.stream_id = "camera-lifetime";
    metadata.frame_number = 33;
    metadata.local_frame_number = 9;
    metadata.frame_pts = 1234;
    metadata.time_base = {1, 90000};
    metadata.width = 6;
    metadata.height = 4;
    metadata.pitch_bytes = 8;
    metadata.frame_bytes = storage.size();
    metadata.pixel_format = FramePixelFormat::NV12;
    metadata.surface_backend = FrameSurfaceBackend::AppleVideoToolbox;
    metadata.ownership = FrameSurfaceOwnership::ReferenceCounted;
    metadata.lifetime = FrameSurfaceLifetime::ReferenceCounted;
    metadata.planes = describeFramePlanes(metadata.pixel_format, metadata.width,
                                          metadata.height, metadata.pitch_bytes,
                                          &metadata.plane_count);

    int destruction_count = 0;
    auto surface = std::make_shared<MockFrameSurface>(
        frameSurfaceDescriptorFromMetadata(metadata), 0x1000,
        &destruction_count);
    std::weak_ptr<FrameSurface> weak_surface = surface;

    auto write = frameSlotAcquireWritable(slot);
    CHECK(write.has_value());
    write->publish(metadata, surface);
    surface.reset();

    auto read = frameSlotAcquireReadable(slot);
    CHECK(read.has_value());
    CHECK(read->surface().descriptor().backend ==
          FrameSurfaceBackend::AppleVideoToolbox);
    CHECK(read->surface().nativeHandle(0) == 0x1000);
    CHECK(read->surface().nativeHandle(1) == 0x1020);

    frameSlotReleaseForReuse(slot);
    CHECK(!weak_surface.expired());
    CHECK(destruction_count == 0);
    CHECK(!frameSlotAcquireWritable(slot).has_value());

    read->release();
    CHECK(weak_surface.expired());
    CHECK(destruction_count == 1);
    CHECK(frameSlotAcquireWritable(slot).has_value());

    frameSlotDestroy(slot);
}

void testSharedFrameSelectionPolicies() {
    const std::vector<BufferedFrameCandidate> buffered = {
        candidate(0, 10), candidate(1, 14), candidate(2, 18)};

    FrameSelectionRequest exact;
    exact.target_frame = 14;
    exact.preferred_slot = 1;
    const FrameSelectionResult exact_result =
        selectBufferedFrame(buffered, exact);
    CHECK(exact_result.slot_index == 1);
    CHECK(exact_result.frame_number == 14);
    CHECK(exact_result.reason == FrameSelectionReason::Exact);

    FrameSelectionRequest nearest_future;
    nearest_future.target_frame = 7;
    nearest_future.fallback =
        FrameSelectionFallback::LatestAtOrBeforeThenNearest;
    const FrameSelectionResult nearest_result =
        selectBufferedFrame(buffered, nearest_future);
    CHECK(nearest_result.slot_index == 0);
    CHECK(nearest_result.frame_number == 10);
    CHECK(nearest_result.reason == FrameSelectionReason::Nearest);

    FrameSelectionRequest buffered_before;
    buffered_before.target_frame = 16;
    buffered_before.fallback =
        FrameSelectionFallback::LatestAtOrBeforeThenNearest;
    const FrameSelectionResult before_result =
        selectBufferedFrame(buffered, buffered_before);
    CHECK(before_result.slot_index == 1);
    CHECK(before_result.frame_number == 14);
    CHECK(before_result.reason == FrameSelectionReason::BufferedBefore);

    FrameSelectionRequest skipped_present;
    skipped_present.target_frame = 17;
    skipped_present.minimum_frame_exclusive = 14;
    skipped_present.retained_slot = 1;
    skipped_present.retained_frame = 14;
    skipped_present.fallback = FrameSelectionFallback::LatestAtOrBefore;
    const FrameSelectionResult skipped_result =
        selectBufferedFrame(buffered, skipped_present);
    CHECK(skipped_result.slot_index == 1);
    CHECK(skipped_result.frame_number == 14);
    CHECK(skipped_result.reason == FrameSelectionReason::RetainedPrevious);
    CHECK(!skipped_result.selectedBufferedFrame());

    FrameSelectionRequest paused_missing;
    paused_missing.target_frame = 15;
    paused_missing.retained_slot = 1;
    paused_missing.retained_frame = 14;
    paused_missing.fallback = FrameSelectionFallback::ExactOnly;
    const FrameSelectionResult paused_retained =
        selectBufferedFrame(buffered, paused_missing);
    CHECK(paused_retained.reason == FrameSelectionReason::RetainedPrevious);
    CHECK(paused_retained.frame_number == 14);

    FrameSelectionRequest paused_exact = paused_missing;
    paused_exact.target_frame = 18;
    paused_exact.preferred_slot = 2;
    const FrameSelectionResult paused_selected =
        selectBufferedFrame(buffered, paused_exact);
    CHECK(paused_selected.reason == FrameSelectionReason::Exact);
    CHECK(paused_selected.slot_index == 2);

    FrameSelectionRequest nearest_tie;
    nearest_tie.target_frame = 12;
    nearest_tie.fallback = FrameSelectionFallback::Nearest;
    const FrameSelectionResult tie_result =
        selectBufferedFrame(buffered, nearest_tie);
    CHECK(tie_result.frame_number == 14);
}

void testLogicalPlaybackClock() {
    using namespace std::chrono_literals;
    const LogicalPlaybackClock::TimePoint start{};
    LogicalPlaybackClock clock;
    clock.configure(100.0, 1000, start);
    CHECK(!clock.isPlaying());
    CHECK(clock.requestedFrame(start + 1s) == 0);

    clock.play(start);
    CHECK(clock.isPlaying());
    CHECK(clock.requestedFrame(start + 5ms) == 0);
    CHECK(clock.requestedFrame(start + 10ms) == 1);
    CHECK(clock.requestedFrame(start + 1234ms) == 123);

    clock.pause(start + 1234ms);
    CHECK(!clock.isPlaying());
    CHECK(clock.requestedFrame(start + 5s) == 123);

    clock.seek(700, start + 5s);
    CHECK(clock.requestedFrame(start + 5s) == 700);
    clock.play(start + 5s);
    CHECK(clock.requestedFrame(start + 5500ms) == 750);
    clock.seek(998, start + 5500ms);
    CHECK(clock.requestedFrame(start + 10s) == 999);
    clock.pause(start + 10s);
    CHECK(clock.requestedFrame(start + 20s) == 999);

    clock.seek(-50, start + 20s);
    CHECK(clock.requestedFrame(start + 20s) == 0);
}

int findNearestReadableSlot(PictureBuffer* slots, int slot_count,
                            int target_frame) {
    int best_slot = -1;
    int best_distance = std::numeric_limits<int>::max();
    int best_frame = -1;

    for (int i = 0; i < slot_count; ++i) {
        auto metadata = frameSlotSnapshotReadable(slots[i]);
        if (!metadata.has_value()) {
            continue;
        }
        const int distance = std::abs(metadata->frame_number - target_frame);
        if (distance < best_distance || (distance == best_distance &&
                                         metadata->frame_number > best_frame)) {
            best_slot = i;
            best_distance = distance;
            best_frame = metadata->frame_number;
        }
    }

    return best_slot;
}

void publishStimulusStyleFrame(PictureBuffer& slot, int frame_number,
                               unsigned char value) {
    auto write = frameSlotAcquireWritable(slot);
    CHECK(write.has_value());
    std::memset(write->frame(), value, slot.frame_bytes);

    FrameSlotMetadata metadata;
    metadata.frame_number = frame_number;
    metadata.local_frame_number = frame_number;
    metadata.frame_pts = -1;
    metadata.frame_source_code = 2;
    metadata.width = 1;
    metadata.height = static_cast<int>(slot.frame_bytes / slot.pitch_bytes);
    metadata.pitch_bytes = slot.pitch_bytes;
    metadata.frame_bytes = slot.frame_bytes;
    metadata.color_matrix = ColorSpaceStandard_BT709;
    metadata.color_range = ColorRange_Unspecified;
    metadata.pixel_format = FramePixelFormat::RGBA8;
    metadata.surface_backend = FrameSurfaceBackend::Cpu;
    metadata.ownership = FrameSurfaceOwnership::SlotOwned;
    metadata.lifetime = FrameSurfaceLifetime::UntilReadLeaseReleased;
    write->publish(metadata);
}

void testStimulusStyleRingSnapshotsAndReuse() {
    constexpr int kSlotCount = 3;
    std::vector<std::vector<unsigned char>> storage;
    storage.reserve(kSlotCount);
    for (int i = 0; i < kSlotCount; ++i) {
        storage.emplace_back(8, 0);
    }

    PictureBuffer slots[kSlotCount];
    for (int i = 0; i < kSlotCount; ++i) {
        slots[i] = makeSlot(storage[i]);
    }

    publishStimulusStyleFrame(slots[0], 10, 10);
    publishStimulusStyleFrame(slots[1], 11, 11);
    publishStimulusStyleFrame(slots[2], 12, 12);

    CHECK(findNearestReadableSlot(slots, kSlotCount, 11) == 1);
    CHECK(findNearestReadableSlot(slots, kSlotCount, 13) == 2);

    auto read = frameSlotAcquireReadable(slots[1]);
    CHECK(read.has_value());
    CHECK(read->metadata().frame_number == 11);
    CHECK(read->metadata().local_frame_number == 11);
    CHECK(read->slot().frame[0] == 11);
    CHECK(read->slot().frame[storage[1].size() - 1] == 11);

    frameSlotReleaseForReuse(slots[1]);
    CHECK(!frameSlotAcquireWritable(slots[1]).has_value());

    read->release();
    publishStimulusStyleFrame(slots[1], 13, 13);

    for (int i = 0; i < kSlotCount; ++i) {
        auto metadata = frameSlotSnapshotReadable(slots[i]);
        if (metadata.has_value() && metadata->frame_number < 12) {
            frameSlotReleaseForReuse(slots[i]);
        }
    }

    CHECK(!frameSlotSnapshotReadable(slots[0]).has_value());
    auto slot_one_metadata = frameSlotSnapshotReadable(slots[1]);
    auto slot_two_metadata = frameSlotSnapshotReadable(slots[2]);
    CHECK(slot_one_metadata.has_value());
    CHECK(slot_two_metadata.has_value());
    CHECK(slot_one_metadata->frame_number == 13);
    CHECK(slot_two_metadata->frame_number == 12);

    for (int i = 0; i < kSlotCount; ++i) {
        frameSlotDestroy(slots[i]);
    }
}

void runAllTests() {
    testPublishReadRelease();
    testCancelReturnsWritable();
    testMultipleReadersBlockWriterUntilAllReleased();
    testConcurrentPublishReadReuseStress();
    testExplicitPixelFormatsAndPlaneLayouts();
    testBackendSurfaceLifetimeFollowsReadLease();
    testSharedFrameSelectionPolicies();
    testLogicalPlaybackClock();
    testStimulusStyleRingSnapshotsAndReuse();
}

}  // namespace

int main() {
    try {
        runAllTests();
    } catch (const TestFailure& failure) {
        std::cerr << failure.message << std::endl;
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Unexpected exception: " << ex.what() << std::endl;
        return 2;
    }

    std::cout << "frame_slot_tests: PASS" << std::endl;
    return 0;
}
