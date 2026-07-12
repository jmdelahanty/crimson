#include "frame_slot.h"

#include <algorithm>
#include <memory>
#include <mutex>

enum class FrameSlotPhase {
    Writable,
    Writing,
    Ready,
};

namespace {

class PictureBufferFrameSurface final : public FrameSurface {
  public:
    PictureBufferFrameSurface(unsigned char* frame,
                              const FrameSurfaceDescriptor& descriptor)
        : base_handle_(reinterpret_cast<uintptr_t>(frame)),
          descriptor_(descriptor) {}

    const FrameSurfaceDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    uintptr_t nativeHandle(size_t plane_index) const noexcept override {
        if (plane_index >= descriptor_.plane_count) {
            return 0;
        }
        return base_handle_ + descriptor_.planes[plane_index].offset_bytes;
    }

  private:
    uintptr_t base_handle_ = 0;
    FrameSurfaceDescriptor descriptor_;
};

}  // namespace

struct FrameSlotState {
    mutable std::mutex mutex;
    FrameSlotPhase phase = FrameSlotPhase::Writable;
    int active_readers = 0;
    FrameSlotMetadata metadata;
    std::shared_ptr<FrameSurface> surface;
};

namespace {

FrameSlotMetadata metadataFromSlot(const PictureBuffer& slot) {
    FrameSlotMetadata metadata;
    metadata.frame_number = slot.frame_number;
    metadata.local_frame_number = slot.local_frame_number;
    metadata.frame_pts = slot.frame_pts;
    metadata.frame_source_code = slot.frame_source_code;
    metadata.pitch_bytes = slot.pitch_bytes;
    metadata.frame_bytes = slot.frame_bytes;
    metadata.color_matrix = slot.color_matrix;
    metadata.color_range = slot.color_range;
    metadata.pixel_format = slot.format;
    return metadata;
}

FrameSlotMetadata normalizeMetadata(const PictureBuffer& slot,
                                    const FrameSlotMetadata& input) {
    FrameSlotMetadata metadata = input;
    if (metadata.pixel_format == FramePixelFormat::Unknown) {
        metadata.pixel_format = slot.format;
    }
    if (metadata.pitch_bytes <= 0) {
        metadata.pitch_bytes = slot.pitch_bytes;
    }
    if (metadata.frame_bytes == 0) {
        metadata.frame_bytes = slot.frame_bytes;
    }
    if (metadata.plane_count == 0) {
        metadata.planes = describeFramePlanes(
            metadata.pixel_format, metadata.width, metadata.height,
            metadata.pitch_bytes, &metadata.plane_count);
    }
    if (metadata.ownership == FrameSurfaceOwnership::Unspecified) {
        metadata.ownership = FrameSurfaceOwnership::SlotOwned;
    }
    if (metadata.lifetime == FrameSurfaceLifetime::Unspecified) {
        metadata.lifetime = FrameSurfaceLifetime::UntilReadLeaseReleased;
    }
    return metadata;
}

void applyMetadata(PictureBuffer& slot, const FrameSlotMetadata& metadata) {
    slot.frame_number = metadata.frame_number;
    slot.local_frame_number = metadata.local_frame_number;
    slot.frame_pts = metadata.frame_pts;
    slot.frame_source_code = metadata.frame_source_code;
    slot.pitch_bytes = metadata.pitch_bytes;
    slot.frame_bytes = metadata.frame_bytes;
    slot.color_matrix = metadata.color_matrix;
    slot.color_range = metadata.color_range;
    slot.format = metadata.pixel_format;
}

void clearPublishedMetadata(PictureBuffer& slot, FrameSlotState* state) {
    slot.frame_number = -1;
    slot.local_frame_number = -1;
    slot.frame_pts = -1;
    slot.frame_source_code = 0;
    slot.color_matrix = ColorSpaceStandard_BT709;
    slot.color_range = ColorRange_Unspecified;
    if (state != nullptr) {
        state->metadata = {};
        state->surface.reset();
    }
}

FrameSlotState* ensureState(PictureBuffer& slot) {
    if (slot.frame_slot_state == nullptr) {
        slot.frame_slot_state = new FrameSlotState();
        slot.frame_slot_state->phase = slot.available_to_write
                                           ? FrameSlotPhase::Writable
                                           : FrameSlotPhase::Ready;
    }
    return slot.frame_slot_state;
}

}  // namespace

void frameSlotInitialize(PictureBuffer& slot) {
    FrameSlotState* state = ensureState(slot);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->phase = slot.available_to_write ? FrameSlotPhase::Writable
                                           : FrameSlotPhase::Ready;
    state->active_readers = 0;
    state->metadata = metadataFromSlot(slot);
    if (!slot.available_to_write && slot.frame_number >= 0) {
        state->surface = std::make_shared<PictureBufferFrameSurface>(
            slot.frame, frameSurfaceDescriptorFromMetadata(state->metadata));
    }
}

void frameSlotDestroy(PictureBuffer& slot) {
    delete slot.frame_slot_state;
    slot.frame_slot_state = nullptr;
}

void frameSlotResetForWrite(PictureBuffer& slot) {
    FrameSlotState* state = ensureState(slot);
    std::lock_guard<std::mutex> lock(state->mutex);
    clearPublishedMetadata(slot, state);
    slot.available_to_write = true;
    state->phase = FrameSlotPhase::Writable;
}

void frameSlotReleaseForReuse(PictureBuffer& slot) {
    frameSlotResetForWrite(slot);
}

