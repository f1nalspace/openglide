//**************************************************************
//*            OpenGLide - Glide to OpenGL Wrapper
//*             http://openglide.sourceforge.net
//*
//*           implementation of the PGTexture class
//*
//*         OpenGLide is OpenSource under LGPL license
//*              Originaly made by Fabio Barros
//*      Modified by Paul for Glidos (http://www.glidos.net)
//*               Linux version by Simon White
//**************************************************************

#include "GlOgl.h"
#include "PGTexture.h"
#include "GLExtensions.h"
#include "FormatConversion.h"
#include "OGLTables.h"

#include <stdlib.h>
#include <stdarg.h>

//**************************************************************
//* Diagnostics: which texture formats, palettes and pipeline
//* states does a game actually use?
//*
//* Switched on with QEMU_3DFX_TEXLOG=1, writes OpenGLid.tex. Only
//* distinct records are kept, each with the number of times it was
//* seen, and the file is rewritten whenever a new one shows up --
//* so the findings survive a game that crashes.
//**************************************************************

#define TEXDIAG_FILE        "OpenGLid.tex"
#define TEXDIAG_MAX_RECORDS 192
#define TEXDIAG_MAX_LENGTH  256

static bool  texdiag_enabled_known = false;
static bool  texdiag_enabled       = false;
static char  texdiag_record[ TEXDIAG_MAX_RECORDS ][ TEXDIAG_MAX_LENGTH ];
static FxU32 texdiag_count[ TEXDIAG_MAX_RECORDS ];
static int   texdiag_records       = 0;

// Probe: with QEMU_3DFX_KEYMARK=1 the chroma-keyed palette entries do not become
// transparent but opaque magenta. Whatever turns magenta on screen was keyed out
// by the palette -- everything that stays as it was comes from somewhere else.
static bool texdiag_keymark_known = false;
static bool texdiag_keymark       = false;

static bool TexDiagKeyMark( void )
{
    if ( ! texdiag_keymark_known )
    {
        const char * setting = getenv( "QEMU_3DFX_KEYMARK" );

        texdiag_keymark = ( setting != NULL ) && ( setting[ 0 ] != '0' );
        texdiag_keymark_known = true;
    }

    return texdiag_keymark;
}

static bool TexDiagEnabled( void )
{
    if ( ! texdiag_enabled_known )
    {
        const char * setting = getenv( "QEMU_3DFX_TEXLOG" );

        texdiag_enabled = ( setting != NULL ) && ( setting[ 0 ] != '0' );
        texdiag_enabled_known = true;
    }

    return texdiag_enabled;
}

static void TexDiagWriteFile( void )
{
    FILE * file = fopen( TEXDIAG_FILE, "w" );
    int    index;

    if ( file == NULL )
    {
        return;
    }

    fprintf( file, "OpenGLide texture diagnostics -- %d distinct records\n\n", texdiag_records );
    for ( index = 0; index < texdiag_records; index++ )
    {
        fprintf( file, "%8u x  %s\n", texdiag_count[ index ], texdiag_record[ index ] );
    }
    fclose( file );
}

static int texdiag_palettes_written = 0;

// The first few palettes go into OpenGLid.pal in full. Without that there is no
// way to tell whether the chroma key colour is in the palette at all.
static void TexDiagPalette( GrTexTable_t type, const FxU32 * data, int first, int count )
{
    const int maximum_palettes = 4;

    FILE * file;
    int    entry;

    if ( ! TexDiagEnabled( ) || ( type != GR_TEXTABLE_PALETTE ) )
    {
        return;
    }
    if ( texdiag_palettes_written >= maximum_palettes )
    {
        return;
    }

    file = fopen( "OpenGLid.pal", ( texdiag_palettes_written == 0 ) ? "w" : "at" );
    if ( file == NULL )
    {
        return;
    }

    fprintf( file, "palette %d: first=%d count=%d\n", texdiag_palettes_written, first, count );
    for ( entry = 0; entry < count; entry++ )
    {
        fprintf( file, "%3d 0x%08x%s", first + entry, (unsigned)data[ entry ],
                 ( ( entry % 4 ) == 3 ) ? "\n" : "  " );
    }
    fprintf( file, "\n\n" );
    fclose( file );

    texdiag_palettes_written++;
}

