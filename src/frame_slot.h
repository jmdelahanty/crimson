#ifndef CRIMSON_FRAME_SLOT_H
#define CRIMSON_FRAME_SLOT_H

#include "frame_types.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

void frameSlotInitialize(PictureBuffer& slot);
void frameSlotDestroy(PictureBuffer& slot);
void frameSlotResetForWrite(PictureBuffer& slot);
void frameSlotReleaseForReuse(PictureBuffer& slot);
bool frameSlotTryReleaseForReuse(PictureBuffer& slot,
                                 int expected_frame_number);
bool frameSlotIsWritable(const PictureBuffer& slot);
std::optional<FrameSlotMetadata>
frameSlotSnapshotReadable(const PictureBuffer& slot);

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
    unsigned char* frame() const {
        return slot_ != nullptr ? slot_->frame : nullptr;
    }

    void publish(const FrameSlotMetadata& metadata,
                 std::shared_ptr<FrameSurface> surface = {});
    void cancel();

  private:
    friend std::optional<FrameSlotWriteLease>
    frameSlotAcquireWritable(PictureBuffer& slot);

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
    unsigned char* frame() const {
        return slot_ != nullptr ? slot_->frame : nullptr;
    }
    const FrameSlotMetadata& metadata() const { return metadata_; }
    const FrameSurface& surface() const { return *surface_; }

    void release();

  private:
    friend std::optional<FrameSlotReadLease>
    frameSlotAcquireReadable(PictureBuffer& slot);

    FrameSlotReadLease(PictureBuffer& slot, const FrameSlotMetadata& metadata,
                       std::shared_ptr<const FrameSurface> surface);
    void moveFrom(FrameSlotReadLease&& other) noexcept;

    PictureBuffer* slot_ = nullptr;
    FrameSlotMetadata metadata_;
    std::shared_ptr<const FrameSurface> surface_;
    bool active_ = false;
};

std::optional<FrameSlotWriteLease>
frameSlotAcquireWritable(PictureBuffer& slot);
std::optional<FrameSlotReadLease> frameSlotAcquireReadable(PictureBuffer& slot);

#endif
