// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_INLINE_PASS_CONTEXT_H_
#define FLUTTER_IMPELLER_ENTITY_INLINE_PASS_CONTEXT_H_

#include <cstdint>

#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/entity_pass_target.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/render_pass.h"

namespace impeller {

/// @brief  The load action the color attachment of pass number `pass_count`
///         over an entity pass target must use.
///
///         The first pass clears, because the target it is about to paint has
///         no contents anyone has vouched for. Targets handed out by
///         `RenderTargetCache` are recycled within and across frames and still
///         hold the previous tenant's pixels; their declared `kDontCare` is a
///         statement that no one chose a load action, not a statement that the
///         pixels are disposable. Inheriting it paints a save layer over
///         another layer's leftovers.
///
///         `honor_declared_load_action` is the narrow exception: the caller
///         owns this target and its declared action is a real decision. The
///         only such caller is the root pass over an embedder-supplied render
///         target, where an embedder doing partial repaint declares `kLoad` to
///         keep the pixels it preserved outside the damage region.
///
///         Later passes are continuations of a target this context has already
///         painted, so they load it -- except that a fresh MSAA attachment has
///         nothing to load and must be cleared before it is resolved.
LoadAction ColorLoadActionForPass(uint32_t pass_count,
                                  bool is_msaa,
                                  LoadAction declared_load_action,
                                  bool honor_declared_load_action);

class InlinePassContext {
 public:
  /// @param  honor_declared_load_action  Whether the first render pass keeps
  ///         the load action that `pass_target`'s color attachment already
  ///         declares, instead of clearing. See `ColorLoadActionForPass`.
  InlinePassContext(const ContentContext& renderer,
                    EntityPassTarget& pass_target,
                    bool honor_declared_load_action);

  ~InlinePassContext();

  bool IsValid() const;

  bool IsActive() const;

  std::shared_ptr<Texture> GetTexture();

  bool EndPass(bool is_onscreen = false);

  EntityPassTarget& GetPassTarget() const;

  uint32_t GetPassCount() const;

  const std::shared_ptr<RenderPass>& GetRenderPass();

 private:
  const ContentContext& renderer_;
  EntityPassTarget& pass_target_;
  std::shared_ptr<CommandBuffer> command_buffer_;
  std::shared_ptr<RenderPass> pass_;
  uint32_t pass_count_ = 0;
  bool honor_declared_load_action_ = false;

  InlinePassContext(const InlinePassContext&) = delete;

  InlinePassContext& operator=(const InlinePassContext&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_INLINE_PASS_CONTEXT_H_
