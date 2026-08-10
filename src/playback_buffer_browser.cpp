#include "playback_buffer_browser.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace crimson::playback {

PlaybackBufferBrowserModel buildPlaybackBufferBrowserModel(
    std::vector<PlaybackPresentationCandidate> candidates,
    int64_t selected_frame) {
  PlaybackBufferBrowserModel model;
  model.selected_frame = selected_frame;

  candidates.erase(
      std::remove_if(candidates.begin(), candidates.end(),
                     [](const auto &item) { return item.frame_number < 0; }),
      candidates.end());
  std::sort(candidates.begin(), candidates.end(),
            [](const auto &a, const auto &b) {
              if (a.frame_number != b.frame_number) {
                return a.frame_number < b.frame_number;
              }
              if (a.slot.has_value() != b.slot.has_value()) {
                return a.slot.has_value();
              }
              return a.slot.value_or(0) < b.slot.value_or(0);
            });
  model.items = std::move(candidates);
  if (model.items.empty()) {
    return model;
  }

  model.oldest_frame = model.items.front().frame_number;
  model.newest_frame = model.items.back().frame_number;
  model.spans.reserve(model.items.size());
  for (size_t index = 0; index < model.items.size(); ++index) {
    const bool starts_new_span =
        model.spans.empty() ||
        model.items[index].frame_number -
                model.items[model.spans.back().end_index].frame_number >
            1;
    if (starts_new_span) {
      model.spans.push_back({index, index});
    } else {
      model.spans.back().end_index = index;
    }
  }
  for (size_t span_index = 1; span_index < model.spans.size(); ++span_index) {
    const int64_t previous_frame =
        model.items[model.spans[span_index - 1].end_index].frame_number;
    const int64_t next_frame =
        model.items[model.spans[span_index].begin_index].frame_number;
    model.largest_gap =
        std::max(model.largest_gap, next_frame - previous_frame - 1);
  }

  int64_t best_distance = std::numeric_limits<int64_t>::max();
  int64_t best_frame = std::numeric_limits<int64_t>::min();
  for (size_t index = 0; index < model.items.size(); ++index) {
    const auto &item = model.items[index];
    if (item.frame_number == selected_frame) {
      model.selected_index = index;
      model.preferred_slot = item.slot;
      break;
    }
    const int64_t distance = item.frame_number > selected_frame
                                 ? item.frame_number - selected_frame
                                 : selected_frame - item.frame_number;
    if (distance < best_distance ||
        (distance == best_distance && item.frame_number > best_frame)) {
      best_distance = distance;
      best_frame = item.frame_number;
      model.selected_index = index;
    }
  }
  return model;
}

} // namespace crimson::playback
