//**************************************************************
//*            OpenGLide - Glide to OpenGL Wrapper
//*             http://openglide.sourceforge.net
//*
//*      fvm3dx: OpenGL extension functions through EGL
//*
//*         OpenGLide is OpenSource under LGPL license
//**************************************************************
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef C_USE_FVM3DX

#include <EGL/egl.h>

#include "platform/openglext.h"

ExtFn OGLGetProcAddress( const char *x )
{
    return (ExtFn)eglGetProcAddress( x );
}

#endif // C_USE_FVM3DX
