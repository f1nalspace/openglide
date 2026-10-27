//**************************************************************
//*            OpenGLide - Glide to OpenGL Wrapper
//*             http://openglide.sourceforge.net
//*
//*       fvm3dx: library initialisation inside QEMU
//*
//*         OpenGLide is OpenSource under LGPL license
//**************************************************************
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef C_USE_FVM3DX

#include <stdio.h>

#include "GlOgl.h"

// QEMU loads the library: without a log file OpenGLide still works, so it never ends the process the way the Linux platform does.
class InitLibrary
{
public:
    InitLibrary( )
    {
        if ( !ClearAndGenerateLogFile( ) )
        {
            fprintf( stderr, "openglide: cannot write %s in the working directory, going on without it\n", GLIDEFILE );
        }
        InitMainVariables( );
    }
    ~InitLibrary( )
    {
        grGlideShutdown( );
        CloseLogFile( );
    }
};

static InitLibrary initLibrary;

#endif // C_USE_FVM3DX