// Writes the palettized texture that is about to be uploaded as a PPM, one file
// per size, always the most recent one. Texels the chroma key removes come out
// magenta, so it is visible at a glance what should have been transparent.
static void TexDiagTexturePPM( int width, int height, const FxU8 * data, const FxU32 * palette )
{
    char   name[ 64 ];
    FILE * file;
    int    x, y;

    if ( ! TexDiagEnabled( ) )
    {
        return;
    }

    sprintf( name, "OpenGLid-%dx%d.ppm", width, height );
    file = fopen( name, "wb" );
    if ( file == NULL )
    {
        return;
    }

    fprintf( file, "P6\n%d %d\n255\n", width, height );
    for ( y = 0; y < height; y++ )
    {
        for ( x = 0; x < width; x++ )
        {
            FxU32 colour = palette[ data[ y * width + x ] ];
            FxU8  pixel[ 3 ];

            if ( ( colour & 0xff000000 ) == 0 )
            {
                pixel[ 0 ] = 255; pixel[ 1 ] = 0; pixel[ 2 ] = 255;
            }
            else
            {
                pixel[ 0 ] = (FxU8)( ( colour >> 16 ) & 0xff );
                pixel[ 1 ] = (FxU8)( ( colour >>  8 ) & 0xff );
                pixel[ 2 ] = (FxU8)(   colour         & 0xff );
            }
            fwrite( pixel, 1, 3, file );
        }
    }
    fclose( file );
}

static void TexDiagNote( const char * format, ... )
{
    char    line[ TEXDIAG_MAX_LENGTH ];
    va_list arguments;
    int     index;

    if ( ! TexDiagEnabled( ) )
    {
        return;
    }

    va_start( arguments, format );
    vsnprintf( line, sizeof( line ), format, arguments );
    va_end( arguments );

    for ( index = 0; index < texdiag_records; index++ )
    {
        if ( strcmp( texdiag_record[ index ], line ) == 0 )
        {
            texdiag_count[ index ]++;
            return;
        }
    }

    if ( texdiag_records >= TEXDIAG_MAX_RECORDS )
    {
        return;
    }

    strcpy( texdiag_record[ texdiag_records ], line );
    texdiag_count[ texdiag_records ] = 1;
    texdiag_records++;
    TexDiagWriteFile( );
}

#define OGL_LOAD_CREATE_TEXTURE( compnum, compformat, comptype, texdata )   \
    {                                                                       \
        glTexImage2D( GL_TEXTURE_2D, texVals.lod, compnum,                  \
                    texVals.width, texVals.height,                          \
                    0, compformat, comptype, texdata );                     \
        if ( InternalConfig.BuildMipMaps )                                  \
        {                                                                   \
            gluBuild2DMipmaps( GL_TEXTURE_2D, compnum,                      \
                               texVals.width, texVals.height,               \
                               compformat, comptype, texdata );             \
        }                                                                   \
    }


void genPaletteMipmaps( FxU32 width, FxU32 height, FxU8 *data )
{
    FxU8    buf[ 128 * 128 ];
    FxU32   mmwidth;
    FxU32   mmheight;
    FxU32   lod;
    FxU32   skip;

    mmwidth = width;
    mmheight = height;
    lod = 0;
    skip = 1;

    while ( ( mmwidth > 1 ) || ( mmheight > 1 ) )
    {
        FxU32   x, 
                y;

        mmwidth = mmwidth > 1 ? mmwidth / 2 : 1;
        mmheight = mmheight > 1 ? mmheight / 2 : 1;
        lod += 1;
        skip *= 2;

        for ( y = 0; y < mmheight; y++ )
        {
            FxU8    * in, 
                    * out;

            in = data + width * y * skip;
            out = buf + mmwidth * y;
            for ( x = 0; x < mmwidth; x++ )
            {
                out[ x ] = in[ x * skip ];
            }
        }

        glTexImage2D( GL_TEXTURE_2D, lod, GL_COLOR_INDEX8_EXT, mmwidth, mmheight, 0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, buf );
    }
}

PGTexture::PGTexture( int mem_size )
{
    m_db = new TexDB( mem_size );
    m_palette_dirty = true;
    m_valid = false;
    m_chromakey_mode = GR_CHROMAKEY_DISABLE;
    m_tex_memory_size = mem_size;
    m_memory = new FxU8[ mem_size ];
    m_ncc_select = GR_NCCTABLE_NCC0;

#ifdef OGL_DEBUG
    Num_565_Tex = 0;
    Num_1555_Tex = 0;
    Num_4444_Tex = 0;
    Num_332_Tex = 0;
    Num_8332_Tex = 0;
    Num_Alpha_Tex = 0;
    Num_AlphaIntensity88_Tex = 0;
    Num_AlphaIntensity44_Tex = 0;
    Num_AlphaPalette_Tex = 0;
    Num_Palette_Tex = 0;
    Num_Intensity_Tex = 0;
    Num_YIQ_Tex = 0;
    Num_AYIQ_Tex = 0;
    Num_Other_Tex = 0;
#endif
}

