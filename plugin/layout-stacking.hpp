// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

// The layout's stacking rule (layout.schema.json zIndex): a higher zIndex is above
// a lower one, and on a tie the later slot in `slots` is above. OBS follows it exactly, on every
// apply; the scene's other items (not bundles) keep their places. No OBS, so it is tested alone.

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

struct StackEntry {
  bool laidOut = false; // a bundle item the layout places
  int zIndex = 0;
  int slotIndex = 0; // its slot's place in `slots`
};

// The scene's new order, bottom first, as indexes into `current` (also bottom first). The laid
// out items are sorted by (zIndex, slotIndex) into the positions laid out items already hold.
inline std::vector<std::size_t> stackingOrder(const std::vector<StackEntry> &current)
{
  std::vector<std::size_t> order(current.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::vector<std::size_t> laidOut;
  for (std::size_t i = 0; i < current.size(); ++i)
    if (current[i].laidOut) laidOut.push_back(i);
  std::vector<std::size_t> sorted = laidOut;
  std::stable_sort(sorted.begin(), sorted.end(), [&](std::size_t a, std::size_t b) {
    if (current[a].zIndex != current[b].zIndex) return current[a].zIndex < current[b].zIndex;
    return current[a].slotIndex < current[b].slotIndex;
  });
  for (std::size_t i = 0; i < laidOut.size(); ++i) order[laidOut[i]] = sorted[i];
  return order;
}
