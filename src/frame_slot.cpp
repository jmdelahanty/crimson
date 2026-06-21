#include "frame_slot.h"

#include <algorithm>
#include <mutex>

enum class FrameSlotPhase {
    Writable,
    Writing,
    Ready,
};

struct FrameSlotState {
    mutable std::mutex mutex;
    FrameSlotPhase phase = FrameSlotPhase::Writable;
    int active_readers = 0;
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
    metadata.format = slot.format;
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
    slot.format = metadata.format;
}

void clearPublishedMetadata(PictureBuffer& slot) {
    slot.frame_number = -1;
    slot.local_frame_number = -1;
    slot.frame_pts = -1;
    slot.frame_source_code = 0;
    slot.color_matrix = ColorSpaceStandard_BT709;
}

FrameSlotState* ensureState(PictureBuffer& slot) {
    if (slot.frame_slot_state == nullptr) {
        slot.frame_slot_state = new FrameSlotState();
        slot.frame_slot_state->phase =
            slot.available_to_write ? FrameSlotPhase::Writable
                                    : FrameSlotPhase::Ready;
    }
    return slot.frame_slot_state;
}

}  // namespace

void frameSlotInitialize(PictureBuffer& slot) {
    FrameSlotState* state = ensureState(slot);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->phase =
        slot.available_to_write ? FrameSlotPhase::Writable : FrameSlotPhase::Ready;
    state->active_readers = 0;
}

void frameSlotDestroy(PictureBuffer& slot) {
    delete slot.frame_slot_state;
    slot.frame_slot_state = nullptr;
}

void frameSlotResetForWrite(PictureBuffer& slot) {
    FrameSlotState* state = ensureState(slot);
    std::lock_guard<std::mutex> lock(state->mutex);
    clearPublishedMetadata(slot);
    slot.available_to_write = true;
    state->phase = FrameSlotPhase::Writable;
}

void frameSlotReleaseForReuse(PictureBuffer& slot) {
    frameSlotResetForWrite(slot);
}

bool frameSlotIsWritable(const PictureBuffer& slot) {
    FrameSlotState* state = slot.frame_slot_state;
    if (state == nullptr) {
        return slot.available_to_write;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return slot.available_to_write && state->active_readers == 0;
}

std::optional<FrameSlotMetadata> frameSlotSnapshotReadable(
    const PictureBuffer& slot) {
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
    return metadataFromSlot(slot);
}

FrameSlotWriteLease::FrameSlotWriteLease(PictureBuffer& slot)
    : slot_(&slot), active_(true) {}

FrameSlotWriteLease::FrameSlotWriteLease(FrameSlotWriteLease&& other) noexcept {
    moveFrom(std::move(other));
}

FrameSlotWriteLease& FrameSlotWriteLease::operator=(
    FrameSlotWriteLease&& other) noexcept {
    if (this != &other) {
        cancel();
        moveFrom(std::move(other));
    }
    return *this;
}

FrameSlotWriteLease::~FrameSlotWriteLease() {
    cancel();
}

void FrameSlotWriteLease::moveFrom(FrameSlotWriteLease&& other) noexcept {
    slot_ = other.slot_;
    active_ = other.active_;
    other.slot_ = nullptr;
    other.active_ = false;
}

void FrameSlotWriteLease::publish(const FrameSlotMetadata& metadata) {
    if (slot_ == nullptr || !active_) {
        return;
    }
    FrameSlotState* state = ensureState(*slot_);
    std::lock_guard<std::mutex> lock(state->mutex);
    applyMetadata(*slot_, metadata);
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
    clearPublishedMetadata(*slot_);
    slot_->available_to_write = true;
    state->phase = FrameSlotPhase::Writable;
    active_ = false;
    slot_ = nullptr;
}

std::optional<FrameSlotWriteLease> frameSlotAcquireWritable(
    PictureBuffer& slot) {
    FrameSlotState* state = ensureState(slot);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!slot.available_to_write || state->active_readers > 0) {
        return std::nullopt;
    }
    clearPublishedMetadata(slot);
    slot.available_to_write = false;
    state->phase = FrameSlotPhase::Writing;
    return FrameSlotWriteLease(slot);
}

FrameSlotReadLease::FrameSlotReadLease(PictureBuffer& slot,
                                       const FrameSlotMetadata& metadata)
    : slot_(&slot), metadata_(metadata), active_(true) {}

FrameSlotReadLease::FrameSlotReadLease(FrameSlotReadLease&& other) noexcept {
    moveFrom(std::move(other));
}

FrameSlotReadLease& FrameSlotReadLease::operator=(
    FrameSlotReadLease&& other) noexcept {
    if (this != &other) {
        release();
        moveFrom(std::move(other));
    }
    return *this;
}

FrameSlotReadLease::~FrameSlotReadLease() {
    release();
}

void FrameSlotReadLease::moveFrom(FrameSlotReadLease&& other) noexcept {
    slot_ = other.slot_;
    metadata_ = other.metadata_;
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
}

std::optional<FrameSlotReadLease> frameSlotAcquireReadable(PictureBuffer& slot) {
    FrameSlotState* state = ensureState(slot);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (slot.available_to_write || slot.frame_number < 0) {
        return std::nullopt;
    }
    state->phase = FrameSlotPhase::Ready;
    ++state->active_readers;
    return FrameSlotReadLease(slot, metadataFromSlot(slot));
}
