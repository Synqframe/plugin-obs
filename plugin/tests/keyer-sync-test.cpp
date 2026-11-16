// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

// A creator's edits in Downstream Keyer's dock read as layout keyer edits (keyer-sync.hpp). No OBS.
#include "keyer-sync.hpp"

#include "check.hpp"

int main()
{
  int minted = 0;
  const auto newId = [&minted] { return "new" + std::to_string(++minted); };
  const std::vector<KeyerModel> web{
    {"k1", "Overlays", {{"s1", "Bug", "kh1", "kv1"}, {"s2", "Lower third", "kh2", ""}}, "s1", {}, false}};
  const DskTab tab{"Overlays", {{"s1", "Bug"}, {"s2", "Lower third"}}, "Bug", {}, false};

  // Nothing changed: nothing to send.
  KeyerEdit edit = diffKeyers(web, {tab}, {tab}, newId);
  CHECK(edit.select.empty() && !edit.keyers);

  // A selection: keyer.select only.
  DskTab selected = tab;
  selected.selected = "Lower third";
  edit = diffKeyers(web, {tab}, {selected}, newId);
  CHECK(edit.select.size() == 1 && edit.select[0].first == "k1" && edit.select[0].second == "s2" && !edit.keyers);
  // Selecting nothing: null.
  selected.selected.clear();
  edit = diffKeyers(web, {tab}, {selected}, newId);
  CHECK(edit.select.size() == 1 && edit.select[0].second.empty());

  // A creator's scene added: a new keyer scene with no keys, in the tab's place.
  DskTab added = tab;
  added.scenes.insert(added.scenes.begin() + 1, {"", "Webcam frame"});
  edit = diffKeyers(web, {tab}, {added}, newId);
  CHECK(edit.keyers && edit.keyers->size() == 1);
  if (edit.keyers) {
    const auto &scenes = edit.keyers->front().scenes;
    CHECK(scenes.size() == 3 && scenes[1].name == "Webcam frame" && scenes[1].horizontal.empty() && scenes[1].id.rfind("new", 0) == 0);
    CHECK(scenes[0].horizontal == "kh1" && scenes[0].vertical == "kv1"); // the others keep their keys
  }
  // The same creator's scene still there on the next poll (the server refused it): not sent again.
  edit = diffKeyers(web, {added}, {added}, newId);
  CHECK(!edit.keyers);

  // A scene removed on one canvas goes from the keyer; the selection follows it out.
  DskTab removed = tab;
  removed.scenes.erase(removed.scenes.begin());
  removed.selected.clear();
  edit = diffKeyers(web, {tab}, {removed}, newId);
  CHECK(edit.keyers && edit.keyers->front().scenes.size() == 1 && edit.keyers->front().selected.empty());
  CHECK(edit.select.empty());

  // The vertical tab lists only scenes with a vertical key: s2's absence there isn't a removal.
  const DskTab vertical{"Overlays", {{"s1", "Bug"}}, "Bug", {}, false};
  edit = diffKeyers(web, {vertical}, {vertical}, newId);
  CHECK(!edit.keyers);

  // Exclusions and tie.
  DskTab excluded = tab;
  excluded.exclude = {"brb"};
  excluded.tie = true;
  edit = diffKeyers(web, {tab}, {excluded}, newId);
  CHECK(edit.keyers && edit.keyers->front().exclude == std::vector<std::string>{"brb"} && edit.keyers->front().tie);

  // The keyer's tab removed: the keyer goes.
  edit = diffKeyers(web, {tab}, {}, newId);
  CHECK(edit.keyers && edit.keyers->empty());

  // Renamed in place: same keyer, new name.
  DskTab renamed = tab;
  renamed.name = "Graphics";
  edit = diffKeyers(web, {tab}, {renamed}, newId);
  CHECK(edit.keyers && edit.keyers->size() == 1 && edit.keyers->front().id == "k1" && edit.keyers->front().name == "Graphics");

  // A creator's new tab: nothing while empty, a new keyer once it has a scene.
  const DskTab empty{"Mine", {}, "", {}, false};
  edit = diffKeyers(web, {tab}, {tab, empty}, newId);
  CHECK(!edit.keyers);
  DskTab mine = empty;
  mine.scenes = {{"", "Sponsor"}};
  mine.selected = "Sponsor";
  edit = diffKeyers(web, {tab, empty}, {tab, mine}, newId);
  CHECK(edit.keyers && edit.keyers->size() == 2);
  if (edit.keyers && edit.keyers->size() == 2) {
    const KeyerModel &made = edit.keyers->back();
    CHECK(made.name == "Mine" && made.scenes.size() == 1 && made.selected == made.scenes[0].id);
  }
  // A creator's tab that was there before and didn't change (first poll's baseline): left alone.
  edit = diffKeyers(web, {tab, mine}, {tab, mine}, newId);
  CHECK(!edit.keyers);

  // A keyer's tab appearing (ours, being made by an apply): nothing.
  edit = diffKeyers(web, {}, {tab}, newId);
  CHECK(!edit.keyers && edit.select.empty());

  return failures ? 1 : 0;
}
