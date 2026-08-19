// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fl_framebuffer.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include "flutter/shell/platform/linux/fl_egl_image.h"

// How multisample content reaches the backing texture.
typedef enum {
  // Not multisampled; the texture is the colour attachment.
  kFlFramebufferResolveNone,
  // GL_EXT_multisampled_render_to_texture. The driver rasterizes at the
  // requested sample count into storage it owns and resolves into the texture
  // whenever the framebuffer is read, so consumers of the texture need no
  // extra step.
  kFlFramebufferResolveImplicit,
  // Multisample renderbuffers plus a second framebuffer that owns the texture.
  // fl_framebuffer_resolve() blits between the two.
  kFlFramebufferResolveExplicit,
} FlFramebufferResolve;

struct _FlFramebuffer {
  GObject parent_instance;

  // Width of framebuffer in pixels.
  size_t width;

  // Height of framebuffer in pixels.
  size_t height;

  // Framebuffer ID. Flutter renders into this one.
  GLuint framebuffer_id;

  // Texture backing framebuffer. Always single sample; multisample content
  // reaches it by implicit or explicit resolve.
  GLuint texture_id;

  // Stencil buffer associated with this framebuffer.
  GLuint depth_stencil;

  // Multisample colour renderbuffer. Only used when resolving explicitly.
  GLuint multisample_color;

  // Framebuffer whose colour attachment is #texture_id. Only differs from
  // #framebuffer_id when resolving explicitly.
  GLuint resolve_framebuffer_id;

  // Samples this framebuffer rasterizes at; 1 when not multisampled.
  GLsizei samples;

  // How #texture_id receives the rendered content.
  FlFramebufferResolve resolve;

  // Unsized and sized colour format #texture_id was created with. An explicit
  // resolve constrains these, so they are not necessarily what the caller
  // asked for.
  GLint format;
  GLint sized_format;

  // EGL image for this texture.
  FlEGLImage* image;
};

G_DEFINE_TYPE(FlFramebuffer, fl_framebuffer, G_TYPE_OBJECT)

// Checks if the driver can resolve multisample content into the texture
// itself, which needs neither a second framebuffer nor a blit.
static gboolean has_implicit_multisample() {
  return epoxy_has_gl_extension("GL_EXT_multisampled_render_to_texture");
}

// Checks if the driver has everything the explicit resolve needs: sized
// renderbuffer storage, a multisample variant of it, and the framebuffer blit
// that resolves one into the other.
static gboolean has_explicit_multisample() {
  if (epoxy_gl_version() >= 30) {
    return TRUE;
  }

  gboolean has_sized_storage = epoxy_has_gl_extension("GL_OES_rgb8_rgba8");
  gboolean has_multisample_storage =
      epoxy_has_gl_extension("GL_EXT_framebuffer_multisample") ||
      epoxy_has_gl_extension("GL_ANGLE_framebuffer_multisample") ||
      epoxy_has_gl_extension("GL_NV_framebuffer_multisample");
  gboolean has_blit = epoxy_has_gl_extension("GL_EXT_framebuffer_blit") ||
                      epoxy_has_gl_extension("GL_ANGLE_framebuffer_blit") ||
                      epoxy_has_gl_extension("GL_NV_framebuffer_blit");

  return has_sized_storage && has_multisample_storage && has_blit;
}

// Returns the number of samples usable for @requested, or 1 if this driver
// cannot multisample a framebuffer at all.
static GLsizei negotiate_samples(GLsizei requested) {
  if (requested <= 1 ||
      (!has_implicit_multisample() && !has_explicit_multisample())) {
    return 1;
  }

  // Every extension gated above also accepts this query; GL_MAX_SAMPLES_EXT
  // and GL_MAX_SAMPLES_NV share its value.
  GLint max_samples = 0;
  glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
  if (max_samples < 2) {
    return 1;
  }

  return requested < max_samples ? requested : max_samples;
}

