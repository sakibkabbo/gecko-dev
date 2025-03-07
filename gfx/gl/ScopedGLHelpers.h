/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SCOPEDGLHELPERS_H_
#define SCOPEDGLHELPERS_H_

#include "GLDefs.h"
#include "mozilla/UniquePtr.h"

namespace mozilla {
namespace gl {

class GLContext;

#ifdef DEBUG
bool IsContextCurrent(GLContext* gl);
#endif

// RAII via CRTP!
template <class Derived>
struct ScopedGLWrapper {
 private:
  bool mIsUnwrapped;

 protected:
  GLContext* const mGL;

  explicit ScopedGLWrapper(GLContext* gl) : mIsUnwrapped(false), mGL(gl) {
    MOZ_ASSERT(&ScopedGLWrapper<Derived>::Unwrap == &Derived::Unwrap);
    MOZ_ASSERT(&Derived::UnwrapImpl);
  }

  virtual ~ScopedGLWrapper() {
    if (!mIsUnwrapped) Unwrap();
  }

 public:
  void Unwrap() {
    MOZ_ASSERT(!mIsUnwrapped);

    Derived* derived = static_cast<Derived*>(this);
    derived->UnwrapImpl();

    mIsUnwrapped = true;
  }
};

// Wraps glEnable/Disable.
struct ScopedGLState : public ScopedGLWrapper<ScopedGLState> {
  friend struct ScopedGLWrapper<ScopedGLState>;

 protected:
  const GLenum mCapability;
  bool mOldState;

 public:
  // Use |newState = true| to enable, |false| to disable.
  ScopedGLState(GLContext* aGL, GLenum aCapability, bool aNewState);
  // variant that doesn't change state; simply records existing state to be
  // restored by the destructor
  ScopedGLState(GLContext* aGL, GLenum aCapability);

 protected:
  void UnwrapImpl();
};

// Saves and restores with GetUserBoundFB and BindUserFB.
struct ScopedBindFramebuffer : public ScopedGLWrapper<ScopedBindFramebuffer> {
  friend struct ScopedGLWrapper<ScopedBindFramebuffer>;

 protected:
  GLuint mOldReadFB;
  GLuint mOldDrawFB;

 private:
  void Init();

 public:
  explicit ScopedBindFramebuffer(GLContext* aGL);
  ScopedBindFramebuffer(GLContext* aGL, GLuint aNewFB);

 protected:
  void UnwrapImpl();
};

struct ScopedBindTextureUnit : public ScopedGLWrapper<ScopedBindTextureUnit> {
  friend struct ScopedGLWrapper<ScopedBindTextureUnit>;

 protected:
  GLenum mOldTexUnit;

 public:
  ScopedBindTextureUnit(GLContext* aGL, GLenum aTexUnit);

 protected:
  void UnwrapImpl();
};

struct ScopedTexture : public ScopedGLWrapper<ScopedTexture> {
  friend struct ScopedGLWrapper<ScopedTexture>;

 protected:
  GLuint mTexture;

 public:
  explicit ScopedTexture(GLContext* aGL);

  GLuint Texture() const { return mTexture; }
  operator GLuint() const { return mTexture; }

 protected:
  void UnwrapImpl();
};

struct ScopedFramebuffer : public ScopedGLWrapper<ScopedFramebuffer> {
  friend struct ScopedGLWrapper<ScopedFramebuffer>;

 protected:
  GLuint mFB;

 public:
  explicit ScopedFramebuffer(GLContext* aGL);
  GLuint FB() { return mFB; }

 protected:
  void UnwrapImpl();
};

struct ScopedRenderbuffer : public ScopedGLWrapper<ScopedRenderbuffer> {
  friend struct ScopedGLWrapper<ScopedRenderbuffer>;

 protected:
  GLuint mRB;

 public:
  explicit ScopedRenderbuffer(GLContext* aGL);
  GLuint RB() { return mRB; }

 protected:
  void UnwrapImpl();
};

struct ScopedBindTexture : public ScopedGLWrapper<ScopedBindTexture> {
  friend struct ScopedGLWrapper<ScopedBindTexture>;

 protected:
  const GLenum mTarget;
  const GLuint mOldTex;

 public:
  ScopedBindTexture(GLContext* aGL, GLuint aNewTex,
                    GLenum aTarget = LOCAL_GL_TEXTURE_2D);

 protected:
  void UnwrapImpl();
};

struct ScopedBindRenderbuffer : public ScopedGLWrapper<ScopedBindRenderbuffer> {
  friend struct ScopedGLWrapper<ScopedBindRenderbuffer>;

 protected:
  GLuint mOldRB;

 private:
  void Init();

