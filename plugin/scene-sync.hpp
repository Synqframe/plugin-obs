// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

// Scene integrity between OBS and the web (layout.schema.json layoutScenes): one plugin-owned OBS scene per web scene on a canvas, marked with the web scene's
// id. This plans what one canvas needs; no OBS, so it is tested alone.

#include <set>
#include <string>
#include <utility>
#include <vector>

struct WantedScene {
  std::string id;   // the category scene key; never changes
  std::string name; // may be renamed in the web
};

struct ExistingScene {
  std::string name;
  std::string id;       // our mark; empty: not ours (the creator's own scene)
  bool created = false; // made by the plugin for the web (not adopted from the creator)
};

struct SceneSyncPlan {
  std::vector<WantedScene> create;                         // a new scene, marked
  std::vector<std::pair<std::string, std::string>> adopt;  // existing scene name -> id to mark
  std::vector<std::pair<std::string, std::string>> rename; // scene name -> new name
  std::vector<std::string> remove;                         // names of scenes we made that the web no longer has
  // Scenes we adopted (the creator's own, marked on a first sync or by name) that the web no
  // longer has: the mark comes off and the scene stays, since its contents are the creator's.
  std::vector<std::string> unmark;
};

// `firstSyncScene` is the scene that holds the bundles (OBS's current one), adopted as
// `firstSyncId` when nothing on the canvas is ours yet, so nothing is duplicated. An unmarked
// scene with a wanted scene's exact name is adopted too. Scenes not ours are never renamed or
// removed, and a rename that would collide with another scene's name waits. Only a scene the
// plugin itself created is ever removed; an adopted one the web drops is given back unmarked
// (a web edit must never delete a creator's scene).
inline SceneSyncPlan planSceneSync(const std::vector<WantedScene> &wanted, const std::vector<ExistingScene> &existing,
                                   const std::string &firstSyncScene, const std::string &firstSyncId)
{
  SceneSyncPlan plan;
  bool anyOurs = false;
  for (const ExistingScene &scene : existing)
    if (!scene.id.empty()) anyOurs = true;
  std::set<std::string> names, adopted;
  for (const ExistingScene &scene : existing) names.insert(scene.name);
  std::set<std::string> wantedIds;
  for (const WantedScene &want : wanted) wantedIds.insert(want.id);

  const auto renameTo = [&](const std::string &from, const std::string &to) {
    if (from == to || names.count(to)) return;
    plan.rename.push_back({from, to});
    names.erase(from);
    names.insert(to);
  };
  for (const WantedScene &want : wanted) {
    const ExistingScene *ours = nullptr;
    for (const ExistingScene &scene : existing)
      if (scene.id == want.id) ours = &scene;
    if (ours) {
      renameTo(ours->name, want.name);
      continue;
    }
    const ExistingScene *same = nullptr;
    for (const ExistingScene &scene : existing)
      if (scene.id.empty() && scene.name == want.name && !adopted.count(scene.name)) same = &scene;
    if (!same && !anyOurs && want.id == firstSyncId)
      for (const ExistingScene &scene : existing)
        if (scene.id.empty() && scene.name == firstSyncScene && !adopted.count(scene.name)) same = &scene;
    if (same) {
      plan.adopt.push_back({same->name, want.id});
      adopted.insert(same->name);
      renameTo(same->name, want.name);
      continue;
    }
    if (names.count(want.name)) continue; // a scene of the creator's has that name: wait
    plan.create.push_back(want);
    names.insert(want.name);
  }
  for (const ExistingScene &scene : existing) {
    if (scene.id.empty() || wantedIds.count(scene.id)) continue;
    (scene.created ? plan.remove : plan.unmark).push_back(scene.name);
  }
  return plan;
}
