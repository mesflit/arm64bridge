//! OpenGL ES 1.x / 2.0. GL functions only take scalars and pointers into guest
//! memory, so nearly all of them are forwarded with automatic marshalling.
// SPDX-License-Identifier: GPL-2.0-or-later
#![allow(non_snake_case)]

pub fn register_gles_thunks() {
    #[cfg(target_os = "android")]
    gl::register();
}

#[cfg(target_os = "android")]
mod gl {
    use std::collections::HashMap;
    use std::os::raw::{c_char, c_void};
    use std::sync::Mutex;

    use crate::mem::*;
    use crate::thunk::reg;
    use crate::thunks::libc::intern_cstr;
    use crate::util::lock;

    type E = u32; // GLenum, GLbitfield
    type U = u32; // GLuint
    type I = i32; // GLint, GLsizei
    type F = f32; // GLfloat, GLclampf
    type B = u8; // GLboolean, GLubyte
    type L = isize; // GLintptr, GLsizeiptr ("long": 32 bits in the guest)
    type P = *const c_void;
    type S = *const c_char;
    type MS = *mut c_char;

    // Declares host GL functions and registers each one under its own name.
    macro_rules! gl {
        ($lib:literal, $reg:ident: $(fn $name:ident($($a:ident : $t:ty),*) $(-> $r:ty)?;)*) => {
            #[link(name = $lib)]
            extern "C" { $(fn $name($($a: $t),*) $(-> $r)?;)* }
            fn $reg() {
                $( reg(stringify!($name), |$($a: $t),*| $(-> $r)? { unsafe { $name($($a),*) } }); )*
            }
        };
    }

