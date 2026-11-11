// OpenGL ES 1.x / 2.0. GL functions only take scalars and pointers into guest
// memory, so nearly all of them are forwarded with automatic marshalling.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "thunk.h"

#ifdef __ANDROID__
#include <GLES/gl.h>
#include <GLES2/gl2.h>
#include <vector>
#endif

namespace a64b {

void register_gles_thunks() {
#ifdef __ANDROID__
#define G(name) reg(#name, &::name);
    // ---- OpenGL ES 2.0 ----
    G(glActiveTexture) G(glAttachShader) G(glBindAttribLocation) G(glBindBuffer) G(glBindFramebuffer)
    G(glBindRenderbuffer) G(glBindTexture) G(glBlendColor) G(glBlendEquation) G(glBlendEquationSeparate)
    G(glBlendFunc) G(glBlendFuncSeparate) G(glBufferData) G(glBufferSubData) G(glCheckFramebufferStatus)
    G(glClear) G(glClearColor) G(glClearDepthf) G(glClearStencil) G(glColorMask) G(glCompileShader)
    G(glCompressedTexImage2D) G(glCompressedTexSubImage2D) G(glCopyTexImage2D) G(glCopyTexSubImage2D)
    G(glCreateProgram) G(glCreateShader) G(glCullFace) G(glDeleteBuffers) G(glDeleteFramebuffers)
    G(glDeleteProgram) G(glDeleteRenderbuffers) G(glDeleteShader) G(glDeleteTextures) G(glDepthFunc)
    G(glDepthMask) G(glDepthRangef) G(glDetachShader) G(glDisable) G(glDisableVertexAttribArray)
    G(glDrawArrays) G(glDrawElements) G(glEnable) G(glEnableVertexAttribArray) G(glFinish) G(glFlush)
    G(glFramebufferRenderbuffer) G(glFramebufferTexture2D) G(glFrontFace) G(glGenBuffers) G(glGenerateMipmap)
    G(glGenFramebuffers) G(glGenRenderbuffers) G(glGenTextures) G(glGetActiveAttrib) G(glGetActiveUniform)
    G(glGetAttachedShaders) G(glGetAttribLocation) G(glGetBooleanv) G(glGetBufferParameteriv) G(glGetError)
    G(glGetFloatv) G(glGetFramebufferAttachmentParameteriv) G(glGetIntegerv) G(glGetProgramiv)
    G(glGetProgramInfoLog) G(glGetRenderbufferParameteriv) G(glGetShaderiv) G(glGetShaderInfoLog)
    G(glGetShaderPrecisionFormat) G(glGetShaderSource) G(glGetTexParameterfv) G(glGetTexParameteriv)
    G(glGetUniformfv) G(glGetUniformiv) G(glGetUniformLocation) G(glGetVertexAttribfv) G(glGetVertexAttribiv)
    G(glHint) G(glIsBuffer) G(glIsEnabled) G(glIsFramebuffer) G(glIsProgram) G(glIsRenderbuffer) G(glIsShader)
    G(glIsTexture) G(glLineWidth) G(glLinkProgram) G(glPixelStorei) G(glPolygonOffset) G(glReadPixels)
    G(glReleaseShaderCompiler) G(glRenderbufferStorage) G(glSampleCoverage) G(glScissor) G(glShaderBinary)
    G(glStencilFunc) G(glStencilFuncSeparate) G(glStencilMask) G(glStencilMaskSeparate) G(glStencilOp)
    G(glStencilOpSeparate) G(glTexImage2D) G(glTexParameterf) G(glTexParameterfv) G(glTexParameteri)
    G(glTexParameteriv) G(glTexSubImage2D) G(glUniform1f) G(glUniform1fv) G(glUniform1i) G(glUniform1iv)
    G(glUniform2f) G(glUniform2fv) G(glUniform2i) G(glUniform2iv) G(glUniform3f) G(glUniform3fv) G(glUniform3i)
    G(glUniform3iv) G(glUniform4f) G(glUniform4fv) G(glUniform4i) G(glUniform4iv) G(glUniformMatrix2fv)
    G(glUniformMatrix3fv) G(glUniformMatrix4fv) G(glUseProgram) G(glValidateProgram) G(glVertexAttrib1f)
    G(glVertexAttrib1fv) G(glVertexAttrib2f) G(glVertexAttrib2fv) G(glVertexAttrib3f) G(glVertexAttrib3fv)
    G(glVertexAttrib4f) G(glVertexAttrib4fv) G(glVertexAttribPointer) G(glViewport)
    // ---- OpenGL ES 1.x fixed-function additions ----
    G(glAlphaFunc) G(glClientActiveTexture) G(glColor4f) G(glColor4ub) G(glColorPointer) G(glDisableClientState)
    G(glEnableClientState) G(glFogf) G(glFogfv) G(glFrustumf) G(glLightf) G(glLightfv) G(glLightModelf)
    G(glLightModelfv) G(glLoadIdentity) G(glLoadMatrixf) G(glMaterialf) G(glMaterialfv) G(glMatrixMode)
    G(glMultMatrixf) G(glMultiTexCoord4f) G(glNormal3f) G(glNormalPointer) G(glOrthof) G(glPointSize)
    G(glPopMatrix) G(glPushMatrix) G(glRotatef) G(glScalef) G(glShadeModel) G(glTexCoordPointer) G(glTexEnvf)
    G(glTexEnvfv) G(glTexEnvi) G(glTranslatef) G(glVertexPointer)
#undef G
    // Functions that return or take host-sized pointers need manual handling.
    reg("glGetString", +[](GLenum name) -> u32 { return guest_intern_string((const char*)glGetString(name)); });
    reg("glShaderSource", +[](GLuint shader, GLsizei count, const u32* strings, const GLint* lengths) {
        std::vector<const GLchar*> ptrs((size_t)(count > 0 ? count : 0));
        for (GLsizei i = 0; i < count; i++) ptrs[i] = g2h<GLchar>(strings[i]);
        glShaderSource(shader, count, ptrs.data(), lengths);
    });
    reg("glGetVertexAttribPointerv", +[](GLuint index, GLenum pname, u32* out) {
        void* p = nullptr;
        glGetVertexAttribPointerv(index, pname, &p);
        *out = (u32)(uintptr_t)p;
    });
#endif
}

}  // namespace a64b