PGTexture::~PGTexture( void )
{
    delete[] m_memory;
    delete m_db;
}

void PGTexture::DownloadMipMap( FxU32 startAddress, FxU32 evenOdd, GrTexInfo *info )
{
    FxU32 mip_size = MipMapMemRequired( info->smallLod, 
                                        info->aspectRatio, 
                                        info->format );
    FxU32 mip_offset = startAddress + TextureMemRequired( evenOdd, info );
    
    if ( info->format == GR_TEXFMT_BGRA_8888 )
    {
        // Glidos never defines an extension texture unless it intends to use
        // it, so we can create the OpenGL texture here and not bother to
        // copy anything to the virtual glide texture memory
        GLuint  texNum;
        TexValues  texVals;

        texVals.width = texInfo[ info->aspectRatio ][ info->largeLod ].width;
        texVals.height = texInfo[ info->aspectRatio ][ info->largeLod ].height;
        texVals.nPixels = texInfo[ info->aspectRatio ][ info->largeLod ].numPixels;
        texVals.lod = 0;

        m_db->WipeRange( startAddress, mip_offset, 0 );
        m_db->Add( startAddress, mip_offset, info, 0, &texNum, NULL);

        glBindTexture( GL_TEXTURE_2D, texNum );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, OpenGL.SClampMode );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, OpenGL.TClampMode );
    
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, OpenGL.MinFilterMode );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, OpenGL.MagFilterMode );

        if ( InternalConfig.EnableMipMaps && !InternalConfig.BuildMipMaps )
            glTexParameteri( GL_TEXTURE_2D, GL_GENERATE_MIPMAP_SGIS, true );

        OGL_LOAD_CREATE_TEXTURE( 4, GL_BGRA, GL_UNSIGNED_BYTE, info->data );
    }
    else
    {
        if ( mip_offset <= m_tex_memory_size )
        {
            memcpy( m_memory + mip_offset - mip_size, info->data, mip_size );
        }
        
        // Any texture based on memory crossing this range
        // is now out of date
        m_db->WipeRange( startAddress, mip_offset, 0 );
    }

#ifdef OGL_DEBUG
    if ( info->smallLod == info->largeLod )
    {
        switch ( info->format )
        {
        case GR_TEXFMT_RGB_332:             Num_332_Tex++;                  break;
        case GR_TEXFMT_YIQ_422:             Num_YIQ_Tex++;                  break;
        case GR_TEXFMT_ALPHA_8:             Num_Alpha_Tex++;                break;
        case GR_TEXFMT_INTENSITY_8:         Num_Intensity_Tex++;            break;
        case GR_TEXFMT_ALPHA_INTENSITY_44:  Num_AlphaIntensity44_Tex++;     break;
        case GR_TEXFMT_P_8:                 Num_565_Tex++;                  break;
        case GR_TEXFMT_ARGB_8332:           Num_8332_Tex++;                 break;
        case GR_TEXFMT_AYIQ_8422:           Num_AYIQ_Tex++;                 break;
        case GR_TEXFMT_RGB_565:             Num_565_Tex++;                  break;
        case GR_TEXFMT_ARGB_1555:           Num_1555_Tex++;                 break;
        case GR_TEXFMT_ARGB_4444:           Num_4444_Tex++;                 break;
        case GR_TEXFMT_ALPHA_INTENSITY_88:  Num_AlphaIntensity88_Tex++;     break;
        case GR_TEXFMT_AP_88:               Num_AlphaPalette_Tex++;         break;
        case GR_TEXFMT_RSVD0:
        case GR_TEXFMT_RSVD1:
        case GR_TEXFMT_RSVD2:               Num_Other_Tex++;                break;
        }
    }
#endif
}

void PGTexture::DownloadMipMapPartial( FxU32 startAddress, FxU32 evenOdd, GrTexInfo *info, int start, int end )
{
    // Glidos has different handling of grTexDownload, so keep it
    if ( info->format == GR_TEXFMT_BGRA_8888 )
    {
        DownloadMipMap( startAddress, evenOdd, info );
        return;
    }

    FxU32 mip_size = MipMapMemRequired( info->smallLod, 
                                        info->aspectRatio, 
                                        info->format );
    FxU32 mip_offset = startAddress + TextureMemRequired( evenOdd, info );
 
    if ( mip_offset <= m_tex_memory_size )
    {
        int stride = texInfo[ info->aspectRatio ][ info->smallLod ].width;
        if (info->format >= GR_TEXFMT_16BIT)
            stride *= 2;
        FxU8 *mip_memory = m_memory + mip_offset - mip_size;
        memcpy( mip_memory + start * stride, info->data, (end - start + 1) * stride );

        m_db->WipeRange( startAddress, mip_offset, 0 );
    }
}

