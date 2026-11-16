// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

// The pack's downstream keyers (layout.schema.json keyers) against Exeldro's Downstream Keyer
// tabs, OBS's side to the web's: what a creator's edit in DSK's dock means for the layout. No OBS,
// no Qt, so it is tested alone (downstream-keyer.cpp drives DSK with it).
//
// A keyer is one DSK tab on each canvas, named as the keyer is; its scenes are OBS scenes marked
// with their keyer scene id (one per canvas the scene has a key for). Each poll of DSK is read
// into DskTabs in the web's terms, and two polls in a row with no apply of ours between them are
// compared: what changed is the creator's.

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct KeyerSceneModel {
  std::string id, name;
  std::string horizontal, vertical; // layout keys; empty: none on that canvas
  bool operator==(const KeyerSceneModel &) const = default;
};

struct KeyerModel {
  std::string id, name;
  std::vector<KeyerSceneModel> scenes;
  std::string selected; // keyer scene id; empty: nothing shown
  std::vector<std::string> exclude; // layoutScenes ids
  bool tie = false;
  bool operator==(const KeyerModel &) const = default;
};

// One entry of a DSK tab's scene list, read in the web's terms.
struct DskScene {
  std::string keyerSceneId; // the scene's mark; empty: a scene of the creator's own
  std::string name;         // the OBS scene's name
  bool operator==(const DskScene &) const = default;
};

// One DSK tab on one canvas.
struct DskTab {
  std::string name;
  std::vector<DskScene> scenes;
  std::string selected;             // the OBS scene name selected; empty: none
  std::vector<std::string> exclude; // layoutScenes ids it is hidden on (a creator scene's: left out)
  bool tie = false;
  bool operator==(const DskTab &) const = default;
};

struct KeyerEdit {
  // keyer.select {keyerId, sceneId}: sceneId empty sends null.
  std::vector<std::pair<std::string, std::string>> select;
  // keyers.commit: the whole array as OBS now has it; none when the structure didn't change.
  std::optional<std::vector<KeyerModel>> keyers;
};

namespace keyer_sync_detail {
inline const KeyerModel *byName(const std::vector<KeyerModel> &keyers, const std::string &name)
{
  for (const KeyerModel &keyer : keyers)
    if (keyer.name == name) return &keyer;
  return nullptr;
}
inline const DskTab *tabNamed(const std::vector<DskTab> &tabs, const std::string &name)
{
  for (const DskTab &tab : tabs)
    if (tab.name == name) return &tab;
  return nullptr;
}
inline std::string selectedId(const DskTab &tab)
{
  for (const DskScene &scene : tab.scenes)
    if (scene.name == tab.selected) return scene.keyerSceneId;
  return {};
}
} // namespace keyer_sync_detail

