#include "frame_slot.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TestFailure {
    std::string message;
};

#define CHECK(condition)                                                        \
    do {                                                                       \
        if (!(condition)) {                                                     \
            throw TestFailure{std::string("CHECK failed: ") + #condition +     \
                              " at " + __FILE__ + ":" +                       \
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
    slot.format = PictureBufferFormat::RGBA32;
    slot.frame_slot_state = nullptr;
    frameSlotInitialize(slot);
    return slot;
}

FrameSlotMetadata makeMetadata(int frame_number, size_t frame_bytes) {
    FrameSlotMetadata metadata;
    metadata.frame_number = frame_number;
    metadata.local_frame_number = frame_number + 1000;
    metadata.frame_pts = static_cast<int64_t>(frame_number) * 10;
    metadata.frame_source_code = 2;
    metadata.pitch_bytes = 4;
    metadata.frame_bytes = frame_bytes;
    metadata.color_matrix = ColorSpaceStandard_BT709;
    metadata.color_range = ColorRange_JPEG;
    metadata.format = PictureBufferFormat::RGBA32;
    return metadata;
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

    auto read = frameSlotAcquireReadable(slot);
    CHECK(read.has_value());
    CHECK(read->metadata().frame_number == 7);
    CHECK(read->metadata().color_range == ColorRange_JPEG);
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
        for (int frame = 0; frame < kIterations &&
                            !failed.load(std::memory_order_acquire);
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
            const unsigned char value =
                static_cast<unsigned char>(frame % 251);
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
                fail("reader observed pixel payload that did not match metadata");
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
        if (distance < best_distance ||
            (distance == best_distance &&
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
    metadata.pitch_bytes = slot.pitch_bytes;
    metadata.frame_bytes = slot.frame_bytes;
    metadata.color_matrix = ColorSpaceStandard_BT709;
    metadata.color_range = ColorRange_Unspecified;
    metadata.format = PictureBufferFormat::RGBA32;
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
