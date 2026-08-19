// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/testing/linux_test.h"
#include "gtest/gtest.h"

#include "flutter/shell/platform/linux/fl_framebuffer.h"
#include "flutter/shell/platform/linux/testing/mock_epoxy.h"

class FlFramebufferTest : public flutter::testing::LinuxTest {
 protected:
  ::testing::NiceMock<flutter::testing::MockEpoxy> epoxy;
};

TEST_F(FlFramebufferTest, HasDepthStencil) {
  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new(GL_RGB, 100, 100, FALSE);

  GLint depth_type = GL_NONE;
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                        &depth_type);
  EXPECT_NE(depth_type, GL_NONE);

  GLint stencil_type = GL_NONE;
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                        &stencil_type);
  EXPECT_NE(stencil_type, GL_NONE);
}

TEST_F(FlFramebufferTest, ResourcesRemoved) {
  EXPECT_CALL(epoxy, glGenFramebuffers);
  EXPECT_CALL(epoxy, glGenTextures);
  EXPECT_CALL(epoxy, glGenRenderbuffers);
  FlFramebuffer* framebuffer = fl_framebuffer_new(GL_RGB, 100, 100, FALSE);

  EXPECT_CALL(epoxy, glDeleteFramebuffers);
  EXPECT_CALL(epoxy, glDeleteTextures);
  EXPECT_CALL(epoxy, glDeleteRenderbuffers);
  g_object_unref(framebuffer);
}

TEST_F(FlFramebufferTest, Sibling) {
  EXPECT_CALL(epoxy, eglCreateImageKHR);
  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new(GL_RGB, 100, 100, TRUE);
  g_autoptr(FlFramebuffer) sibling = fl_framebuffer_create_sibling(framebuffer);
}

// Reports the driver as supporting @extension and nothing else.
static void only_extension(::testing::NiceMock<flutter::testing::MockEpoxy>& e,
                           const char* extension) {
  EXPECT_CALL(e, epoxy_gl_version).WillRepeatedly(::testing::Return(20));
  EXPECT_CALL(e, epoxy_has_gl_extension(::testing::_))
      .WillRepeatedly(::testing::Return(false));
  EXPECT_CALL(e, epoxy_has_gl_extension(::testing::StrEq(extension)))
      .WillRepeatedly(::testing::Return(true));
  EXPECT_CALL(e, glCheckFramebufferStatus)
      .WillRepeatedly(::testing::Return(GL_FRAMEBUFFER_COMPLETE));
}

TEST_F(FlFramebufferTest, SingleSampleByDefault) {
  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new(GL_RGB, 100, 100, FALSE);

  EXPECT_EQ(fl_framebuffer_get_samples(framebuffer), 1);
  EXPECT_EQ(fl_framebuffer_get_resolved_id(framebuffer),
            fl_framebuffer_get_id(framebuffer));
}

TEST_F(FlFramebufferTest, NoMultisampleWithoutDriverSupport) {
  EXPECT_CALL(epoxy, epoxy_gl_version).WillRepeatedly(::testing::Return(20));
  EXPECT_CALL(epoxy, epoxy_has_gl_extension(::testing::_))
      .WillRepeatedly(::testing::Return(false));

  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new_multisampled(GL_RGB, 100, 100, FALSE, 4);

  EXPECT_EQ(fl_framebuffer_get_samples(framebuffer), 1);
}

TEST_F(FlFramebufferTest, ImplicitMultisampleNeedsNoResolve) {
  only_extension(epoxy, "GL_EXT_multisampled_render_to_texture");

  EXPECT_CALL(epoxy, glFramebufferTexture2DMultisampleEXT(
                         GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         ::testing::_, 0, 4));
  EXPECT_CALL(epoxy, glRenderbufferStorageMultisampleEXT(
                         GL_RENDERBUFFER, 4, GL_DEPTH24_STENCIL8, 100, 100));
  // The driver resolves into the texture, so nothing is blitted.
  EXPECT_CALL(epoxy, glBlitFramebuffer).Times(0);

  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new_multisampled(GL_RGB, 100, 100, FALSE, 4);

  EXPECT_EQ(fl_framebuffer_get_samples(framebuffer), 4);
  EXPECT_EQ(fl_framebuffer_get_resolved_id(framebuffer),
            fl_framebuffer_get_id(framebuffer));

  fl_framebuffer_resolve(framebuffer);
}

TEST_F(FlFramebufferTest, ExplicitMultisampleResolvesWithABlit) {
  EXPECT_CALL(epoxy, epoxy_gl_version).WillRepeatedly(::testing::Return(30));
  EXPECT_CALL(epoxy, epoxy_has_gl_extension(::testing::_))
      .WillRepeatedly(::testing::Return(false));
  EXPECT_CALL(epoxy, glCheckFramebufferStatus)
      .WillRepeatedly(::testing::Return(GL_FRAMEBUFFER_COMPLETE));

  // Colour and depth-stencil both need multisample storage.
  EXPECT_CALL(epoxy, glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4,
                                                      GL_RGBA8, 100, 100));
  EXPECT_CALL(epoxy, glRenderbufferStorageMultisample(
                         GL_RENDERBUFFER, 4, GL_DEPTH24_STENCIL8, 100, 100));
  EXPECT_CALL(epoxy, glBlitFramebuffer(0, 0, 100, 100, 0, 0, 100, 100,
                                       GL_COLOR_BUFFER_BIT, GL_NEAREST));

  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new_multisampled(GL_RGB, 100, 100, FALSE, 4);

  EXPECT_EQ(fl_framebuffer_get_samples(framebuffer), 4);

  fl_framebuffer_resolve(framebuffer);
}

TEST_F(FlFramebufferTest, SamplesClampedToDriverMaximum) {
  only_extension(epoxy, "GL_EXT_multisampled_render_to_texture");

  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new_multisampled(GL_RGB, 100, 100, FALSE, 16);

  EXPECT_EQ(fl_framebuffer_get_samples(framebuffer), 4);
}

TEST_F(FlFramebufferTest, IncompleteMultisampleFallsBackToSingleSample) {
  EXPECT_CALL(epoxy, epoxy_gl_version).WillRepeatedly(::testing::Return(20));
  EXPECT_CALL(epoxy, epoxy_has_gl_extension(::testing::_))
      .WillRepeatedly(::testing::Return(false));
  EXPECT_CALL(epoxy, epoxy_has_gl_extension(::testing::StrEq(
                         "GL_EXT_multisampled_render_to_texture")))
      .WillRepeatedly(::testing::Return(true));
  EXPECT_CALL(epoxy, glCheckFramebufferStatus)
      .WillRepeatedly(::testing::Return(GL_FRAMEBUFFER_UNSUPPORTED));

  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new_multisampled(GL_RGB, 100, 100, FALSE, 4);

  EXPECT_EQ(fl_framebuffer_get_samples(framebuffer), 1);
  EXPECT_EQ(fl_framebuffer_get_resolved_id(framebuffer),
            fl_framebuffer_get_id(framebuffer));
}