 public:
  explicit ScopedBindRenderbuffer(GLContext* aGL);

  ScopedBindRenderbuffer(GLContext* aGL, GLuint aNewRB);

 protected:
  void UnwrapImpl();
};

struct ScopedFramebufferForTexture
    : public ScopedGLWrapper<ScopedFramebufferForTexture> {
  friend struct ScopedGLWrapper<ScopedFramebufferForTexture>;

 protected:
  bool mComplete;  // True if the framebuffer we create is complete.
  GLuint mFB;

 public:
  ScopedFramebufferForTexture(GLContext* aGL, GLuint aTexture,
                              GLenum aTarget = LOCAL_GL_TEXTURE_2D);

  bool IsComplete() const { return mComplete; }

  GLuint FB() const {
    MOZ_GL_ASSERT(mGL, IsComplete());
    return mFB;
  }

 protected:
  void UnwrapImpl();
};

struct ScopedFramebufferForRenderbuffer
    : public ScopedGLWrapper<ScopedFramebufferForRenderbuffer> {
  friend struct ScopedGLWrapper<ScopedFramebufferForRenderbuffer>;

 protected:
  bool mComplete;  // True if the framebuffer we create is complete.
  GLuint mFB;

 public:
  ScopedFramebufferForRenderbuffer(GLContext* aGL, GLuint aRB);

  bool IsComplete() const { return mComplete; }

  GLuint FB() const { return mFB; }

 protected:
  void UnwrapImpl();
};

struct ScopedViewportRect : public ScopedGLWrapper<ScopedViewportRect> {
  friend struct ScopedGLWrapper<ScopedViewportRect>;

 protected:
  GLint mSavedViewportRect[4];

 public:
  ScopedViewportRect(GLContext* aGL, GLint x, GLint y, GLsizei width,
                     GLsizei height);

 protected:
  void UnwrapImpl();
};

struct ScopedScissorRect : public ScopedGLWrapper<ScopedScissorRect> {
  friend struct ScopedGLWrapper<ScopedScissorRect>;

 protected:
  GLint mSavedScissorRect[4];

 public:
  ScopedScissorRect(GLContext* aGL, GLint x, GLint y, GLsizei width,
                    GLsizei height);
  explicit ScopedScissorRect(GLContext* aGL);

 protected:
  void UnwrapImpl();
};

struct ScopedVertexAttribPointer
    : public ScopedGLWrapper<ScopedVertexAttribPointer> {
  friend struct ScopedGLWrapper<ScopedVertexAttribPointer>;

 protected:
  GLuint mAttribIndex;
  GLint mAttribEnabled;
  GLint mAttribSize;
  GLint mAttribStride;
  GLint mAttribType;
  GLint mAttribNormalized;
  GLint mAttribBufferBinding;
  void* mAttribPointer;
  GLuint mBoundBuffer;

 public:
  ScopedVertexAttribPointer(GLContext* aGL, GLuint index, GLint size,
                            GLenum type, realGLboolean normalized,
                            GLsizei stride, GLuint buffer,
                            const GLvoid* pointer);
  explicit ScopedVertexAttribPointer(GLContext* aGL, GLuint index);

 protected:
  void WrapImpl(GLuint index);
  void UnwrapImpl();
};

struct ScopedPackState : public ScopedGLWrapper<ScopedPackState> {
  friend struct ScopedGLWrapper<ScopedPackState>;

 protected:
  GLint mAlignment;

  GLuint mPixelBuffer;
  GLint mRowLength;
  GLint mSkipPixels;
  GLint mSkipRows;

 public:
  explicit ScopedPackState(GLContext* gl);

 protected:
  void UnwrapImpl();
};

struct ResetUnpackState : public ScopedGLWrapper<ResetUnpackState> {
  friend struct ScopedGLWrapper<ResetUnpackState>;

 protected:
  GLuint mAlignment;

  GLuint mPBO;
  GLuint mRowLength;
  GLuint mImageHeight;
  GLuint mSkipPixels;
  GLuint mSkipRows;
  GLuint mSkipImages;

 public:
  explicit ResetUnpackState(GLContext* gl);

 protected:
  void UnwrapImpl();
};

struct ScopedBindPBO final : public ScopedGLWrapper<ScopedBindPBO> {
  friend struct ScopedGLWrapper<ScopedBindPBO>;

 protected:
  const GLenum mTarget;
  const GLuint mPBO;

 public:
  ScopedBindPBO(GLContext* gl, GLenum target);

 protected:
  void UnwrapImpl();
};

} /* namespace gl */
} /* namespace mozilla */

#endif /* SCOPEDGLHELPERS_H_ */
