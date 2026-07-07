#ifndef CRIMSON_FRAME_SLOT_H
#define CRIMSON_FRAME_SLOT_H

#include "decoder.h"
#include <cstddef>
#include <cstdint>
#include <optional>

struct FrameSlotMetadata {
    int frame_number = -1;
    int local_frame_number = -1;
    int64_t frame_pts = -1;
    int frame_source_code = 0;
    int pitch_bytes = 0;
    size_t frame_bytes = 0;
    int color_matrix = ColorSpaceStandard_BT709;
    int color_range = ColorRange_Unspecified;
    PictureBufferFormat format = PictureBufferFormat::RGBA32;
};

void frameSlotInitialize(PictureBuffer& slot);
void frameSlotDestroy(PictureBuffer& slot);
void frameSlotResetForWrite(PictureBuffer& slot);
void frameSlotReleaseForReuse(PictureBuffer& slot);
bool frameSlotTryReleaseForReuse(PictureBuffer& slot,
                                  int expected_frame_number);
bool frameSlotIsWritable(const PictureBuffer& slot);
std::optional<FrameSlotMetadata> frameSlotSnapshotReadable(
    const PictureBuffer& slot);

class FrameSlotWriteLease {
public:
    FrameSlotWriteLease() = default;
    FrameSlotWriteLease(FrameSlotWriteLease&& other) noexcept;
    FrameSlotWriteLease& operator=(FrameSlotWriteLease&& other) noexcept;
    FrameSlotWriteLease(const FrameSlotWriteLease&) = delete;
    FrameSlotWriteLease& operator=(const FrameSlotWriteLease&) = delete;
    ~FrameSlotWriteLease();

    explicit operator bool() const { return slot_ != nullptr; }
    PictureBuffer& slot() const { return *slot_; }
    unsigned char* frame() const { return slot_ != nullptr ? slot_->frame : nullptr; }

    void publish(const FrameSlotMetadata& metadata);
    void cancel();

private:
    friend std::optional<FrameSlotWriteLease> frameSlotAcquireWritable(
        PictureBuffer& slot);

    explicit FrameSlotWriteLease(PictureBuffer& slot);
    void moveFrom(FrameSlotWriteLease&& other) noexcept;

    PictureBuffer* slot_ = nullptr;
    bool active_ = false;
};

class FrameSlotReadLease {
public:
    FrameSlotReadLease() = default;
    FrameSlotReadLease(FrameSlotReadLease&& other) noexcept;
    FrameSlotReadLease& operator=(FrameSlotReadLease&& other) noexcept;
    FrameSlotReadLease(const FrameSlotReadLease&) = delete;
    FrameSlotReadLease& operator=(const FrameSlotReadLease&) = delete;
    ~FrameSlotReadLease();

    explicit operator bool() const { return slot_ != nullptr; }
    const PictureBuffer& slot() const { return *slot_; }
    unsigned char* frame() const { return slot_ != nullptr ? slot_->frame : nullptr; }
    const FrameSlotMetadata& metadata() const { return metadata_; }

    void release();

private:
    friend std::optional<FrameSlotReadLease> frameSlotAcquireReadable(
        PictureBuffer& slot);

    FrameSlotReadLease(PictureBuffer& slot, const FrameSlotMetadata& metadata);
    void moveFrom(FrameSlotReadLease&& other) noexcept;

    PictureBuffer* slot_ = nullptr;
    FrameSlotMetadata metadata_;
    bool active_ = false;
};

std::optional<FrameSlotWriteLease> frameSlotAcquireWritable(PictureBuffer& slot);
std::optional<FrameSlotReadLease> frameSlotAcquireReadable(PictureBuffer& slot);

#endif