// Returns the sized counterpart of an unsized colour format. This is what the
// embedder ABI reports and what renderbuffer storage requires.
static GLint to_sized_format(GLint format) {
  switch (format) {
    case GL_BGRA_EXT:
      return GL_BGRA8_EXT;
    case GL_RGB:
      return GL_RGB8;
    default:
      return GL_RGBA8;
  }
}

// Attaches single sample colour, depth and stencil to #framebuffer_id.
static void attach_single_sample(FlFramebuffer* self) {
  self->samples = 1;
  self->resolve = kFlFramebufferResolveNone;
  self->resolve_framebuffer_id = self->framebuffer_id;

  glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffer_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         self->texture_id, 0);

  glGenRenderbuffers(1, &self->depth_stencil);
  glBindRenderbuffer(GL_RENDERBUFFER, self->depth_stencil);
  glRenderbufferStorage(GL_RENDERBUFFER,      // target
                        GL_DEPTH24_STENCIL8,  // internal format
                        self->width,          // width
                        self->height          // height
  );
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, self->depth_stencil);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER, self->depth_stencil);
}

// Attaches multisample colour, depth and stencil to #framebuffer_id, using
// whichever resolve #self was configured for. Returns %FALSE if the driver
// will not complete the resulting framebuffer.
static gboolean attach_multisample(FlFramebuffer* self) {
  self->resolve_framebuffer_id = self->framebuffer_id;
  glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffer_id);

  if (self->resolve == kFlFramebufferResolveImplicit) {
    glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                         GL_TEXTURE_2D, self->texture_id, 0,
                                         self->samples);

    glGenRenderbuffers(1, &self->depth_stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, self->depth_stencil);
    // BEWARE: this entry point comes from
    // GL_EXT_multisampled_render_to_texture and is not interchangeable with
    // glRenderbufferStorageMultisample used below.
    glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER,      // target
                                        self->samples,        // samples
                                        GL_DEPTH24_STENCIL8,  // internal format
                                        self->width,          // width
                                        self->height          // height
    );
  } else {
    // GL_RGBA8 by construction: the texture this resolves into was forced to
    // the same format, because a multisample resolve cannot convert.
    glGenRenderbuffers(1, &self->multisample_color);
    glBindRenderbuffer(GL_RENDERBUFFER, self->multisample_color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER,     // target
                                     self->samples,       // samples
                                     self->sized_format,  // internal format
                                     self->width,         // width
                                     self->height         // height
    );
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, self->multisample_color);

    glGenRenderbuffers(1, &self->depth_stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, self->depth_stencil);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER,      // target
                                     self->samples,        // samples
                                     GL_DEPTH24_STENCIL8,  // internal format
                                     self->width,          // width
                                     self->height          // height
    );
  }

  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, self->depth_stencil);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER, self->depth_stencil);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    return FALSE;
  }

  if (self->resolve == kFlFramebufferResolveExplicit) {
    glGenFramebuffers(1, &self->resolve_framebuffer_id);
    glBindFramebuffer(GL_FRAMEBUFFER, self->resolve_framebuffer_id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           self->texture_id, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      return FALSE;
    }
    // Leave the framebuffer Flutter renders into bound, as the single sample
    // path does.
    glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffer_id);
  }

  return TRUE;
}

// Releases everything attach_multisample() may have created, leaving only the
// framebuffer and its texture.
static void discard_multisample(FlFramebuffer* self) {
  if (self->depth_stencil != 0) {
    glDeleteRenderbuffers(1, &self->depth_stencil);
    self->depth_stencil = 0;
  }
  if (self->multisample_color != 0) {
    glDeleteRenderbuffers(1, &self->multisample_color);
    self->multisample_color = 0;
  }
  if (self->resolve_framebuffer_id != 0 &&
      self->resolve_framebuffer_id != self->framebuffer_id) {
    glDeleteFramebuffers(1, &self->resolve_framebuffer_id);
  }
  self->resolve_framebuffer_id = 0;
}