bool frameSlotTryReleaseForReuse(PictureBuffer& slot,
                                 int expected_frame_number) {
    FrameSlotState* state = ensureState(slot);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (slot.available_to_write || slot.frame_number < 0 ||
        state->phase != FrameSlotPhase::Ready || state->active_readers > 0 ||
        slot.frame_number != expected_frame_number) {
        return false;
    }
    clearPublishedMetadata(slot, state);
    slot.available_to_write = true;
    state->phase = FrameSlotPhase::Writable;
    return true;
}

bool frameSlotIsWritable(const PictureBuffer& slot) {
    FrameSlotState* state = slot.frame_slot_state;
    if (state == nullptr) {
        return slot.available_to_write;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return slot.available_to_write && state->active_readers == 0;
}

std::optional<FrameSlotMetadata>
frameSlotSnapshotReadable(const PictureBuffer& slot) {
    FrameSlotState* state = slot.frame_slot_state;
    if (state == nullptr) {
        if (slot.available_to_write || slot.frame_number < 0) {
            return std::nullopt;
        }
        return metadataFromSlot(slot);
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    if (slot.available_to_write || slot.frame_number < 0) {
        return std::nullopt;
    }
    return state->metadata.frame_number >= 0 ? state->metadata
                                             : metadataFromSlot(slot);
}

FrameSlotWriteLease::FrameSlotWriteLease(PictureBuffer& slot)
    : slot_(&slot), active_(true) {}

FrameSlotWriteLease::FrameSlotWriteLease(FrameSlotWriteLease&& other) noexcept {
    moveFrom(std::move(other));
}

FrameSlotWriteLease&
FrameSlotWriteLease::operator=(FrameSlotWriteLease&& other) noexcept {
    if (this != &other) {
        cancel();
        moveFrom(std::move(other));
    }
    return *this;
}

FrameSlotWriteLease::~FrameSlotWriteLease() { cancel(); }

void FrameSlotWriteLease::moveFrom(FrameSlotWriteLease&& other) noexcept {
    slot_ = other.slot_;
    active_ = other.active_;
    other.slot_ = nullptr;
    other.active_ = false;
}

void FrameSlotWriteLease::publish(const FrameSlotMetadata& input_metadata,
                                  std::shared_ptr<FrameSurface> surface) {
    if (slot_ == nullptr || !active_) {
        return;
    }
    FrameSlotState* state = ensureState(*slot_);
    std::lock_guard<std::mutex> lock(state->mutex);
    const FrameSlotMetadata metadata =
        normalizeMetadata(*slot_, input_metadata);
    applyMetadata(*slot_, metadata);
    state->metadata = metadata;
    state->surface =
        surface
            ? std::move(surface)
            : std::make_shared<PictureBufferFrameSurface>(
                  slot_->frame, frameSurfaceDescriptorFromMetadata(metadata));
    slot_->available_to_write = false;
    state->phase = FrameSlotPhase::Ready;
    active_ = false;
    slot_ = nullptr;
}

void FrameSlotWriteLease::cancel() {
    if (slot_ == nullptr || !active_) {
        return;
    }
    FrameSlotState* state = ensureState(*slot_);
    std::lock_guard<std::mutex> lock(state->mutex);
    clearPublishedMetadata(*slot_, state);
    slot_->available_to_write = true;
    state->phase = FrameSlotPhase::Writable;
    active_ = false;
    slot_ = nullptr;
}

std::optional<FrameSlotWriteLease>
frameSlotAcquireWritable(PictureBuffer& slot) {
    FrameSlotState* state = ensureState(slot);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!slot.available_to_write || state->active_readers > 0) {
        return std::nullopt;
    }
    clearPublishedMetadata(slot, state);
    slot.available_to_write = false;
    state->phase = FrameSlotPhase::Writing;
    return FrameSlotWriteLease(slot);
}

FrameSlotReadLease::FrameSlotReadLease(
    PictureBuffer& slot, const FrameSlotMetadata& metadata,
    std::shared_ptr<const FrameSurface> surface)
    : slot_(&slot), metadata_(metadata), surface_(std::move(surface)),
      active_(true) {}

FrameSlotReadLease::FrameSlotReadLease(FrameSlotReadLease&& other) noexcept {
    moveFrom(std::move(other));
}

FrameSlotReadLease&
FrameSlotReadLease::operator=(FrameSlotReadLease&& other) noexcept {
    if (this != &other) {
        release();
        moveFrom(std::move(other));
    }
    return *this;
}

FrameSlotReadLease::~FrameSlotReadLease() { release(); }

void FrameSlotReadLease::moveFrom(FrameSlotReadLease&& other) noexcept {
    slot_ = other.slot_;
    metadata_ = other.metadata_;
    surface_ = std::move(other.surface_);
    active_ = other.active_;
    other.slot_ = nullptr;
    other.active_ = false;
}

void FrameSlotReadLease::release() {
    if (slot_ == nullptr || !active_) {
        return;
    }
    FrameSlotState* state = ensureState(*slot_);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->active_readers = std::max(0, state->active_readers - 1);
    active_ = false;
    slot_ = nullptr;
    surface_.reset();
}

std::optional<FrameSlotReadLease>
frameSlotAcquireReadable(PictureBuffer& slot) {
    FrameSlotState* state = ensureState(slot);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (slot.available_to_write || slot.frame_number < 0) {
        return std::nullopt;
    }
    state->phase = FrameSlotPhase::Ready;
    ++state->active_readers;
    const FrameSlotMetadata metadata = state->metadata.frame_number >= 0
                                           ? state->metadata
                                           : metadataFromSlot(slot);
    if (!state->surface) {
        state->surface = std::make_shared<PictureBufferFrameSurface>(
            slot.frame, frameSurfaceDescriptorFromMetadata(metadata));
    }
    return FrameSlotReadLease(slot, metadata, state->surface);
}