    gl! { "GLESv2", reg_es2:
        fn glActiveTexture(t: E);
        fn glAttachShader(p: U, s: U);
        fn glBindAttribLocation(p: U, i: U, n: S);
        fn glBindBuffer(t: E, b: U);
        fn glBindFramebuffer(t: E, f: U);
        fn glBindRenderbuffer(t: E, r: U);
        fn glBindTexture(t: E, x: U);
        fn glBlendColor(r: F, g: F, b: F, a: F);
        fn glBlendEquation(m: E);
        fn glBlendEquationSeparate(rgb: E, a: E);
        fn glBlendFunc(s: E, d: E);
        fn glBlendFuncSeparate(sr: E, dr: E, sa: E, da: E);
        fn glBufferData(t: E, size: L, data: P, usage: E);
        fn glBufferSubData(t: E, off: L, size: L, data: P);
        fn glCheckFramebufferStatus(t: E) -> E;
        fn glClear(mask: E);
        fn glClearColor(r: F, g: F, b: F, a: F);
        fn glClearDepthf(d: F);
        fn glClearStencil(s: I);
        fn glColorMask(r: B, g: B, b: B, a: B);
        fn glCompileShader(s: U);
        fn glCompressedTexImage2D(t: E, level: I, fmt: E, w: I, h: I, border: I, size: I, data: P);
        fn glCompressedTexSubImage2D(t: E, level: I, x: I, y: I, w: I, h: I, fmt: E, size: I, data: P);
        fn glCopyTexImage2D(t: E, level: I, fmt: E, x: I, y: I, w: I, h: I, border: I);
        fn glCopyTexSubImage2D(t: E, level: I, xo: I, yo: I, x: I, y: I, w: I, h: I);
        fn glCreateProgram() -> U;
        fn glCreateShader(t: E) -> U;
        fn glCullFace(m: E);
        fn glDeleteBuffers(n: I, b: *const U);
        fn glDeleteFramebuffers(n: I, b: *const U);
        fn glDeleteProgram(p: U);
        fn glDeleteRenderbuffers(n: I, b: *const U);
        fn glDeleteShader(s: U);
        fn glDeleteTextures(n: I, b: *const U);
        fn glDepthFunc(f: E);
        fn glDepthMask(f: B);
        fn glDepthRangef(n: F, f: F);
        fn glDetachShader(p: U, s: U);
        fn glDisable(c: E);
        fn glDisableVertexAttribArray(i: U);
        fn glDrawArrays(m: E, first: I, count: I);
        fn glDrawElements(m: E, count: I, t: E, indices: P);
        fn glEnable(c: E);
        fn glEnableVertexAttribArray(i: U);
        fn glFinish();
        fn glFlush();
        fn glFramebufferRenderbuffer(t: E, a: E, rt: E, r: U);
        fn glFramebufferTexture2D(t: E, a: E, tt: E, tex: U, level: I);
        fn glFrontFace(m: E);
        fn glGenBuffers(n: I, b: *mut U);
        fn glGenerateMipmap(t: E);
        fn glGenFramebuffers(n: I, b: *mut U);
        fn glGenRenderbuffers(n: I, b: *mut U);
        fn glGenTextures(n: I, b: *mut U);
        fn glGetActiveAttrib(p: U, i: U, buf: I, len: *mut I, size: *mut I, t: *mut E, name: MS);
        fn glGetActiveUniform(p: U, i: U, buf: I, len: *mut I, size: *mut I, t: *mut E, name: MS);
        fn glGetAttachedShaders(p: U, max: I, count: *mut I, shaders: *mut U);
        fn glGetAttribLocation(p: U, n: S) -> I;
        fn glGetBooleanv(n: E, d: *mut B);
        fn glGetBufferParameteriv(t: E, n: E, d: *mut I);
        fn glGetError() -> E;
        fn glGetFloatv(n: E, d: *mut F);
        fn glGetFramebufferAttachmentParameteriv(t: E, a: E, n: E, d: *mut I);
        fn glGetIntegerv(n: E, d: *mut I);
        fn glGetProgramiv(p: U, n: E, d: *mut I);
        fn glGetProgramInfoLog(p: U, buf: I, len: *mut I, log: MS);
        fn glGetRenderbufferParameteriv(t: E, n: E, d: *mut I);
        fn glGetShaderiv(s: U, n: E, d: *mut I);
        fn glGetShaderInfoLog(s: U, buf: I, len: *mut I, log: MS);
        fn glGetShaderPrecisionFormat(st: E, pt: E, range: *mut I, precision: *mut I);
        fn glGetShaderSource(s: U, buf: I, len: *mut I, src: MS);
        fn glGetTexParameterfv(t: E, n: E, d: *mut F);
        fn glGetTexParameteriv(t: E, n: E, d: *mut I);
        fn glGetUniformfv(p: U, loc: I, d: *mut F);
        fn glGetUniformiv(p: U, loc: I, d: *mut I);
        fn glGetUniformLocation(p: U, n: S) -> I;
        fn glGetVertexAttribfv(i: U, n: E, d: *mut F);
        fn glGetVertexAttribiv(i: U, n: E, d: *mut I);
        fn glHint(t: E, m: E);
        fn glIsBuffer(x: U) -> B;
        fn glIsEnabled(c: E) -> B;
        fn glIsFramebuffer(x: U) -> B;
        fn glIsProgram(x: U) -> B;
        fn glIsRenderbuffer(x: U) -> B;
        fn glIsShader(x: U) -> B;
        fn glIsTexture(x: U) -> B;
        fn glLineWidth(w: F);
        fn glLinkProgram(p: U);
        fn glPixelStorei(n: E, v: I);
        fn glPolygonOffset(f: F, u: F);
        fn glReadPixels(x: I, y: I, w: I, h: I, fmt: E, t: E, pixels: *mut c_void);
        fn glReleaseShaderCompiler();
        fn glRenderbufferStorage(t: E, fmt: E, w: I, h: I);
        fn glSampleCoverage(v: F, invert: B);
        fn glScissor(x: I, y: I, w: I, h: I);
        fn glShaderBinary(n: I, shaders: *const U, fmt: E, binary: P, len: I);
        fn glStencilFunc(f: E, r: I, m: U);
        fn glStencilFuncSeparate(face: E, f: E, r: I, m: U);
        fn glStencilMask(m: U);
        fn glStencilMaskSeparate(face: E, m: U);
        fn glStencilOp(f: E, zf: E, zp: E);
        fn glStencilOpSeparate(face: E, f: E, zf: E, zp: E);
        fn glTexImage2D(t: E, level: I, ifmt: I, w: I, h: I, border: I, fmt: E, ty: E, pixels: P);
        fn glTexParameterf(t: E, n: E, v: F);
        fn glTexParameterfv(t: E, n: E, v: *const F);
        fn glTexParameteri(t: E, n: E, v: I);
        fn glTexParameteriv(t: E, n: E, v: *const I);
        fn glTexSubImage2D(t: E, level: I, x: I, y: I, w: I, h: I, fmt: E, ty: E, pixels: P);
        fn glUniform1f(l: I, x: F);
        fn glUniform1fv(l: I, n: I, v: *const F);
        fn glUniform1i(l: I, x: I);
        fn glUniform1iv(l: I, n: I, v: *const I);
        fn glUniform2f(l: I, x: F, y: F);
        fn glUniform2fv(l: I, n: I, v: *const F);
        fn glUniform2i(l: I, x: I, y: I);
        fn glUniform2iv(l: I, n: I, v: *const I);
        fn glUniform3f(l: I, x: F, y: F, z: F);
        fn glUniform3fv(l: I, n: I, v: *const F);
        fn glUniform3i(l: I, x: I, y: I, z: I);
        fn glUniform3iv(l: I, n: I, v: *const I);
        fn glUniform4f(l: I, x: F, y: F, z: F, w: F);
        fn glUniform4fv(l: I, n: I, v: *const F);
        fn glUniform4i(l: I, x: I, y: I, z: I, w: I);
        fn glUniform4iv(l: I, n: I, v: *const I);
        fn glUniformMatrix2fv(l: I, n: I, transpose: B, v: *const F);
        fn glUniformMatrix3fv(l: I, n: I, transpose: B, v: *const F);
        fn glUniformMatrix4fv(l: I, n: I, transpose: B, v: *const F);
        fn glUseProgram(p: U);
        fn glValidateProgram(p: U);
        fn glVertexAttrib1f(i: U, x: F);
        fn glVertexAttrib1fv(i: U, v: *const F);
        fn glVertexAttrib2f(i: U, x: F, y: F);
        fn glVertexAttrib2fv(i: U, v: *const F);
        fn glVertexAttrib3f(i: U, x: F, y: F, z: F);
        fn glVertexAttrib3fv(i: U, v: *const F);
        fn glVertexAttrib4f(i: U, x: F, y: F, z: F, w: F);
        fn glVertexAttrib4fv(i: U, v: *const F);
        fn glVertexAttribPointer(i: U, size: I, t: E, normalized: B, stride: I, p: P);
        fn glViewport(x: I, y: I, w: I, h: I);
    }

