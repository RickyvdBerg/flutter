// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_FRAMEBUFFER_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_FRAMEBUFFER_H_

#include <epoxy/gl.h>
#include <glib-object.h>

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(FlFramebuffer, fl_framebuffer, FL, FRAMEBUFFER, GObject)

/**
 * FlFramebuffer:
 *
 * #FlFramebuffer creates framebuffers and their backing textures
 * for use by the Flutter compositor.
 */

/**
 * fl_framebuffer_new:
 * @format: format, e.g. GL_RGB, GL_BGR
 * @width: width of texture.
 * @height: height of texture.
 * @shareable: %TRUE if this framebuffer can be shared between contexts
 * (requires EGL).
 *
 * Creates a new frame buffer. Requires a valid OpenGL context to create.
 *
 * Returns: a new #FlFramebuffer.
 */
FlFramebuffer* fl_framebuffer_new(GLint format,
                                  size_t width,
                                  size_t height,
                                  gboolean shareable);

/**
 * fl_framebuffer_new_multisampled:
 * @format: format, e.g. GL_RGB, GL_BGR
 * @width: width of texture.
 * @height: height of texture.
 * @shareable: %TRUE if this framebuffer can be shared between contexts
 * (requires EGL).
 * @samples: number of samples to rasterize at, clamped to what the driver
 * supports. 1 requests no multisampling.
 *
 * Creates a new frame buffer that rasterizes at more than one sample per
 * pixel, so that geometry rendered into it is antialiased. The backing texture
 * stays single sample; see fl_framebuffer_resolve() for how the samples reach
 * it. Requires a valid OpenGL context to create.
 *
 * @format is a preference, not a guarantee: resolving multisample content can
 * only be done between identical formats, so this may downgrade it. Take the
 * format actually used from fl_framebuffer_get_sized_format() rather than
 * deriving it from @format again.
 *
 * Returns: a new #FlFramebuffer.
 */
FlFramebuffer* fl_framebuffer_new_multisampled(GLint format,
                                               size_t width,
                                               size_t height,
                                               gboolean shareable,
                                               GLsizei samples);

/**
 * fl_framebuffer_resolve:
 * @framebuffer: an #FlFramebuffer.
 *
 * Makes the rendered content available through fl_framebuffer_get_texture_id()
 * and fl_framebuffer_get_resolved_id(). Does nothing unless this framebuffer
 * rasterizes into a separate multisample attachment that the driver will not
 * resolve on its own. Call this after Flutter has rendered a frame and before
 * reading it. Requires a valid OpenGL context.
 */
void fl_framebuffer_resolve(FlFramebuffer* framebuffer);

/**
 * fl_framebuffer_get_shareable:
 * @framebuffer: an #FlFramebuffer.
 *
 * Checks if this framebuffer can be shared between contexts (using
 * fl_framebuffer_create_sibling).
 *
 * Returns: %TRUE if this framebuffer can be shared.
 */
gboolean fl_framebuffer_get_shareable(FlFramebuffer* framebuffer);

/**
 * fl_framebuffer_create_sibling:
 * @framebuffer: an #FlFramebuffer.
 *
 * Creates a new framebuffer with the same backing texture as the original. This
 * uses EGLImage to share the texture and allows a framebuffer created in one
 * OpenGL context to be used in another.
 *
 * Returns: a new #FlFramebuffer.
 */
FlFramebuffer* fl_framebuffer_create_sibling(FlFramebuffer* framebuffer);

/**
 * fl_framebuffer_get_id:
 * @framebuffer: an #FlFramebuffer.
 *
 * Gets the ID for this framebuffer.
 *
 * Returns: OpenGL framebuffer id or 0 if creation failed.
 */
GLuint fl_framebuffer_get_id(FlFramebuffer* framebuffer);

/**
 * fl_framebuffer_get_resolved_id:
 * @framebuffer: an #FlFramebuffer.
 *
 * Gets the ID of the framebuffer whose colour attachment is the texture
 * returned by fl_framebuffer_get_texture_id(). This is the framebuffer to read
 * rendered content from; it is the same as fl_framebuffer_get_id() unless
 * rendering goes to a separate multisample attachment. Call
 * fl_framebuffer_resolve() first.
 *
 * Returns: OpenGL framebuffer id or 0 if creation failed.
 */
GLuint fl_framebuffer_get_resolved_id(FlFramebuffer* framebuffer);

/**
 * fl_framebuffer_get_samples:
 * @framebuffer: an #FlFramebuffer.
 *
 * Gets the number of samples per pixel this framebuffer rasterizes at.
 *
 * Returns: sample count, 1 if not multisampled.
 */
GLsizei fl_framebuffer_get_samples(FlFramebuffer* framebuffer);

/**
 * fl_framebuffer_get_sized_format:
 * @framebuffer: an #FlFramebuffer.
 *
 * Gets the sized colour format of the backing texture, e.g. GL_RGBA8. This is
 * the single authority on what this framebuffer holds: it is the sized form of
 * the format requested at construction unless the resolve required a different
 * one.
 *
 * Returns: a sized OpenGL colour format.
 */
GLint fl_framebuffer_get_sized_format(FlFramebuffer* framebuffer);

/**
 * fl_framebuffer_get_texture_id:
 * @framebuffer: an #FlFramebuffer.
 *
 * Gets the ID of the texture associated with this framebuffer.
 *
 * Returns: OpenGL texture id or 0 if creation failed.
 */
GLuint fl_framebuffer_get_texture_id(FlFramebuffer* framebuffer);

/**
 * fl_framebuffer_get_width:
 * @framebuffer: an #FlFramebuffer.
 *
 * Gets the width of the framebuffer in pixels.
 *
 * Returns: width in pixels.
 */
size_t fl_framebuffer_get_width(FlFramebuffer* framebuffer);

/**
 * fl_framebuffer_get_height:
 * @framebuffer: an #FlFramebuffer.
 *
 * Gets the height of the framebuffer in pixels.
 *
 * Returns: height in pixels.
 */
size_t fl_framebuffer_get_height(FlFramebuffer* framebuffer);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_FRAMEBUFFER_H_