void PGTexture::Source( FxU32 startAddress, FxU32 evenOdd, GrTexInfo *info )
{
    m_startAddress = startAddress;
    m_evenOdd = evenOdd;
    m_info = *info;

    m_wAspect = texAspects[ info->aspectRatio ].w;
    m_hAspect = texAspects[ info->aspectRatio ].h;

    m_valid = ( ( startAddress + TextureMemRequired( evenOdd, info ) ) <= m_tex_memory_size );
}

void PGTexture::DownloadTable( GrTexTable_t type, FxU32 *data, int first, int count )
{
    TexDiagNote( "TABLE type=%d first=%d count=%d data[0]=0x%08x data[1]=0x%08x data[%d]=0x%08x",
                 (int)type, first, count,
                 (unsigned)data[ 0 ], (unsigned)data[ ( count > 1 ) ? 1 : 0 ],
                 count - 1, (unsigned)data[ count - 1 ] );
    TexDiagPalette( type, data, first, count );

    if ( type == GR_TEXTABLE_PALETTE )
    {
        for ( int i = count - 1; i >= 0; i-- )
        {
              m_palette[ first + i ] = data[ i ]; 
        }
        
        m_palette_dirty = true;
    }
    else
    {
        // GR_TEXTABLE_NCC0 or GR_TEXTABLE_NCC1
        int         i;
        GuNccTable *ncc = &(m_ncc[ type ]);

        memcpy( ncc, data, sizeof( GuNccTable ) );

        for ( i = 0; i < 4; i++ )
        {
            if ( ncc->iRGB[ i ][ 0 ] & 0x100 )
                ncc->iRGB[ i ][ 0 ] |= 0xff00;
            if ( ncc->iRGB[ i ][ 1 ] & 0x100 )
                ncc->iRGB[ i ][ 1 ] |= 0xff00;
            if ( ncc->iRGB[ i ][ 2 ] & 0x100 )
                ncc->iRGB[ i ][ 2 ] |= 0xff00;

            if ( ncc->qRGB[ i ][ 0 ] & 0x100 )
                ncc->qRGB[ i ][ 0 ] |= 0xff00;
            if ( ncc->qRGB[ i ][ 1 ] & 0x100 )
                ncc->qRGB[ i ][ 1 ] |= 0xff00;
            if ( ncc->qRGB[ i ][ 2 ] & 0x100 )
                ncc->qRGB[ i ][ 2 ] |= 0xff00;
        }
    }
}