// Performs the resolve once and reports whether the driver accepted it.
// Whether an unsized texture format counts as identical to the renderbuffer's
// sized one is a driver judgement, and a rejected blit is silent, so ask
// instead of assuming.
static gboolean resolve_is_accepted(FlFramebuffer* self) {
  if (self->resolve != kFlFramebufferResolveExplicit) {
    return TRUE;
  }

  // Drain errors this framebuffer did not cause, bounded in case a lost
  // context never reports GL_NO_ERROR.
  for (int i = 0; i < 16; i++) {
    if (glGetError() == GL_NO_ERROR) {
      break;
    }
  }

  fl_framebuffer_resolve(self);

  return glGetError() == GL_NO_ERROR;
}

static void fl_framebuffer_dispose(GObject* object) {
  FlFramebuffer* self = FL_FRAMEBUFFER(object);

  glDeleteFramebuffers(1, &self->framebuffer_id);
  if (self->resolve_framebuffer_id != 0 &&
      self->resolve_framebuffer_id != self->framebuffer_id) {
    glDeleteFramebuffers(1, &self->resolve_framebuffer_id);
  }
  glDeleteTextures(1, &self->texture_id);
  glDeleteRenderbuffers(1, &self->depth_stencil);
  if (self->multisample_color != 0) {
    glDeleteRenderbuffers(1, &self->multisample_color);
  }
  g_clear_object(&self->image);

  G_OBJECT_CLASS(fl_framebuffer_parent_class)->dispose(object);
}

static void fl_framebuffer_class_init(FlFramebufferClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_framebuffer_dispose;
}

static void fl_framebuffer_init(FlFramebuffer* self) {
  self->samples = 1;
  self->resolve = kFlFramebufferResolveNone;
}

FlFramebuffer* fl_framebuffer_new(GLint format,
                                  size_t width,
                                  size_t height,
                                  gboolean shareable) {
  return fl_framebuffer_new_multisampled(format, width, height, shareable, 1);
}

FlFramebuffer* fl_framebuffer_new_multisampled(GLint format,
                                               size_t width,
                                               size_t height,
                                               gboolean shareable,
                                               GLsizei samples) {
  FlFramebuffer* self =
      FL_FRAMEBUFFER(g_object_new(fl_framebuffer_get_type(), nullptr));

  self->width = width;
  self->height = height;

  // Both the resolve and the format follow from the sample count, so settle
  // them before anything is allocated.
  self->samples = negotiate_samples(samples);
  if (self->samples > 1) {
    self->resolve = has_implicit_multisample() ? kFlFramebufferResolveImplicit
                                               : kFlFramebufferResolveExplicit;
  }

  if (self->resolve == kFlFramebufferResolveExplicit) {
    // Resolving a multisample framebuffer requires the read and draw buffers
    // to have identical formats (OpenGL ES 3.0 specification, section 4.3.2),
    // and GL_RGBA8 is the only colour format portable across the multisample
    // renderbuffer storage entry points. So the texture becomes RGBA8 too.
    // Byte order only matters to a CPU reader, and this framebuffer has none:
    // the compositor's presentation framebuffer, which glReadPixels does read,
    // is single sample and keeps its caller's format. Callers must take the
    // format actually used from fl_framebuffer_get_sized_format().
    self->format = GL_RGBA;
    self->sized_format = GL_RGBA8;
  } else {
    self->format = format;
    self->sized_format = to_sized_format(format);
  }

  glGenTextures(1, &self->texture_id);
  glGenFramebuffers(1, &self->framebuffer_id);

  glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffer_id);

  glBindTexture(GL_TEXTURE_2D, self->texture_id);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // Name the sized format where the resolve depends on matching it exactly.
  // Sized internal formats need OpenGL (ES) 3.0; below that an unsized
  // GL_RGBA with GL_UNSIGNED_BYTE is defined to be RGBA8 anyway, and
  // resolve_is_accepted() catches a driver that disagrees.
  GLint internal_format =
      self->resolve == kFlFramebufferResolveExplicit && epoxy_gl_version() >= 30
          ? self->sized_format
          : self->format;
  glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0,
               self->format, GL_UNSIGNED_BYTE, NULL);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (shareable) {
    self->image = fl_egl_image_new(self->texture_id);
  }

  if (self->samples > 1) {
    if (!attach_multisample(self) || !resolve_is_accepted(self)) {
      // A driver that advertises the extensions but will not complete the
      // framebuffer, or will not perform the resolve, still has to render, so
      // give up the antialiasing rather than the frame. The texture keeps the
      // format the resolve asked for; fl_framebuffer_get_sized_format() stays
      // the single answer either way.
      discard_multisample(self);
      attach_single_sample(self);
    }
  } else {
    attach_single_sample(self);
  }

  return self;
}

