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

#include "GlOgl.h"

// QEMU loads the library. Messages go to the device through setConfigMessages, so there is no log file to fail on. The device shuts
// Glide down itself while the guest's context is still current: at QEMU's exit no context is, and the GL calls of grGlideShutdown would go nowhere.
class InitLibrary
{
public:
    InitLibrary( )
    {
        ClearAndGenerateLogFile( );
        InitMainVariables( );
    }
};

static InitLibrary initLibrary;

#endif // C_USE_FVM3DX