bool PGTexture::MakeReady( void )
{
    FxU8        * data;
    TexValues   texVals;
    GLuint      texNum;
    GLuint      tex2Num;
    FxU32       size;
    FxU32       test_hash;
    FxU32       wipe_hash;
    bool        palette_changed;
    bool        * pal_change_ptr;
    bool        use_mipmap_ext;
    bool        use_mipmap_ext2;
    bool        use_two_textures;

    if( ! m_valid )
    {
        return false;
    }

    test_hash        = 0;
    wipe_hash        = 0;
    palette_changed  = false;
    use_mipmap_ext   = ( InternalConfig.EnableMipMaps && !InternalConfig.BuildMipMaps );
    use_mipmap_ext2  = use_mipmap_ext;
    use_two_textures = false;
    pal_change_ptr   = NULL;
    
    data             = m_memory + m_startAddress;

    size             = TextureMemRequired( m_evenOdd, &m_info );

    GetTexValues( &texVals );

    if ( TexDiagEnabled( ) )
    {
        // How often does the chroma key actually hit the palette -- compared at
        // full 24 bit, the way OpenGLide does it, and at the 5:6:5 of the frame
        // buffer, the way a Voodoo does it?
        FxU32 matches888 = 0;
        FxU32 matches565 = 0;
        int   firstMatchIndex = -1;
        FxU32 firstMatchColour = 0;
        int   entry;

        for ( entry = 0; entry < 256; entry++ )
        {
            FxU32 colour    = m_palette[ entry ] & 0x00ffffff;
            FxU16 colour565 = (FxU16)( ( colour & 0x00F80000 ) >> 8 |
                                       ( colour & 0x0000FC00 ) >> 5 |
                                       ( colour & 0x000000F8 ) >> 3 );

            if ( colour == m_chromakey_value_8888 )
            {
                matches888++;
            }
            if ( colour565 == m_chromakey_value_565 )
            {
                matches565++;
                if ( firstMatchIndex < 0 )
                {
                    firstMatchIndex  = entry;
                    firstMatchColour = colour;
                }
            }
        }

        TexDiagNote( "TEX fmt=0x%02x %dx%d chroma=%d key=0x%06x/0x%04x cfmt=%d blend=%d sf=%d df=%d "
                     "acomb(fn=%d,fac=%d,loc=%d,oth=%d) ccomb(fn=%d,fac=%d,loc=%d,oth=%d) "
                     "pal[0]=0x%08x hit888=%u hit565=%u first=%d/0x%06x raw=0x%08x const=0x%08x",
                     (unsigned)m_info.format, (int)texVals.width, (int)texVals.height,
                     (int)m_chromakey_mode, (unsigned)m_chromakey_value_8888,
                     (unsigned)m_chromakey_value_565, (int)Glide.State.ColorFormat,
                     OpenGL.Blend ? 1 : 0,
                     (int)Glide.State.AlphaBlendRgbSf, (int)Glide.State.AlphaBlendRgbDf,
                     (int)Glide.State.AlphaFunction, (int)Glide.State.AlphaFactor,
                     (int)Glide.State.AlphaLocal, (int)Glide.State.AlphaOther,
                     (int)Glide.State.ColorCombineFunction, (int)Glide.State.ColorCombineFactor,
                     (int)Glide.State.ColorCombineLocal, (int)Glide.State.ColorCombineOther,
                     (unsigned)m_palette[ 0 ], (unsigned)matches888, (unsigned)matches565,
                     firstMatchIndex, (unsigned)firstMatchColour,
                     (unsigned)Glide.State.ChromakeyValue,
                     (unsigned)Glide.State.ConstantColorValue );
    }

    switch ( m_info.format )
    {
    case GR_TEXFMT_P_8:
        ApplyKeyToPalette( );
        if ( InternalConfig.EXT_paletted_texture )
        {
            //
            // OpenGL's mipmap generation doesn't seem
            // to handle paletted textures.
            //
            use_mipmap_ext = false;
            pal_change_ptr = &palette_changed;
        }
        else
        {
            wipe_hash = m_palette_hash;
        }

        test_hash = m_palette_hash;
        break;

    case GR_TEXFMT_AP_88:
        ApplyKeyToPalette( );
        if ( InternalConfig.EXT_paletted_texture && InternalConfig.ARB_multitexture )
        {
            use_mipmap_ext   = false;
            pal_change_ptr   = &palette_changed;
            use_two_textures = true;
        }
        else
        {
            wipe_hash = m_palette_hash;
        }

        test_hash = m_palette_hash;
        break;
    }

    // See if we already have an OpenGL texture to match this
    if ( m_db->Find( m_startAddress, &m_info, test_hash,
                     &texNum, use_two_textures ? &tex2Num : NULL,
                     pal_change_ptr ) )
    {
        glBindTexture( GL_TEXTURE_2D, texNum );

        if ( palette_changed )
        {
            p_glColorTableEXT( GL_TEXTURE_2D, GL_RGBA, 256, GL_BGRA_EXT, GL_UNSIGNED_BYTE, m_palette );
        }

        if ( use_two_textures )
        {
            p_glActiveTextureARB( GL_TEXTURE1_ARB );

            glBindTexture( GL_TEXTURE_2D, tex2Num );

            p_glActiveTextureARB( GL_TEXTURE0_ARB );
        }
    }
    else
    {
        // Any existing textures crossing this memory range
        // is unlikely to be used, so remove the OpenGL version
        // of them
        m_db->WipeRange( m_startAddress, m_startAddress + size, wipe_hash );

        // Add this new texture to the data base
        m_db->Add( m_startAddress, m_startAddress + size, &m_info, test_hash,
                   &texNum, use_two_textures ? &tex2Num : NULL );

        glBindTexture( GL_TEXTURE_2D, texNum );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, OpenGL.SClampMode );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, OpenGL.TClampMode );
    
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, OpenGL.MinFilterMode );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, OpenGL.MagFilterMode );
    
        if( use_mipmap_ext )
        {
            glTexParameteri( GL_TEXTURE_2D, GL_GENERATE_MIPMAP_SGIS, true );
        }

        if ( use_two_textures )
        {
            p_glActiveTextureARB( GL_TEXTURE1_ARB );

            glBindTexture( GL_TEXTURE_2D, tex2Num );
            glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, OpenGL.SClampMode );
            glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, OpenGL.TClampMode );
        
            glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, OpenGL.MinFilterMode );
            glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, OpenGL.MagFilterMode );

            if( use_mipmap_ext2 )
            {
                glTexParameteri( GL_TEXTURE_2D, GL_GENERATE_MIPMAP_SGIS, true );
            }
            
            p_glActiveTextureARB( GL_TEXTURE0_ARB );
        }

        switch ( m_info.format )
        {
        case GR_TEXFMT_RGB_565:
            if ( m_chromakey_mode )
            {
                Convert565Kto8888( (FxU16*)data, m_chromakey_value_565, m_tex_temp, texVals.nPixels );
                OGL_LOAD_CREATE_TEXTURE( 4, GL_RGBA, GL_UNSIGNED_BYTE, m_tex_temp );
            }
            else if ( InternalConfig.OGLVersion > OGL_VER_1_1 )
            {
                OGL_LOAD_CREATE_TEXTURE( 3, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, data );
            }
            else
            {
                if ( InternalConfig.Wrap565to5551 )
                {
                    Convert565to5551( (FxU32*)data, m_tex_temp, texVals.nPixels );
                    OGL_LOAD_CREATE_TEXTURE( 4, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1_EXT, m_tex_temp );
                }
                else
                {
                    Convert565to8888( (FxU16*)data, m_tex_temp, texVals.nPixels );
                    OGL_LOAD_CREATE_TEXTURE( 4, GL_RGBA, GL_UNSIGNED_BYTE, m_tex_temp );
                }
            }
            break;
            
        case GR_TEXFMT_ARGB_4444:
            if ( InternalConfig.OGLVersion > OGL_VER_1_1 )
            {
                OGL_LOAD_CREATE_TEXTURE( 4, GL_BGRA_EXT, GL_UNSIGNED_SHORT_4_4_4_4_REV, data );
            }
            else
            {
                Convert4444to4444special( (FxU32*)data, m_tex_temp, texVals.nPixels );
                OGL_LOAD_CREATE_TEXTURE( 4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4_EXT, m_tex_temp );
            }
            break;
            
        case GR_TEXFMT_ARGB_1555:
            if ( InternalConfig.OGLVersion > OGL_VER_1_1 )
            {
                OGL_LOAD_CREATE_TEXTURE( 4, GL_BGRA_EXT, GL_UNSIGNED_SHORT_1_5_5_5_REV, data );
            }
            else
            {
                Convert1555to5551( (FxU32*)data, m_tex_temp, texVals.nPixels );
                OGL_LOAD_CREATE_TEXTURE( 4, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1_EXT, m_tex_temp );
            }
            break;
            
        case GR_TEXFMT_P_8:
            TexDiagTexturePPM( texVals.width, texVals.height, data, m_palette );
            if ( InternalConfig.EXT_paletted_texture )
            {
                p_glColorTableEXT( GL_TEXTURE_2D, GL_RGBA, 256, GL_BGRA_EXT, GL_UNSIGNED_BYTE, m_palette );
                
                glTexImage2D( GL_TEXTURE_2D, texVals.lod, GL_COLOR_INDEX8_EXT, 
                              texVals.width, texVals.height, 0, 
                              GL_COLOR_INDEX, GL_UNSIGNED_BYTE, data );
                if ( InternalConfig.EnableMipMaps )
                {
                    genPaletteMipmaps( texVals.width, texVals.height, data );
                }
            }
            else
            {
                ConvertP8to8888( data, m_tex_temp, texVals.nPixels, m_palette );
                OGL_LOAD_CREATE_TEXTURE( 4, GL_BGRA_EXT, GL_UNSIGNED_BYTE, m_tex_temp );
            }
            break;
            
        case GR_TEXFMT_AP_88:
            if ( use_two_textures )
            {
                FxU32 *tex_temp2 = m_tex_temp + 256 * 128;

                p_glColorTableEXT( GL_TEXTURE_2D, GL_RGBA, 256, GL_BGRA_EXT, GL_UNSIGNED_BYTE, m_palette );

                SplitAP88( (FxU16 *)data, (FxU8 *)m_tex_temp, (FxU8 *)tex_temp2, texVals.nPixels );
                
                glTexImage2D( GL_TEXTURE_2D, texVals.lod, GL_COLOR_INDEX8_EXT, 
                              texVals.width, texVals.height, 0, 
                              GL_COLOR_INDEX, GL_UNSIGNED_BYTE, m_tex_temp );
                if ( InternalConfig.EnableMipMaps )
                {
                    genPaletteMipmaps( texVals.width, texVals.height, (FxU8 *)m_tex_temp );
                }

                p_glActiveTextureARB( GL_TEXTURE1_ARB );

                OGL_LOAD_CREATE_TEXTURE( GL_ALPHA, GL_ALPHA, GL_UNSIGNED_BYTE, tex_temp2 );

                p_glActiveTextureARB( GL_TEXTURE0_ARB );
            }
            else
            {
                ConvertAP88to8888( (FxU16*)data, m_tex_temp, texVals.nPixels, m_palette );
                OGL_LOAD_CREATE_TEXTURE( 4, GL_BGRA_EXT, GL_UNSIGNED_BYTE, m_tex_temp );
            }
            break;
            
        case GR_TEXFMT_ALPHA_8:
            ConvertA8toAP88( (FxU8*)data, (FxU16*)m_tex_temp, texVals.nPixels );
            OGL_LOAD_CREATE_TEXTURE( 2, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, m_tex_temp );
            break;
            
        case GR_TEXFMT_ALPHA_INTENSITY_88:
            OGL_LOAD_CREATE_TEXTURE( 2, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, data );
            break;
            
        case GR_TEXFMT_INTENSITY_8:
            OGL_LOAD_CREATE_TEXTURE( 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, data );
            break;
            
        case GR_TEXFMT_ALPHA_INTENSITY_44:
#if 0
            OGL_LOAD_CREATE_TEXTURE( GL_LUMINANCE4_ALPHA4, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, data );
#else
            ConvertAI44toAP88( (FxU8*)data, (FxU16*)m_tex_temp, texVals.nPixels );
            glTexImage2D( GL_TEXTURE_2D, texVals.lod, 2, texVals.width, texVals.height, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, m_tex_temp );
            if ( InternalConfig.BuildMipMaps )
            {
                gluBuild2DMipmaps( GL_TEXTURE_2D, 2, texVals.width, texVals.height, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, m_tex_temp );
            }
#endif
            break;
            
        case GR_TEXFMT_8BIT: //GR_TEXFMT_RGB_332
            OGL_LOAD_CREATE_TEXTURE( 3, GL_RGB, GL_UNSIGNED_BYTE_3_3_2_EXT, data );
            break;
            
        case GR_TEXFMT_16BIT: //GR_TEXFMT_ARGB_8332:
            Convert8332to8888( (FxU16*)data, m_tex_temp, texVals.nPixels );
            OGL_LOAD_CREATE_TEXTURE( 4, GL_RGBA, GL_UNSIGNED_BYTE, m_tex_temp );
            break;
            
        case GR_TEXFMT_YIQ_422:
            ConvertYIQto8888( (FxU8*)data, m_tex_temp, texVals.nPixels, &(m_ncc[m_ncc_select]) );
            OGL_LOAD_CREATE_TEXTURE( 4, GL_RGBA, GL_UNSIGNED_BYTE, m_tex_temp );
            break;
            
        case GR_TEXFMT_AYIQ_8422:
            ConvertAYIQto8888( (FxU16*)data, m_tex_temp, texVals.nPixels, &(m_ncc[m_ncc_select]) );
            OGL_LOAD_CREATE_TEXTURE( 4, GL_RGBA, GL_UNSIGNED_BYTE, m_tex_temp );
            break;
            
        case GR_TEXFMT_RSVD0:
        case GR_TEXFMT_RSVD1:
        case GR_TEXFMT_RSVD2:
            Error( "grTexDownloadMipMapLevel - Unsupported format(%d)\n", m_info.format );
            memset( m_tex_temp, 255, texVals.nPixels * 2 );
            OGL_LOAD_CREATE_TEXTURE( 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, m_tex_temp );
            break;
        }
    }

    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, OpenGL.SClampMode );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, OpenGL.TClampMode );
    
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, OpenGL.MinFilterMode );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, OpenGL.MagFilterMode );
    
    if ( use_two_textures )
    {
        p_glActiveTextureARB( GL_TEXTURE1_ARB );
        
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, OpenGL.SClampMode );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, OpenGL.TClampMode );
        
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, OpenGL.MinFilterMode );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, OpenGL.MagFilterMode );
        
        p_glActiveTextureARB( GL_TEXTURE0_ARB );
    }

    return use_two_textures;
}