    // OpenGL ES 1.x fixed-function additions and the OES framebuffer object extension.
    gl! { "GLESv1_CM", reg_es1:
        fn glAlphaFunc(f: E, r: F);
        fn glClientActiveTexture(t: E);
        fn glColor4f(r: F, g: F, b: F, a: F);
        fn glColor4ub(r: B, g: B, b: B, a: B);
        fn glColorPointer(size: I, t: E, stride: I, p: P);
        fn glDisableClientState(a: E);
        fn glEnableClientState(a: E);
        fn glFogf(n: E, v: F);
        fn glFogfv(n: E, v: *const F);
        fn glFrustumf(l: F, r: F, b: F, t: F, n: F, f: F);
        fn glLightf(l: E, n: E, v: F);
        fn glLightfv(l: E, n: E, v: *const F);
        fn glLightModelf(n: E, v: F);
        fn glLightModelfv(n: E, v: *const F);
        fn glLoadIdentity();
        fn glLoadMatrixf(m: *const F);
        fn glMaterialf(face: E, n: E, v: F);
        fn glMaterialfv(face: E, n: E, v: *const F);
        fn glMatrixMode(m: E);
        fn glMultMatrixf(m: *const F);
        fn glMultiTexCoord4f(t: E, s: F, tt: F, r: F, q: F);
        fn glNormal3f(x: F, y: F, z: F);
        fn glNormalPointer(t: E, stride: I, p: P);
        fn glOrthof(l: F, r: F, b: F, t: F, n: F, f: F);
        fn glPointSize(s: F);
        fn glPopMatrix();
        fn glPushMatrix();
        fn glRotatef(a: F, x: F, y: F, z: F);
        fn glScalef(x: F, y: F, z: F);
        fn glShadeModel(m: E);
        fn glTexCoordPointer(size: I, t: E, stride: I, p: P);
        fn glTexEnvf(t: E, n: E, v: F);
        fn glTexEnvfv(t: E, n: E, v: *const F);
        fn glTexEnvi(t: E, n: E, v: I);
        fn glTranslatef(x: F, y: F, z: F);
        fn glVertexPointer(size: I, t: E, stride: I, p: P);
        fn glGenRenderbuffersOES(n: I, b: *mut U);
        fn glDeleteRenderbuffersOES(n: I, b: *const U);
        fn glBindRenderbufferOES(t: E, r: U);
        fn glRenderbufferStorageOES(t: E, fmt: E, w: I, h: I);
        fn glGenFramebuffersOES(n: I, b: *mut U);
        fn glDeleteFramebuffersOES(n: I, b: *const U);
        fn glBindFramebufferOES(t: E, f: U);
        fn glFramebufferTexture2DOES(t: E, a: E, tt: E, tex: U, level: I);
        fn glFramebufferRenderbufferOES(t: E, a: E, rt: E, r: U);
        fn glCheckFramebufferStatusOES(t: E) -> E;
        fn glGenerateMipmapOES(t: E);
        fn glIsFramebufferOES(x: U) -> B;
        fn glIsRenderbufferOES(x: U) -> B;
        fn glGetRenderbufferParameterivOES(t: E, n: E, d: *mut I);
        fn glGetFramebufferAttachmentParameterivOES(t: E, a: E, n: E, d: *mut I);
        fn glBlendEquationOES(m: E);
        fn glBlendFuncSeparateOES(sr: E, dr: E, sa: E, da: E);
        fn glBlendEquationSeparateOES(rgb: E, a: E);
        fn glDrawTexiOES(x: I, y: I, z: I, w: I, h: I);
        fn glDrawTexfOES(x: F, y: F, z: F, w: F, h: F);
    }

