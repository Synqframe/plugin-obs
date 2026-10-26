// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

// The layout's stacking rule as OBS applies it (layout-stacking.hpp). No OBS.
#include "layout-stacking.hpp"

#include "check.hpp"

#include <vector>

int main()
{
  using Order = std::vector<std::size_t>;

  // A real scene: the camera (zIndex 1) sat below the game (zIndex 0).
  // Bottom first: camera, game. It must become game, camera.
  CHECK((stackingOrder({{true, 1, 1}, {true, 0, 0}}) == Order{1, 0}));
  // Already right: unchanged.
  CHECK((stackingOrder({{true, 0, 0}, {true, 1, 1}}) == Order{0, 1}));

  // A tie: the later slot in `slots` is above, whatever OBS had.
  CHECK((stackingOrder({{true, 2, 3}, {true, 2, 1}}) == Order{1, 0}));
  CHECK((stackingOrder({{true, 2, 1}, {true, 2, 3}}) == Order{0, 1}));

  // Items the layout doesn't place (an overlay, a plain source) keep their positions; the laid
  // out ones are sorted into the positions they held.
  //   bottom first: [game z1] [overlay] [camera z0] [alert]
  //   becomes:      [camera ] [overlay] [game     ] [alert]
  CHECK((stackingOrder({{true, 1, 0}, {}, {true, 0, 1}, {}}) == Order{2, 1, 0, 3}));

  // Three, with a tie in the middle and a negative zIndex.
  CHECK((stackingOrder({{true, 5, 0}, {true, -1, 1}, {true, 5, 2}}) == Order{1, 0, 2}));

  // Nothing laid out, and an empty scene.
  CHECK((stackingOrder({{}, {}}) == Order{0, 1}));
  CHECK(stackingOrder({}).empty());

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("layout-stacking-test: all checks passed\n");
  return 0;
}
