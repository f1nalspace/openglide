//**************************************************************
//*            OpenGLide - Glide to OpenGL Wrapper
//*             http://openglide.sourceforge.net
//*
//*   fvm3dx: draws into the EGL context the device made current
//*
//*         OpenGLide is OpenSource under LGPL license
//**************************************************************
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef C_USE_FVM3DX

#include <EGL/egl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "GlOgl.h"

#include "platform/window.h"

/* The fvm3dx device owns the context and its drawable. It makes both current before grSstWinOpen, sizes the drawable to the game's
 * resolution and scales the finished frame itself, so OpenGLide creates no window, never swaps on its own and never reaches the host's
 * gamma ramp: a gamma table the game loads is only remembered.
 */

enum { gammaRampEntryCount = 256 };

// The layout grLoadGammaTable and grGetGammaTableExt hand over in g3wrap.cpp.
struct GammaRamp
{
    uint16_t red[ gammaRampEntryCount ];
    uint16_t green[ gammaRampEntryCount ];
    uint16_t blue[ gammaRampEntryCount ];
};

static GammaRamp requestedRamp;
static bool requestedRampValid;

bool InitialiseOpenGLWindow( FxU wnd, int x, int y, int width, int height )
{
    (void)wnd;
    (void)x;
    (void)y;
    EGLContext context = eglGetCurrentContext( );
    if ( context == EGL_NO_CONTEXT )
    {
        fprintf( stderr, "openglide: no current EGL context -- the fvm3dx device makes its context current before grSstWinOpen\n" );
        return false;
    }
    EGLDisplay display = eglGetCurrentDisplay( );
    EGLSurface surface = eglGetCurrentSurface( EGL_DRAW );
    EGLint surfaceWidth = 0;
    EGLint surfaceHeight = 0;
    eglQuerySurface( display, surface, EGL_WIDTH, &surfaceWidth );
    eglQuerySurface( display, surface, EGL_HEIGHT, &surfaceHeight );
    if ( ( surfaceWidth != width ) || ( surfaceHeight != height ) )
    {
        fprintf( stderr, "openglide: the drawable is %dx%d, the game draws %dx%d\n", surfaceWidth, surfaceHeight, width, height );
    }

    GLint depthBits = 0;
    glGetIntegerv( GL_DEPTH_BITS, &depthBits );
    GlideMsg( "fvm3dx: %s, OpenGL %s, drawable %dx%d, depth %d bits\n",
              glGetString( GL_RENDERER ), glGetString( GL_VERSION ), surfaceWidth, surfaceHeight, depthBits );

    // A depth buffer deeper than a Voodoo's needs no help against z-fighting.
    const GLint voodooDepthBits = 16;
    if ( depthBits > voodooDepthBits )
    {
        UserConfig.PrecisionFix = false;
    }
    if ( UserConfig.FramebufferSRGB )
    {
        glEnable( GL_FRAMEBUFFER_SRGB );
    }
    return true;
}

void FinaliseOpenGLWindow( void )
{
    if ( UserConfig.FramebufferSRGB )
    {
        glDisable( GL_FRAMEBUFFER_SRGB );
    }
}

void SetGamma( float value )
{
    const float highestEntry = gammaRampEntryCount - 1;
    const float rampMaximum = 0xFFFF;
    GammaRamp ramp;
    for ( int index = 0; index < gammaRampEntryCount; index++ )
    {
        float level = powf( index / highestEntry, 1.0f / value );
        uint16_t entry = (uint16_t)( rampMaximum * level );
        ramp.red[ index ] = entry;
        ramp.green[ index ] = entry;
        ramp.blue[ index ] = entry;
    }
    SetGammaTable( &ramp );
}

void RestoreGamma( )
{
}

void SetGammaTable( void *table )
{
    memcpy( &requestedRamp, table, sizeof( requestedRamp ) );
    requestedRampValid = true;
}

void GetGammaTable( void *table )
{
    if ( requestedRampValid )
    {
        memcpy( table, &requestedRamp, sizeof( requestedRamp ) );
        return;
    }
    const int byteShift = 8;
    GammaRamp *ramp = (GammaRamp *)table;
    for ( int index = 0; index < gammaRampEntryCount; index++ )
    {
        uint16_t linearEntry = (uint16_t)( ( index << byteShift ) | index );
        ramp->red[ index ] = linearEntry;
        ramp->green[ index ] = linearEntry;
        ramp->blue[ index ] = linearEntry;
    }
}

bool SetScreenMode( int &xsize, int &ysize )
{
    (void)xsize;
    (void)ysize;
    return true;
}

void ResetScreenMode( )
{
}

// The device paces the frame; the game's swap interval reaches it with the swap from the guest.
void SetSwapInterval( const int interval )
{
    (void)interval;
}

// The device presents the frame through the swap callback setConfigRes hands over; without one the frame is only flushed.
void SwapBuffers( )
{
    if ( UserConfig.swap12 )
    {
        void ( *swapCallback )( void ) = (void ( * )( void ))UserConfig.swap12;
        swapCallback( );
        return;
    }
    glFlush( );
}

#endif // C_USE_FVM3DX