    #[link(name = "GLESv2")]
    extern "C" {
        fn glGetString(name: E) -> *const c_char;
        fn glShaderSource(shader: U, count: I, strings: *const *const c_char, lengths: *const I);
        fn glGetVertexAttribPointerv(index: U, pname: E, out: *mut *mut c_void);
    }

    const GL_BUFFER_SIZE: E = 0x8764;

    // GL_OES_mapbuffer: the driver's mapping is outside guest memory, so the guest writes into a
    // shadow buffer that is uploaded with glBufferSubData when it is unmapped.
    static MAPS: Mutex<Option<HashMap<E, (u32, I)>>> = Mutex::new(None); // target -> (guest memory, size)

    pub fn register() {
        reg_es2();
        reg_es1();
        // Functions that return or take host-sized pointers need manual handling.
        reg("glGetString", |name: E| -> u32 { intern_cstr(unsafe { glGetString(name) }) });
        reg("glShaderSource", |shader: U, count: I, strings: *const u32, lengths: *const I| unsafe {
            let ptrs: Vec<*const c_char> = (0..count.max(0) as usize).map(|i| g2h::<c_char>(*strings.add(i)) as *const c_char).collect();
            glShaderSource(shader, count, ptrs.as_ptr(), lengths);
        });
        reg("glMapBufferOES", |target: E, _access: E| -> u32 {
            let mut size: I = 0;
            unsafe { glGetBufferParameteriv(target, GL_BUFFER_SIZE, &mut size) };
            if size <= 0 {
                return 0;
            }
            let mem = g_malloc(size as u32);
            if mem == 0 {
                return 0;
            }
            if let Some((old, _)) = lock(&MAPS).get_or_insert_with(HashMap::new).insert(target, (mem, size)) {
                g_free(old);
            }
            mem
        });
        reg("glUnmapBufferOES", |target: E| -> u32 {
            let Some((mem, size)) = lock(&MAPS).as_mut().and_then(|m| m.remove(&target)) else { return 0 };
            unsafe { glBufferSubData(target, 0, size as L, g2h::<c_void>(mem)) };
            g_free(mem);
            1
        });
        reg("glGetBufferPointervOES", |target: E, _pname: E, out: *mut u32| unsafe {
            *out = lock(&MAPS).as_ref().and_then(|m| m.get(&target)).map(|s| s.0).unwrap_or(0);
        });
        reg("glGetVertexAttribPointerv", |index: U, pname: E, out: *mut u32| unsafe {
            let mut p: *mut c_void = std::ptr::null_mut();
            glGetVertexAttribPointerv(index, pname, &mut p);
            *out = p as usize as u32;
        });
    }
}
