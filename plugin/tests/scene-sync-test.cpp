// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

// Scene integrity's plan for one canvas (scene-sync.hpp). No OBS.
#include "scene-sync.hpp"

#include "check.hpp"

int main()
{
  const std::vector<WantedScene> web{{"gameplay", "Gameplay"}, {"spectate", "Spectate"}};

  // First sync: nothing is ours. The scene holding the bundles becomes gameplay (renamed), the
  // rest is created, and the creator's BRB is left alone.
  SceneSyncPlan plan = planSceneSync(web, {{"Default", ""}, {"BRB", ""}}, "Default", "gameplay");
  CHECK(plan.adopt.size() == 1 && plan.adopt[0].first == "Default" && plan.adopt[0].second == "gameplay");
  CHECK(plan.rename.size() == 1 && plan.rename[0].first == "Default" && plan.rename[0].second == "Gameplay");
  CHECK(plan.create.size() == 1 && plan.create[0].id == "spectate");
  CHECK(plan.remove.empty());

  // In sync: nothing to do.
  plan = planSceneSync(web, {{"Gameplay", "gameplay"}, {"Spectate", "spectate"}, {"BRB", ""}}, "Gameplay", "gameplay");
  CHECK(plan.create.empty() && plan.adopt.empty() && plan.rename.empty() && plan.remove.empty());

  // A rename in the web, and a scene the web dropped: one we created is removed, one we adopted
  // (the creator's own) only loses its mark, and the creator's unmarked BRB is kept.
  plan = planSceneSync({{"gameplay", "Game"}},
                       {{"Gameplay", "gameplay"}, {"Spectate", "spectate", true}, {"Talk", "talk"}, {"BRB", ""}},
                       "Gameplay", "gameplay");
  CHECK(plan.rename.size() == 1 && plan.rename[0].second == "Game");
  CHECK(plan.remove.size() == 1 && plan.remove[0] == "Spectate");
  CHECK(plan.unmark.size() == 1 && plan.unmark[0] == "Talk");

  // Once something is ours, the current scene is never adopted; an unmarked scene with the
  // exact name is.
  plan = planSceneSync(web, {{"Gameplay", "gameplay"}, {"Default", ""}, {"Spectate", ""}}, "Default", "gameplay");
  CHECK(plan.adopt.size() == 1 && plan.adopt[0].first == "Spectate" && plan.adopt[0].second == "spectate");
  CHECK(plan.create.empty() && plan.rename.empty());

  // A rename onto a name the creator uses waits; so does creating one.
  plan = planSceneSync({{"gameplay", "BRB"}, {"spectate", "Talk"}}, {{"Gameplay", "gameplay"}, {"BRB", ""}, {"Talk", "x"}},
                       "Gameplay", "gameplay");
  CHECK(plan.rename.empty());
  CHECK(plan.adopt.empty() && plan.create.empty()); // "Talk" is someone's (marked x): no create
  CHECK(plan.remove.empty() && plan.unmark.size() == 1 && plan.unmark[0] == "Talk"); // x isn't wanted; adopted, so unmarked

  // No web scenes: what we created goes, what we adopted is unmarked, nothing else is touched.
  plan = planSceneSync({}, {{"Gameplay", "gameplay"}, {"Spectate", "spectate", true}, {"BRB", ""}}, "Gameplay", "");
  CHECK(plan.remove.size() == 1 && plan.remove[0] == "Spectate" && plan.create.empty());
  CHECK(plan.unmark.size() == 1 && plan.unmark[0] == "Gameplay");

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("scene-sync-test: all checks passed\n");
  return 0;
}
