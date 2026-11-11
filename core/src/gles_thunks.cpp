// OpenGL ES 1.x / 2.0. GL functions only take scalars and pointers into guest
// memory, so nearly all of them are forwarded with automatic marshalling.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "thunk.h"

#ifdef __ANDROID__
#define GL_GLEXT_PROTOTYPES 1  // declare the extension functions in glext.h / gl2ext.h
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <mutex>
#include <unordered_map>
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
    // ---- OES framebuffer objects (OpenGL ES 1.x extension) ----
    G(glGenRenderbuffersOES) G(glDeleteRenderbuffersOES) G(glBindRenderbufferOES) G(glRenderbufferStorageOES)
    G(glGenFramebuffersOES) G(glDeleteFramebuffersOES) G(glBindFramebufferOES) G(glFramebufferTexture2DOES)
    G(glFramebufferRenderbufferOES) G(glCheckFramebufferStatusOES) G(glGenerateMipmapOES) G(glIsFramebufferOES)
    G(glIsRenderbufferOES) G(glGetRenderbufferParameterivOES) G(glGetFramebufferAttachmentParameterivOES)
    G(glBlendEquationOES) G(glBlendFuncSeparateOES) G(glBlendEquationSeparateOES) G(glDrawTexiOES) G(glDrawTexfOES)
#undef G
    // Functions that return or take host-sized pointers need manual handling.
    reg("glGetString", +[](GLenum name) -> u32 { return guest_intern_string((const char*)glGetString(name)); });
    reg("glShaderSource", +[](GLuint shader, GLsizei count, const u32* strings, const GLint* lengths) {
        std::vector<const GLchar*> ptrs((size_t)(count > 0 ? count : 0));
        for (GLsizei i = 0; i < count; i++) ptrs[i] = g2h<GLchar>(strings[i]);
        glShaderSource(shader, count, ptrs.data(), lengths);
    });
    // GL_OES_mapbuffer: the driver's mapping is outside guest memory, so the guest writes into a
    // shadow buffer that is uploaded with glBufferSubData when it is unmapped.
    struct Shadow { u32 mem; GLint size; };
    static std::mutex s_map_mu;
    static std::unordered_map<GLenum, Shadow> s_maps;
    reg("glMapBufferOES", +[](GLenum target, GLenum) -> u32 {
        GLint size = 0;
        glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
        if (size <= 0) return 0;
        u32 mem = g_malloc((u32)size);
        if (!mem) return 0;
        std::lock_guard<std::mutex> lk(s_map_mu);
        auto it = s_maps.find(target);
        if (it != s_maps.end()) g_free(it->second.mem);
        s_maps[target] = Shadow{mem, size};
        return mem;
    });
    reg("glUnmapBufferOES", +[](GLenum target) -> u32 {
        Shadow sh{};
        {
            std::lock_guard<std::mutex> lk(s_map_mu);
            auto it = s_maps.find(target);
            if (it == s_maps.end()) return GL_FALSE;
            sh = it->second;
            s_maps.erase(it);
        }
        glBufferSubData(target, 0, sh.size, g2h(sh.mem));
        g_free(sh.mem);
        return GL_TRUE;
    });
    reg("glGetBufferPointervOES", +[](GLenum target, GLenum, u32* out) {
        std::lock_guard<std::mutex> lk(s_map_mu);
        auto it = s_maps.find(target);
        *out = it == s_maps.end() ? 0 : it->second.mem;
    });
    reg("glGetVertexAttribPointerv", +[](GLuint index, GLenum pname, u32* out) {
        void* p = nullptr;
        glGetVertexAttribPointerv(index, pname, &p);
        *out = (u32)(uintptr_t)p;
    });
#endif
}

}  // namespace a64b