// What the creator changed on one canvas between `before` and `now` (two polls in a row), against
// the web's `keyers`.
// `newId` mints ids for keyers and keyer scenes the creator made (uuids without dots).
//
// - A selection change in a keyer's tab is a keyer.select (a creator's scene selected that isn't a
//   keyer scene yet: nothing, until the commit that adds it comes back).
// - A tab's scene list, exclusions or tie changing, a keyer's tab removed or renamed, or a new tab
//   with at least one scene: keyers.commit with the whole array. A creator's scene added to a tab
//   is a new keyer scene with no keys (the server mints them); one removed from either canvas's
//   tab goes from the keyer. Keyers whose tab this canvas never had (Aitum missing) are kept.
// - An empty new tab is left alone until a scene is put in it.
inline KeyerEdit diffKeyers(const std::vector<KeyerModel> &keyers, const std::vector<DskTab> &before,
                            const std::vector<DskTab> &now, const std::function<std::string()> &newId)
{
  using namespace keyer_sync_detail;
  KeyerEdit edit;
  std::vector<KeyerModel> next = keyers;
  bool changed = false;

  // A keyer's tab gone from `now`: renamed when an unknown tab took its place, else removed.
  std::set<std::string> renamedTo;
  for (size_t i = 0; i < before.size(); ++i) {
    const KeyerModel *keyer = byName(keyers, before[i].name);
    if (!keyer || tabNamed(now, before[i].name)) continue;
    const DskTab *successor = nullptr;
    if (before.size() == now.size() && !byName(keyers, now[i].name) && !tabNamed(before, now[i].name)) successor = &now[i];
    auto it = std::find_if(next.begin(), next.end(), [&](const KeyerModel &k) { return k.id == keyer->id; });
    if (it == next.end()) continue;
    if (successor) {
      it->name = successor->name;
      renamedTo.insert(successor->name);
    } else {
      next.erase(it);
    }
    changed = true;
  }

  for (const DskTab &tab : now) {
    const DskTab *was = tabNamed(before, tab.name);
    const KeyerModel *known = byName(keyers, tab.name);
    if (!known && renamedTo.count(tab.name)) {
      // The renamed keyer: its tab was `before[i]` under the old name.
      for (const DskTab &old : before)
        if (byName(keyers, old.name) && !tabNamed(now, old.name)) was = &old;
      for (const KeyerModel &keyer : next)
        if (keyer.name == tab.name) known = &keyer;
    }
    if (!known) {
      if (tab.scenes.empty() || (was && *was == tab)) continue;
      // A tab of the creator's with a scene in it: a new keyer.
      KeyerModel keyer;
      keyer.id = newId();
      keyer.name = tab.name;
      for (const DskScene &scene : tab.scenes)
        keyer.scenes.push_back({scene.keyerSceneId.empty() ? newId() : scene.keyerSceneId, scene.name, {}, {}});
      for (size_t i = 0; i < tab.scenes.size(); ++i)
        if (tab.scenes[i].name == tab.selected) keyer.selected = keyer.scenes[i].id;
      keyer.exclude = tab.exclude;
      keyer.tie = tab.tie;
      next.push_back(keyer);
      changed = true;
      continue;
    }
    if (!was) continue; // a keyer's tab just appeared: ours, being made
    auto it = std::find_if(next.begin(), next.end(), [&](const KeyerModel &k) { return k.id == known->id; });
    if (it == next.end()) continue;
    KeyerModel &keyer = *it;
    const bool structure = was->scenes != tab.scenes || was->exclude != tab.exclude || was->tie != tab.tie;
    if (structure) {
      // Scenes: removed ones go; added ones join, in this canvas's order; the rest stay as they are.
      std::vector<KeyerSceneModel> scenes;
      std::set<std::string> listedNow;
      for (const DskScene &scene : tab.scenes)
        if (!scene.keyerSceneId.empty()) listedNow.insert(scene.keyerSceneId);
      std::set<std::string> listedBefore;
      for (const DskScene &scene : was->scenes)
        if (!scene.keyerSceneId.empty()) listedBefore.insert(scene.keyerSceneId);
      for (const KeyerSceneModel &scene : keyer.scenes)
        if (!listedBefore.count(scene.id) || listedNow.count(scene.id)) scenes.push_back(scene);
      if (was->scenes != tab.scenes) {
        // This canvas's order for its scenes; the others keep their places after them.
        std::vector<KeyerSceneModel> ordered;
        for (const DskScene &scene : tab.scenes) {
          if (scene.keyerSceneId.empty()) {
            bool before = false;
            for (const DskScene &old : was->scenes)
              if (old == scene) before = true;
            // A creator's scene already in the tab before is one the server refused: not again.
            if (!before) ordered.push_back({newId(), scene.name, {}, {}});
            continue;
          }
          for (const KeyerSceneModel &model : scenes)
            if (model.id == scene.keyerSceneId) ordered.push_back(model);
        }
        for (const KeyerSceneModel &model : scenes)
          if (std::none_of(ordered.begin(), ordered.end(), [&](const KeyerSceneModel &o) { return o.id == model.id; }))
            ordered.push_back(model);
        scenes = ordered;
      }
      keyer.scenes = scenes;
      if (was->exclude != tab.exclude) {
        std::set<std::string> exclude(keyer.exclude.begin(), keyer.exclude.end());
        for (const std::string &id : was->exclude) exclude.erase(id);
        for (const std::string &id : tab.exclude) exclude.insert(id);
        keyer.exclude.assign(exclude.begin(), exclude.end());
      }
      keyer.tie = tab.tie;
      if (std::none_of(keyer.scenes.begin(), keyer.scenes.end(), [&](const KeyerSceneModel &s) { return s.id == keyer.selected; }))
        keyer.selected.clear();
      changed = true;
    }
    const std::string selectedNow = selectedId(tab);
    if (was->selected != tab.selected && (!selectedNow.empty() || tab.selected.empty()) && selectedNow != keyer.selected) {
      keyer.selected = selectedNow;
      if (!structure) edit.select.push_back({keyer.id, selectedNow});
    }
  }
  if (changed) edit.keyers = next;
  return edit;
}
