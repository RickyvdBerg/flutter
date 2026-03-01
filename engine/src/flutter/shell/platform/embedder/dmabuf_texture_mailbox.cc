// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/dmabuf_texture_mailbox.h"

namespace flutter {

namespace {

void RunReleaseCallback(DmabufMailboxEntry& entry) {
  if (entry.release_callback) {
    auto callback = std::move(entry.release_callback);
    callback();
  }
}

}  // namespace

DmabufTextureMailbox::DmabufTextureMailbox() = default;

DmabufTextureMailbox::~DmabufTextureMailbox() {
  std::vector<DmabufMailboxEntry> drained_entries;
  {
    std::scoped_lock lock(mutex_);
    drained_entries.reserve(entries_.size());
    for (auto& [id, entry] : entries_) {
      drained_entries.push_back(std::move(entry));
    }
    entries_.clear();
  }
  for (auto& entry : drained_entries) {
    RunReleaseCallback(entry);
  }
}

void DmabufTextureMailbox::Store(int64_t texture_id, DmabufMailboxEntry entry) {
  std::optional<DmabufMailboxEntry> replaced_entry;
  {
    std::scoped_lock lock(mutex_);
    auto it = entries_.find(texture_id);
    if (it != entries_.end()) {
      replaced_entry.emplace(std::move(it->second));
      it->second = std::move(entry);
    } else {
      entries_.emplace(texture_id, std::move(entry));
    }
  }
  if (replaced_entry.has_value()) {
    RunReleaseCallback(replaced_entry.value());
  }
}

std::unique_ptr<DmabufMailboxEntry> DmabufTextureMailbox::Consume(
    int64_t texture_id) {
  std::scoped_lock lock(mutex_);
  auto it = entries_.find(texture_id);
  if (it == entries_.end()) {
    return nullptr;
  }
  auto result = std::make_unique<DmabufMailboxEntry>(std::move(it->second));
  entries_.erase(it);
  return result;
}

std::vector<DlIRect> DmabufTextureMailbox::PeekDamage(int64_t texture_id) {
  std::scoped_lock lock(mutex_);
  auto it = entries_.find(texture_id);
  if (it == entries_.end()) {
    return {};
  }
  return it->second.damage_rects;
}

void DmabufTextureMailbox::Remove(int64_t texture_id) {
  std::optional<DmabufMailboxEntry> removed_entry;
  {
    std::scoped_lock lock(mutex_);
    auto it = entries_.find(texture_id);
    if (it == entries_.end()) {
      return;
    }
    removed_entry.emplace(std::move(it->second));
    entries_.erase(it);
  }
  if (removed_entry.has_value()) {
    RunReleaseCallback(removed_entry.value());
  }
}

}  // namespace flutter