gboolean fl_framebuffer_get_shareable(FlFramebuffer* self) {
  g_return_val_if_fail(FL_IS_FRAMEBUFFER(self), FALSE);
  return self->image != nullptr;
}

FlFramebuffer* fl_framebuffer_create_sibling(FlFramebuffer* self) {
  g_return_val_if_fail(FL_IS_FRAMEBUFFER(self), nullptr);
  g_return_val_if_fail(self->image != nullptr, nullptr);

  FlFramebuffer* sibling =
      FL_FRAMEBUFFER(g_object_new(fl_framebuffer_get_type(), nullptr));

  sibling->width = self->width;
  sibling->height = self->height;
  sibling->format = self->format;
  sibling->sized_format = self->sized_format;
  sibling->image = FL_EGL_IMAGE(g_object_ref(self->image));

  // Make texture from existing image.
  glGenTextures(1, &sibling->texture_id);
  glBindTexture(GL_TEXTURE_2D, sibling->texture_id);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,
                               fl_egl_image_get_image(self->image));

  // Make framebuffer that uses this texture. The image is already resolved, so
  // the sibling is always single sample.
  glGenFramebuffers(1, &sibling->framebuffer_id);
  sibling->resolve_framebuffer_id = sibling->framebuffer_id;
  GLint saved_framebuffer_binding;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_framebuffer_binding);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sibling->framebuffer_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         sibling->texture_id, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, saved_framebuffer_binding);

  return sibling;
}

void fl_framebuffer_resolve(FlFramebuffer* self) {
  g_return_if_fail(FL_IS_FRAMEBUFFER(self));

  if (self->resolve != kFlFramebufferResolveExplicit) {
    return;
  }

  GLint saved_draw_framebuffer_binding;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw_framebuffer_binding);
  GLint saved_read_framebuffer_binding;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read_framebuffer_binding);
  // The scissor test clips blit operations.
  // See OpenGL specification version 4.6, section 18.3.1.
  GLboolean saved_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, self->framebuffer_id);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, self->resolve_framebuffer_id);
  glBlitFramebuffer(0, 0, self->width, self->height, 0, 0, self->width,
                    self->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  if (saved_scissor_test) {
    glEnable(GL_SCISSOR_TEST);
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, saved_draw_framebuffer_binding);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, saved_read_framebuffer_binding);
}

GLuint fl_framebuffer_get_id(FlFramebuffer* self) {
  return self->framebuffer_id;
}

GLuint fl_framebuffer_get_resolved_id(FlFramebuffer* self) {
  return self->resolve_framebuffer_id;
}

GLuint fl_framebuffer_get_texture_id(FlFramebuffer* self) {
  return self->texture_id;
}

GLsizei fl_framebuffer_get_samples(FlFramebuffer* self) {
  return self->samples;
}

GLint fl_framebuffer_get_sized_format(FlFramebuffer* self) {
  return self->sized_format;
}

size_t fl_framebuffer_get_width(FlFramebuffer* self) {
  return self->width;
}

size_t fl_framebuffer_get_height(FlFramebuffer* self) {
  return self->height;
}
