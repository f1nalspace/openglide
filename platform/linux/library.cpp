//**************************************************************
//*            OpenGLide - Glide to OpenGL Wrapper
//*             http://openglide.sourceforge.net
//*
//*     Linux specific functions for library initialisation
//*
//*         OpenGLide is OpenSource under LGPL license
//*              Originaly made by Fabio Barros
//*      Modified by Paul for Glidos (http://www.glidos.net)
//*               Linux version by Simon White
//**************************************************************
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if !defined WIN32 && !defined C_USE_FVM3DX

#include <stdlib.h>
#include "GlOgl.h"

class InitLibrary
{
public:
    InitLibrary( )
    {
        if ( !ClearAndGenerateLogFile( ) )
        {
            exit( 0 );
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

#endif // !WIN32 && !C_USE_FVM3DX
