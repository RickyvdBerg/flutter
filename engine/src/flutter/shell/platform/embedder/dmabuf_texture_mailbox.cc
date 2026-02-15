// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/dmabuf_texture_mailbox.h"

namespace flutter {

DmabufTextureMailbox::DmabufTextureMailbox() = default;

DmabufTextureMailbox::~DmabufTextureMailbox() {
  std::scoped_lock lock(mutex_);
  for (auto& [id, entry] : entries_) {
    if (entry.release_callback) {
      entry.release_callback();
    }
  }
  entries_.clear();
}

void DmabufTextureMailbox::Store(int64_t texture_id,
                                 DmabufMailboxEntry entry) {
  std::scoped_lock lock(mutex_);
  auto it = entries_.find(texture_id);
  if (it != entries_.end()) {
    // Previous entry was never consumed — fire its release callback.
    if (it->second.release_callback) {
      it->second.release_callback();
    }
    it->second = std::move(entry);
  } else {
    entries_.emplace(texture_id, std::move(entry));
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
  std::scoped_lock lock(mutex_);
  auto it = entries_.find(texture_id);
  if (it == entries_.end()) {
    return;
  }
  if (it->second.release_callback) {
    it->second.release_callback();
  }
  entries_.erase(it);
}

}  // namespace flutter