FxU32 PGTexture::LodOffset( FxU32 evenOdd, GrTexInfo *info )
{
    FxU32   total = 0;
    GrLOD_t i;

    for( i = info->largeLod; i < info->smallLod; i++ )
    {
        total += MipMapMemRequired( i, info->aspectRatio, info->format );
    }

    total = ( total + 7 ) & ~7;

    return total;
}

FxU32 PGTexture::TextureMemRequired( FxU32 evenOdd, GrTexInfo *info )
{
    //
    // If the format is one of these:
    // GR_TEXFMT_RGB_332, GR_TEXFMT_YIQ_422, GR_TEXFMT_ALPHA_8
    // GR_TEXFMT_INTENSITY_8, GR_TEXFMT_ALPHA_INTENSITY_44, GR_TEXFMT_P_8
    // Reduces the size by 2
    //
    
    // The extension format doesn't use Glide texture memory, but its
    // convenient to pretend it does because of the way TexDB works.
    if( info->format == GR_TEXFMT_BGRA_8888)
        return 2048;

    return nSquareTexLod[ info->format <= GR_TEXFMT_RSVD1 ][ info->aspectRatio ][ info->largeLod ][ info->smallLod ];
}

FxU32 PGTexture::MipMapMemRequired( GrLOD_t lod, GrAspectRatio_t aspectRatio, GrTextureFormat_t format )
{
    //
    // If the format is one of these:
    // GR_TEXFMT_RGB_332, GR_TEXFMT_YIQ_422, GR_TEXFMT_ALPHA_8
    // GR_TEXFMT_INTENSITY_8, GR_TEXFMT_ALPHA_INTENSITY_44, GR_TEXFMT_P_8
    // Reduces the size by 2
    //

    // The extension format doesn't use Glide texture memory, but its
    // convenient to pretend it does because of the way TexDB works.
    if( format == GR_TEXFMT_BGRA_8888)
        return 2048;

    return nSquareLod[ format > GR_TEXFMT_RSVD1 ][ aspectRatio ][ lod ];
}

void PGTexture::GetTexValues( TexValues * tval )
{
    tval->width = texInfo[ m_info.aspectRatio ][ m_info.largeLod ].width;
    tval->height = texInfo[ m_info.aspectRatio ][ m_info.largeLod ].height;
    tval->nPixels = texInfo[ m_info.aspectRatio ][ m_info.largeLod ].numPixels;
    tval->lod = 0;
}

void PGTexture::Clear( void )
{
    m_db->Clear( );
}

void PGTexture::GetAspect( float *hAspect, float *wAspect )
{
    *hAspect = m_hAspect;
    *wAspect = m_wAspect;
}

void PGTexture::ChromakeyValue( GrColor_t value )
{
    m_chromakey_value_8888 = value & 0x00ffffff;
    m_chromakey_value_565  = (FxU16)( ( value & 0x00F80000 ) >> 8 |
                                      ( value & 0x0000FC00 ) >> 5 |
                                      ( value & 0x000000F8 ) >> 3 );
    m_palette_dirty = true;
}

void PGTexture::ChromakeyMode( GrChromakeyMode_t mode )
{
    m_chromakey_mode = mode;
    m_palette_dirty = true;
}

void PGTexture::ApplyKeyToPalette( void )
{
    FxU32   hash;
    int     i;
    
    if ( m_palette_dirty )
    {
        hash = 0;
        for ( i = 0; i < 256; i++ )
        {
            if ( ( m_chromakey_mode ) && 
                 ( ( m_palette[i] & 0x00ffffff ) == m_chromakey_value_8888 ) )
            {
                m_palette[i] = TexDiagKeyMark( ) ? 0xffff00ff : ( m_palette[i] & 0x00ffffff );
            }
            else
            {
                m_palette[i] |= 0xff000000;
            }
            
            hash = ( ( hash << 5 ) | ( hash >> 27 ) );
            hash += ( InternalConfig.IgnorePaletteChange
                      ? ( m_palette[ i ] & 0xff000000  )
                      : m_palette[ i ]);
        }
        
        m_palette_hash = hash;
        m_palette_dirty = false;
    }
}

void PGTexture::NCCTable( GrNCCTable_t tab )
{
    switch ( tab )
    {
    case GR_NCCTABLE_NCC0:
    case GR_NCCTABLE_NCC1:
        m_ncc_select = tab;
    }
}

FxU32 PGTexture::GetMemorySize( void )
{
    return m_tex_memory_size;
}
