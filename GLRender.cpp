//**************************************************************
//*            OpenGLide - Glide to OpenGL Wrapper
//*             http://openglide.sourceforge.net
//*
//*                     Render File
//*
//*         OpenGLide is OpenSource under LGPL license
//*              Originaly made by Fabio Barros
//*      Modified by Paul for Glidos (http://www.glidos.net)
//*               Linux version by Simon White
//**************************************************************

#include "GlOgl.h"
#include "GLRender.h"
#include "GLExtensions.h"
#include "PGTexture.h"

//**************************************************************
// Defines
//**************************************************************

#define DEBUG_MIN_MAX( var, maxvar, minvar )    \
    if ( var > maxvar ) maxvar = var;           \
    if ( var < minvar ) minvar = var;

//**************************************************************
// extern variables and functions prototypes
//**************************************************************

//**************************************************************
// Local variables
//**************************************************************

// The functions for the color combine
ALPHAFACTORFUNCPROC AlphaFactorFunc;
COLORFACTORFUNCPROC ColorFactor3Func;
COLORFUNCTIONPROC   ColorFunctionFunc;

// Snapping constant
static const float vertex_snap_compare = 4096.0f;
static const float vertex_snap = float( 3L << 18 );

// Standard structs for the render
RenderStruct OGLRender;

// Varibles for the Add functions
static TColorStruct     Local, 
                        Other, 
                        CFactor;
static float            AFactor[3];
static TColorStruct     *pC,
                        *pC2;
static TVertexStruct    *pV;
static TTextureStruct   *pTS;
static TFogStruct       *pF;
static void             *pt1, 
                        *pt2, 
                        *pt3;
static float            atmuoow;
static float            btmuoow;
static float            ctmuoow;
static float            aoow, 
                        boow, 
                        coow;
static float            hAspect, 
                        wAspect,
                        maxoow;

// The texture stage: TMU1, then TMU0, as a Voodoo2 combines them, in the TMU's 8-bit integer arithmetic with the factor plus one,
// truncation, clamping and the inversion last. TMU0's output then takes the texture's place in what the fixed pipeline did with it:
// the texture environment grColorCombine chose, the secondary colour, the fog.
static const char *textureStageSource =
    "#version 120\n"
    "uniform sampler2D tmu0Texture;\n"
    "uniform sampler2D tmu1Texture;\n"
    "uniform vec2 tmuTextureValid;\n"
    // Per TMU and channel -- TMU0 RGB, TMU0 alpha, TMU1 RGB, TMU1 alpha: zero other, subtract local, add local, add local alpha.
    "uniform vec4 tmuFunction[4];\n"
    // The same four: factor select, reverse blend, invert.
    "uniform vec4 tmuFactor[4];\n"
    // Per TMU: S and T into Glide's texel space, the LOD bias, the detail maximum 0..255.
    "uniform vec4 tmuLod[2];\n"
    // Per TMU: the smallest and the largest LOD number it may use, the detail bias, 2 to the power of the detail scale.
    "uniform vec4 tmuLodRange[2];\n"
    "uniform float textureEnvMode;\n"
    "uniform float colorSum;\n"
    "uniform float fogEnabled;\n"
    "const float channelMaximum = 255.0;\n"
    "const float factorScale = 256.0;\n"
    "const float lodFixedPoint = 256.0;\n"
    "const float smallestFootprint = 1.0e-20;\n"
    "float CombineChannel(vec4 function, vec4 factorFields, float other, float local, float localAlpha, float factor)\n"
    "{\n"
    "    float selectedOther = function.x > 0.5 ? 0.0 : other;\n"
    "    float difference = selectedOther - (function.y > 0.5 ? local : 0.0);\n"
    "    float effectiveFactor = factorFields.y > 0.5 ? factor : channelMaximum - factor;\n"
    "    float product = floor(difference * (effectiveFactor + 1.0) / factorScale);\n"
    "    float addend = function.z > 0.5 ? local : (function.w > 0.5 ? localAlpha : 0.0);\n"
    "    float result = clamp(product + addend, 0.0, channelMaximum);\n"
    "    return factorFields.z > 0.5 ? channelMaximum - result : result;\n"
    "}\n"
    "float FactorValue(float select, float channelLocal, float localAlpha, float otherAlpha, vec2 lodFactors)\n"
    "{\n"
    "    if (select < 0.5) {\n"
    "        return 0.0;\n"
    "    }\n"
    "    if (select < 1.5) {\n"
    "        return channelLocal;\n"
    "    }\n"
    "    if (select < 2.5) {\n"
    "        return otherAlpha;\n"
    "    }\n"
    "    if (select < 3.5) {\n"
    "        return localAlpha;\n"
    "    }\n"
    "    if (select < 4.5) {\n"
    "        return lodFactors.x;\n"
    "    }\n"
    "    if (select < 5.5) {\n"
    "        return lodFactors.y;\n"
    "    }\n"
    "    return 0.0;\n"
    "}\n"
    // The detail factor and the LOD fraction. The LOD in 8.8 fixed point as the TMU keeps it: the texel footprint in Glide's texel
    // space, plus the bias, clamped to the TMU's levels.
    "vec2 LodFactors(vec4 coordinate, vec4 lod, vec4 lodRange)\n"
    "{\n"
    "    vec2 texelPosition = coordinate.st / coordinate.q * lod.xy;\n"
    "    float footprint = max(length(dFdx(texelPosition)), length(dFdy(texelPosition)));\n"
    "    float level = clamp(log2(max(footprint, smallestFootprint)) + lod.z, lodRange.x, lodRange.y);\n"
    "    float fixedLevel = floor(level * lodFixedPoint);\n"
    "    float lodFraction = fixedLevel - floor(fixedLevel / lodFixedPoint) * lodFixedPoint;\n"
    "    float detailBias = lodRange.z * lodFixedPoint;\n"
    "    float detail = 0.0;\n"
    "    if (detailBias > fixedLevel) {\n"
    "        detail = min(lod.w, floor((detailBias - fixedLevel) * lodRange.w / factorScale));\n"
    "    }\n"
    "    return vec2(detail, lodFraction);\n"
    "}\n"
    "vec4 TmuOutput(vec4 texel, vec4 other, vec4 rgbFunction, vec4 rgbFactor, vec4 alphaFunction, vec4 alphaFactor, vec2 lodFactors)\n"
    "{\n"
    "    vec4 result;\n"
    "    for (int channel = 0; channel < 3; channel++) {\n"
    "        float factor = FactorValue(rgbFactor.x, texel[channel], texel.a, other.a, lodFactors);\n"
    "        result[channel] = CombineChannel(rgbFunction, rgbFactor, other[channel], texel[channel], texel.a, factor);\n"
    "    }\n"
    "    float alphaFactorValue = FactorValue(alphaFactor.x, texel.a, texel.a, other.a, lodFactors);\n"
    "    result.a = CombineChannel(alphaFunction, alphaFactor, other.a, texel.a, texel.a, alphaFactorValue);\n"
    "    return result;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec2 tmu1LodFactors = LodFactors(gl_TexCoord[1], tmuLod[1], tmuLodRange[1]);\n"
    "    vec2 tmu0LodFactors = LodFactors(gl_TexCoord[0], tmuLod[0], tmuLodRange[0]);\n"
    "    vec4 tmu1Sample = texture2DProj(tmu1Texture, gl_TexCoord[1]);\n"
    "    vec4 tmu0Sample = texture2DProj(tmu0Texture, gl_TexCoord[0]);\n"
    "    vec4 tmu1Texel = floor(tmu1Sample * channelMaximum + 0.5) * tmuTextureValid.y;\n"
    "    vec4 tmu0Texel = floor(tmu0Sample * channelMaximum + 0.5) * tmuTextureValid.x;\n"
    "    vec4 upstream = vec4(0.0);\n"
    "    vec4 tmu1Output = TmuOutput(tmu1Texel, upstream, tmuFunction[2], tmuFactor[2], tmuFunction[3], tmuFactor[3], tmu1LodFactors);\n"
    "    vec4 tmu0Output = TmuOutput(tmu0Texel, tmu1Output, tmuFunction[0], tmuFactor[0], tmuFunction[1], tmuFactor[1], tmu0LodFactors);\n"
    "    vec4 textureColor = tmu0Output / channelMaximum;\n"
    "    vec4 primary = gl_Color;\n"
    "    vec4 color;\n"
    "    if (textureEnvMode < 0.5) {\n"
    "        color = primary * textureColor;\n"
    "    } else if (textureEnvMode < 1.5) {\n"
    "        color = vec4(mix(primary.rgb, textureColor.rgb, textureColor.a), primary.a);\n"
    "    } else {\n"
    "        color = vec4(mix(primary.rgb, gl_TextureEnvColor[0].rgb, textureColor.rgb), primary.a * textureColor.a);\n"
    "    }\n"
    "    color.rgb = min(color.rgb + gl_SecondaryColor.rgb * colorSum, 1.0);\n"
    "    if (fogEnabled > 0.5) {\n"
    "        float fogFactor = clamp((gl_Fog.end - gl_FogFragCoord) * gl_Fog.scale, 0.0, 1.0);\n"
    "        color.rgb = mix(gl_Fog.color.rgb, color.rgb, fogFactor);\n"
    "    }\n"
    "    gl_FragColor = color;\n"
    "}\n";

// The texture environment modes as the shader numbers them.
static const GLfloat textureEnvModulate = 0.0f;
static const GLfloat textureEnvDecal = 1.0f;
static const GLfloat textureEnvBlend = 2.0f;
// Glide's combine factor: the source in the low three bits, "one minus" in the fourth.
static const FxU32 combineFactorSelectMask = 0x7;
static const FxU32 combineFactorInvertBit = 0x8;
static const GLfloat channelMaximum = 255.0f;
// One combine unit's channel in tmuFunction and tmuFactor: four values each.
static const int combineFieldCount = 4;
static const int combineUnitsPerTmu = 2;
static const int textureStageLogSize = 1024;
// grTexLodBiasValue as the TMU keeps it: signed quarters of a level, rounded the way 3dfx's Glide rounds.
static const float lodBiasQuarter = 0.25f;
static const float lodBiasRounding = 0.125f;

struct TextureStageProgram
{
    GLuint  program;
    GLint   tmuTextureValid;
    GLint   tmuFunction;
    GLint   tmuFactor;
    GLint   tmuLod;
    GLint   tmuLodRange;
    GLint   textureEnvMode;
    GLint   colorSum;
    GLint   fogEnabled;
};

static TextureStageProgram textureStage;

//**************************************************************
// Functions definitions
//**************************************************************

static GLuint CompileTextureStage( void )
{
    GLuint shader = p_glCreateShader( GL_FRAGMENT_SHADER );
    const GLchar *source = textureStageSource;
    p_glShaderSource( shader, 1, &source, NULL );
    p_glCompileShader( shader );

    GLint compiled = GL_FALSE;
    p_glGetShaderiv( shader, GL_COMPILE_STATUS, &compiled );
    if ( compiled != GL_TRUE )
    {
        GLchar log[ textureStageLogSize ];
        p_glGetShaderInfoLog( shader, textureStageLogSize, NULL, log );
        Error( "The texture stage's shader does not compile, TMU0 draws alone: %s\n", log );
        p_glDeleteShader( shader );
        return 0;
    }
    return shader;
}

void RenderCreateTextureStage( void )
{
    OpenGL.TextureStageShader = false;
    textureStage.program = 0;

    bool haveGlsl = ( p_glCreateShader != NULL ) && ( p_glShaderSource != NULL ) && ( p_glCompileShader != NULL ) &&
                    ( p_glGetShaderiv != NULL ) && ( p_glGetShaderInfoLog != NULL ) && ( p_glDeleteShader != NULL ) &&
                    ( p_glCreateProgram != NULL ) && ( p_glAttachShader != NULL ) && ( p_glLinkProgram != NULL ) &&
                    ( p_glGetProgramiv != NULL ) && ( p_glGetProgramInfoLog != NULL ) && ( p_glDeleteProgram != NULL ) &&
                    ( p_glUseProgram != NULL ) && ( p_glGetUniformLocation != NULL ) && ( p_glUniform1i != NULL ) &&
                    ( p_glUniform1f != NULL ) && ( p_glUniform2f != NULL ) && ( p_glUniform4fv != NULL );
    if ( !haveGlsl )
    {
        GlideMsg( "No GLSL or fewer than two texture units: TMU0 draws alone, its texel as it is\n" );
        return;
    }

    GLuint shader = CompileTextureStage( );
    if ( shader == 0 )
    {
        return;
    }

    GLuint program = p_glCreateProgram( );
    p_glAttachShader( program, shader );
    p_glLinkProgram( program );
    p_glDeleteShader( shader );

    GLint linked = GL_FALSE;
    p_glGetProgramiv( program, GL_LINK_STATUS, &linked );
    if ( linked != GL_TRUE )
    {
        GLchar log[ textureStageLogSize ];
        p_glGetProgramInfoLog( program, textureStageLogSize, NULL, log );
        Error( "The texture stage's shader does not link, TMU0 draws alone: %s\n", log );
        p_glDeleteProgram( program );
        return;
    }

    textureStage.program = program;
    textureStage.tmuTextureValid = p_glGetUniformLocation( program, "tmuTextureValid" );
    textureStage.tmuFunction = p_glGetUniformLocation( program, "tmuFunction" );
    textureStage.tmuFactor = p_glGetUniformLocation( program, "tmuFactor" );
    textureStage.tmuLod = p_glGetUniformLocation( program, "tmuLod" );
    textureStage.tmuLodRange = p_glGetUniformLocation( program, "tmuLodRange" );
    textureStage.textureEnvMode = p_glGetUniformLocation( program, "textureEnvMode" );
    textureStage.colorSum = p_glGetUniformLocation( program, "colorSum" );
    textureStage.fogEnabled = p_glGetUniformLocation( program, "fogEnabled" );

    GLint tmu0Sampler = p_glGetUniformLocation( program, "tmu0Texture" );
    GLint tmu1Sampler = p_glGetUniformLocation( program, "tmu1Texture" );
    p_glUseProgram( program );
    p_glUniform1i( tmu0Sampler, GR_TMU0 );
    p_glUniform1i( tmu1Sampler, GR_TMU1 );
    p_glUseProgram( 0 );

    OpenGL.TextureStageShader = true;
}

void RenderDeleteTextureStage( void )
{
    if ( textureStage.program != 0 )
    {
        p_glDeleteProgram( textureStage.program );
        textureStage.program = 0;
    }
    OpenGL.TextureStageShader = false;
}

// The fixed pipeline draws TMU0's texel as it is. Anything else -- another function, an inversion, TMU1 -- goes through the shader.
static bool TextureStageActive( void )
{
    if ( !OpenGL.TextureStageShader || !OpenGL.Texture )
    {
        return false;
    }
    const TmuState *tmu0 = &Glide.State.Tmu[ GR_TMU0 ];
    bool rgbIsTexel = ( tmu0->CombineCFunction == GR_COMBINE_FUNCTION_LOCAL ) && !tmu0->CombineRGBInvert;
    bool alphaIsTexel = ( tmu0->CombineAFunction == GR_COMBINE_FUNCTION_LOCAL ) && !tmu0->CombineAInvert;
    return !( rgbIsTexel && alphaIsTexel );
}

// One channel of a TMU's combine unit as the fields Glide's grTexCombine writes: tmuFunction's four, then tmuFactor's four.
static void CombineUnitFields( FxU32 function, FxU32 factor, FxU32 invert, GLfloat *functionFields, GLfloat *factorFields )
{
    bool zeroOther = false;
    bool subtractLocal = false;
    bool addLocal = false;
    bool addLocalAlpha = false;
    switch ( function )
    {
    case GR_COMBINE_FUNCTION_LOCAL:
        zeroOther = true;
        addLocal = true;
        break;
    case GR_COMBINE_FUNCTION_LOCAL_ALPHA:
        zeroOther = true;
        addLocalAlpha = true;
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER:
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL:
        addLocal = true;
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA:
        addLocalAlpha = true;
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL:
        subtractLocal = true;
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL:
        subtractLocal = true;
        addLocal = true;
        break;
    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        subtractLocal = true;
        addLocalAlpha = true;
        break;
    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL:
        zeroOther = true;
        subtractLocal = true;
        addLocal = true;
        break;
    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        zeroOther = true;
        subtractLocal = true;
        addLocalAlpha = true;
        break;
    default:
        // GR_COMBINE_FUNCTION_ZERO, and what Glide does not know: nothing passes.
        zeroOther = true;
        break;
    }
    functionFields[ 0 ] = zeroOther ? 1.0f : 0.0f;
    functionFields[ 1 ] = subtractLocal ? 1.0f : 0.0f;
    functionFields[ 2 ] = addLocal ? 1.0f : 0.0f;
    functionFields[ 3 ] = addLocalAlpha ? 1.0f : 0.0f;
    factorFields[ 0 ] = (GLfloat)( factor & combineFactorSelectMask );
    factorFields[ 1 ] = ( ( factor & combineFactorInvertBit ) == 0 ) ? 1.0f : 0.0f;
    factorFields[ 2 ] = invert ? 1.0f : 0.0f;
    factorFields[ 3 ] = 0.0f;
}

// What the shader needs of a TMU's LOD: tmuLod's and tmuLodRange's four values.
static void TmuLodFields( int tmu, GLfloat *lod, GLfloat *lodRange )
{
    const TmuState *tmuState = &Glide.State.Tmu[ tmu ];
    float tmuHAspect;
    float tmuWAspect;
    Textures[ tmu ]->GetAspect( &tmuHAspect, &tmuWAspect );

    int lodBiasQuarters = (int)( ( tmuState->LodBias + lodBiasRounding ) / lodBiasQuarter );
    // grTexDetailControl's maximum as Glide packs it: 0..1 to 8 bits, cut off.
    FxU32 detailMaximum = (FxU32)( tmuState->DetailMax * channelMaximum );
    bool mipmapping = ( tmuState->MipMapMode != GR_MIPMAP_DISABLE );

    lod[ 0 ] = 1.0f / tmuWAspect;
    lod[ 1 ] = 1.0f / tmuHAspect;
    lod[ 2 ] = (GLfloat)lodBiasQuarters * lodBiasQuarter;
    lod[ 3 ] = (GLfloat)detailMaximum;
    lodRange[ 0 ] = (GLfloat)tmuState->LargeLod;
    lodRange[ 1 ] = (GLfloat)( mipmapping ? tmuState->SmallLod : tmuState->LargeLod );
    lodRange[ 2 ] = (GLfloat)tmuState->DetailBias;
    lodRange[ 3 ] = (GLfloat)( 1 << tmuState->DetailScale );
}

// Binds TMU1's texture to GL unit 1 -- TMU0's is on unit 0 already -- and draws through the shader until EndTextureStage.
static void BeginTextureStage( void )
{
    PGTexture *tmu0Texture = Textures[ GR_TMU0 ];
    PGTexture *tmu1Texture = Textures[ GR_TMU1 ];
    bool tmu0Valid = tmu0Texture->IsValid( );
    bool tmu1Valid = tmu1Texture->IsValid( );
    if ( tmu1Valid )
    {
        p_glActiveTextureARB( GL_TEXTURE1_ARB );
        tmu1Texture->MakeReady( );
        p_glActiveTextureARB( GL_TEXTURE0_ARB );
    }

    GLfloat functionFields[ OGL_TMU_COUNT * combineUnitsPerTmu * combineFieldCount ];
    GLfloat factorFields[ OGL_TMU_COUNT * combineUnitsPerTmu * combineFieldCount ];
    GLfloat lodFields[ OGL_TMU_COUNT * combineFieldCount ];
    GLfloat lodRangeFields[ OGL_TMU_COUNT * combineFieldCount ];
    for ( int tmu = GR_TMU0; tmu < OGL_TMU_COUNT; tmu++ )
    {
        const TmuState *tmuState = &Glide.State.Tmu[ tmu ];
        int rgbOffset = ( tmu * combineUnitsPerTmu ) * combineFieldCount;
        int alphaOffset = rgbOffset + combineFieldCount;
        int lodOffset = tmu * combineFieldCount;
        CombineUnitFields( tmuState->CombineCFunction, tmuState->CombineCFactor, tmuState->CombineRGBInvert,
                           &functionFields[ rgbOffset ], &factorFields[ rgbOffset ] );
        CombineUnitFields( tmuState->CombineAFunction, tmuState->CombineAFactor, tmuState->CombineAInvert,
                           &functionFields[ alphaOffset ], &factorFields[ alphaOffset ] );
        TmuLodFields( tmu, &lodFields[ lodOffset ], &lodRangeFields[ lodOffset ] );
    }

    GLfloat environmentMode = textureEnvModulate;
    if ( OpenGL.TextureEnvMode == GL_DECAL )
    {
        environmentMode = textureEnvDecal;
    }
    else if ( OpenGL.TextureEnvMode == GL_BLEND )
    {
        environmentMode = textureEnvBlend;
    }
    bool fogEnabled = OpenGL.Fog && InternalConfig.EXT_fog_coord;

    p_glUseProgram( textureStage.program );
    p_glUniform2f( textureStage.tmuTextureValid, tmu0Valid ? 1.0f : 0.0f, tmu1Valid ? 1.0f : 0.0f );
    p_glUniform4fv( textureStage.tmuFunction, OGL_TMU_COUNT * combineUnitsPerTmu, functionFields );
    p_glUniform4fv( textureStage.tmuFactor, OGL_TMU_COUNT * combineUnitsPerTmu, factorFields );
    p_glUniform4fv( textureStage.tmuLod, OGL_TMU_COUNT, lodFields );
    p_glUniform4fv( textureStage.tmuLodRange, OGL_TMU_COUNT, lodRangeFields );
    p_glUniform1f( textureStage.textureEnvMode, environmentMode );
    p_glUniform1f( textureStage.colorSum, InternalConfig.EXT_secondary_color ? 1.0f : 0.0f );
    p_glUniform1f( textureStage.fogEnabled, fogEnabled ? 1.0f : 0.0f );
}

static void EndTextureStage( void )
{
    p_glUseProgram( 0 );
}

// TMU1's coordinates of one vertex as 3dfx's Glide hands them to the TMU: its own S and T only with GR_STWHINT_ST_DIFF_TMU1, its own W
// only with GR_STWHINT_W_DIFF_TMU1, otherwise TMU0's. S and T are scaled into GL's 0..1 like TMU0's, W goes to q.
static void Tmu1Coordinate( const GrVertex *vertex, float tmu0Oow, float tmu1HAspect, float tmu1WAspect, GLfloat *coordinate )
{
    bool ownST = ( Glide.State.STWHint & GR_STWHINT_ST_DIFF_TMU1 ) != 0;
    bool ownW = ( Glide.State.STWHint & GR_STWHINT_W_DIFF_TMU1 ) != 0;
    const GrTmuVertex *stVertex = &vertex->tmuvtx[ ownST ? GR_TMU1 : GR_TMU0 ];
    coordinate[ 0 ] = stVertex->sow * tmu1WAspect;
    coordinate[ 1 ] = stVertex->tow * tmu1HAspect;
    coordinate[ 2 ] = 0.0f;
    coordinate[ 3 ] = ownW ? vertex->tmuvtx[ GR_TMU1 ].oow : tmu0Oow;
}

// TMU1's coordinates of a triangle on GL unit 1, divided by the largest W like TMU0's, so GL's interpolation stays in range.
static void Tmu1TriangleCoordinates( TTextureStruct *coordinates, const GrVertex *a, const GrVertex *b, const GrVertex *c,
                                     float aTmu0Oow, float bTmu0Oow, float cTmu0Oow )
{
    const int coordinateCount = sizeof( TTextureStruct ) / sizeof( GLfloat );
    float tmu1HAspect;
    float tmu1WAspect;
    Textures[ GR_TMU1 ]->GetAspect( &tmu1HAspect, &tmu1WAspect );
    Tmu1Coordinate( a, aTmu0Oow, tmu1HAspect, tmu1WAspect, &coordinates->as );
    Tmu1Coordinate( b, bTmu0Oow, tmu1HAspect, tmu1WAspect, &coordinates->bs );
    Tmu1Coordinate( c, cTmu0Oow, tmu1HAspect, tmu1WAspect, &coordinates->cs );

    float largestOow = max( coordinates->aoow, max( coordinates->boow, coordinates->coow ) );
    float scale = 1.0f / largestOow;
    GLfloat *values = &coordinates->as;
    for ( int index = 0; index < coordinateCount; index++ )
    {
        values[ index ] *= scale;
    }
}

// Intializes the render and allocates memory
void RenderInitialize( void )
{
    OGLRender.NumberOfTriangles = 0;

    OGLRender.TColor    = new TColorStruct[ MAXTRIANGLES + 1 ];
    OGLRender.TColor2   = new TColorStruct[ MAXTRIANGLES + 1 ];
    OGLRender.TTexture  = new TTextureStruct[ MAXTRIANGLES + 1 ];
    OGLRender.TTexture1 = new TTextureStruct[ MAXTRIANGLES + 1 ];
    OGLRender.TVertex   = new TVertexStruct[ MAXTRIANGLES + 1 ];
    OGLRender.TFog      = new TFogStruct[ MAXTRIANGLES + 1 ];

#ifdef OGL_DEBUG
    OGLRender.FrameTriangles = 0;
    OGLRender.MaxTriangles = 0;
    OGLRender.MaxSequencedTriangles = 0;
    OGLRender.MinX = OGLRender.MinY = OGLRender.MinZ = OGLRender.MinW = 99999999.0f;
    OGLRender.MaxX = OGLRender.MaxY = OGLRender.MaxZ = OGLRender.MaxW = -99999999.0f;
    OGLRender.MinS = OGLRender.MinT = OGLRender.MinF = 99999999.0f;
    OGLRender.MaxS = OGLRender.MaxT = OGLRender.MaxF = -99999999.0f;

    OGLRender.MinR = OGLRender.MinG = OGLRender.MinB = OGLRender.MinA = 99999999.0f;
    OGLRender.MaxR = OGLRender.MaxG = OGLRender.MaxB = OGLRender.MaxA = -99999999.0f;
#endif
}

// Shutdowns the render and frees memory
void RenderFree( void )
{
    delete[] OGLRender.TColor;
    delete[] OGLRender.TColor2;
    delete[] OGLRender.TTexture;
    delete[] OGLRender.TTexture1;
    delete[] OGLRender.TVertex;
    delete[] OGLRender.TFog;
}

void RenderUpdateArrays( void )
{
    glVertexPointer( 3, GL_FLOAT, 4 * sizeof( GLfloat ), &OGLRender.TVertex[0] );
    glColorPointer( 4, GL_FLOAT, 0, &OGLRender.TColor[0] );
    if ( InternalConfig.ARB_multitexture )
    {
        p_glClientActiveTexture( GL_TEXTURE0_ARB );
    }
    glTexCoordPointer( 4, GL_FLOAT, 0, &OGLRender.TTexture[0] );
    if ( InternalConfig.ARB_multitexture )
    {
        p_glClientActiveTexture( GL_TEXTURE1_ARB );
        glTexCoordPointer( 4, GL_FLOAT, 0, &OGLRender.TTexture1[0] );
        glEnableClientState( GL_TEXTURE_COORD_ARRAY );
        p_glClientActiveTexture( GL_TEXTURE0_ARB );
    }
    p_glSecondaryColorPointerEXT( 3, GL_FLOAT, 4 * sizeof( GLfloat ), &OGLRender.TColor2[0] );
    if ( InternalConfig.EXT_fog_coord )
    {
        p_glFogCoordPointerEXT( GL_FLOAT, 0, &OGLRender.TFog[0] );
    }

#ifdef OPENGL_DEBUG
    GLErro( "Render::UpdateArrays" );
#endif
}

// Draw the current saved triangles
void RenderDrawTriangles( void )
{
    bool use_two_tex = false;
    bool texture_stage = false;

    if ( ! OGLRender.NumberOfTriangles )
    {
        return;
    }

    if ( OpenGL.Texture )
    {
        glEnable( GL_TEXTURE_2D );

        use_two_tex = Textures[ GR_TMU0 ]->MakeReady( );
        texture_stage = !use_two_tex && TextureStageActive( );

        if ( texture_stage )
        {
            BeginTextureStage( );
        }
        else if ( use_two_tex )
        {
            p_glActiveTextureARB( GL_TEXTURE1_ARB );

            glEnable( GL_TEXTURE_2D );

            p_glActiveTextureARB( GL_TEXTURE0_ARB );
        }
    }
    else
    {
        glDisable( GL_TEXTURE_2D );
    }

    if ( OpenGL.Blend )
    {
        glEnable( GL_BLEND );
    }
    else
    {
        glDisable( GL_BLEND );
    }

    // A Voodoo tests the combined alpha whatever its source; the test used to be dropped unless the alpha came from the texture.
    if ( Glide.State.AlphaTestFunction != GR_CMP_ALWAYS )
    {
        glEnable( GL_ALPHA_TEST );
    }
    else
    {
        glDisable( GL_ALPHA_TEST );
    }
    
    if( !OpenGL.Blend && Glide.State.ChromaKeyMode )
    {
        glAlphaFunc( GL_GEQUAL, 0.5 );
        glEnable( GL_ALPHA_TEST );
        
        glBegin( GL_TRIANGLES );
        for ( int i = 0; i < OGLRender.NumberOfTriangles; i++ )
        {
            glColor3fv( &OGLRender.TColor[ i ].ar );
            p_glSecondaryColor3fvEXT( &OGLRender.TColor2[ i ].ar );
            p_glFogCoordfEXT( OGLRender.TFog[ i ].af );
            glTexCoord4fv( &OGLRender.TTexture[ i ].as );
            if ( texture_stage )
            {
                p_glMultiTexCoord4fvARB( GL_TEXTURE1_ARB, &OGLRender.TTexture1[ i ].as );
            }
            glVertex3fv( &OGLRender.TVertex[ i ].ax );
            
            glColor3fv( &OGLRender.TColor[ i ].br );
            p_glSecondaryColor3fvEXT( &OGLRender.TColor2[ i ].br );
            p_glFogCoordfEXT( OGLRender.TFog[ i ].bf );
            glTexCoord4fv( &OGLRender.TTexture[ i ].bs );
            if ( texture_stage )
            {
                p_glMultiTexCoord4fvARB( GL_TEXTURE1_ARB, &OGLRender.TTexture1[ i ].bs );
            }
            glVertex3fv( &OGLRender.TVertex[ i ].bx );
            
            glColor3fv( &OGLRender.TColor[ i ].cr );
            p_glSecondaryColor3fvEXT( &OGLRender.TColor2[ i ].cr );
            p_glFogCoordfEXT( OGLRender.TFog[ i ].cf );
            glTexCoord4fv( &OGLRender.TTexture[ i ].cs );
            if ( texture_stage )
            {
                p_glMultiTexCoord4fvARB( GL_TEXTURE1_ARB, &OGLRender.TTexture1[ i ].cs );
            }
            glVertex3fv( &OGLRender.TVertex[ i ].cx );
        }
        glEnd( );
        
        glDisable( GL_ALPHA_TEST );
    }
    else
    {
        // The chroma key has to hold even while blending is on. On a Voodoo the
        // keyed texel never reaches the blending unit at all; here it is marked
        // with alpha 0 in the palette, so an alpha test has to drop it. Without
        // this it only worked for GR_BLEND_ONE/GR_BLEND_ZERO -- and Diablo II
        // draws its menu text with GR_BLEND_ZERO/GR_BLEND_SRC_COLOR, a multiply
        // in which a black texel with alpha 0 blackens the pixel underneath.
        // That was the black box behind the button text.
        bool chromaKeyAlphaTest = ( Glide.State.ChromaKeyMode != 0 );

        if ( chromaKeyAlphaTest )
        {
            glAlphaFunc( GL_GREATER, 0.0f );
            glEnable( GL_ALPHA_TEST );
        }

        if ( InternalConfig.EXT_vertex_array )
        {
            glDrawArrays( GL_TRIANGLES, 0, OGLRender.NumberOfTriangles * 3 );
        }
        else
        {
            glBegin( GL_TRIANGLES );
            for ( int i = 0; i < OGLRender.NumberOfTriangles; i++ )
            {
                glColor4fv( &OGLRender.TColor[ i ].ar );
                p_glSecondaryColor3fvEXT( &OGLRender.TColor2[ i ].ar );
                p_glFogCoordfEXT( OGLRender.TFog[ i ].af );
                glTexCoord4fv( &OGLRender.TTexture[ i ].as );
                if ( use_two_tex || texture_stage )
                {
                    p_glMultiTexCoord4fvARB( GL_TEXTURE1_ARB, &OGLRender.TTexture1[ i ].as );
                }
                glVertex3fv( &OGLRender.TVertex[ i ].ax );
                
                glColor4fv( &OGLRender.TColor[ i ].br );
                p_glSecondaryColor3fvEXT( &OGLRender.TColor2[ i ].br );
                p_glFogCoordfEXT( OGLRender.TFog[ i ].bf );
                glTexCoord4fv( &OGLRender.TTexture[ i ].bs );
                if ( use_two_tex || texture_stage )
                {
                    p_glMultiTexCoord4fvARB( GL_TEXTURE1_ARB, &OGLRender.TTexture1[ i ].bs );
                }
                glVertex3fv( &OGLRender.TVertex[ i ].bx );
                
                glColor4fv( &OGLRender.TColor[ i ].cr );
                p_glSecondaryColor3fvEXT( &OGLRender.TColor2[ i ].cr );
                p_glFogCoordfEXT( OGLRender.TFog[ i ].cf );
                glTexCoord4fv( &OGLRender.TTexture[ i ].cs );
                if ( use_two_tex || texture_stage )
                {
                    p_glMultiTexCoord4fvARB( GL_TEXTURE1_ARB, &OGLRender.TTexture1[ i ].cs );
                }
                glVertex3fv( &OGLRender.TVertex[ i ].cx );
            }
            glEnd( );
        }

        if ( chromaKeyAlphaTest )
        {
            glDisable( GL_ALPHA_TEST );
            // grAlphaTestFunction is the only other place that sets these, so
            // the game's own alpha test has to be put back by hand.
            glAlphaFunc( OpenGL.AlphaTestFunction, OpenGL.AlphaReferenceValue );
        }
    }

    if ( texture_stage )
    {
        EndTextureStage( );
    }
  
    if ( ! InternalConfig.EXT_secondary_color )
    {
        glBlendFunc( GL_ONE, GL_ONE );
        glEnable( GL_BLEND );
        glDisable( GL_TEXTURE_2D );

        if ( OpenGL.DepthBufferType )
        {
            glPolygonOffset( 1.0f, 0.5f );
        }
        else
        {
            glPolygonOffset( -1.0f, -0.5f );
        }

        glEnable( GL_POLYGON_OFFSET_FILL );

        if ( InternalConfig.EXT_vertex_array )
        {
            glColorPointer( 4, GL_FLOAT, 0, &OGLRender.TColor2 );
            glDrawArrays( GL_TRIANGLES, 0, OGLRender.NumberOfTriangles * 3 );
            glColorPointer( 4, GL_FLOAT, 0, &OGLRender.TColor );
        }
        else
        {
            glBegin( GL_TRIANGLES );
            for ( int i = 0; i < OGLRender.NumberOfTriangles; i++ )
            {
                glColor4fv( &OGLRender.TColor2[ i ].ar );
                glVertex3fv( &OGLRender.TVertex[ i ].ax );

                glColor4fv( &OGLRender.TColor2[ i ].br );
                glVertex3fv( &OGLRender.TVertex[ i ].bx );

                glColor4fv( &OGLRender.TColor2[ i ].cr );
                glVertex3fv( &OGLRender.TVertex[ i ].cx );
            }
            glEnd( );
        }

        if ( Glide.State.DepthBiasLevel )
        {
            glPolygonOffset( 1.0f, OpenGL.DepthBiasLevel );
        }
        else
        {
            glDisable( GL_POLYGON_OFFSET_FILL );
        }

        if ( OpenGL.Blend )
        {
            glBlendFunc( OpenGL.SrcBlend, OpenGL.DstBlend );
        }
    }

    if ( use_two_tex )
    {
        p_glActiveTextureARB( GL_TEXTURE1_ARB );

        glDisable( GL_TEXTURE_2D );

        p_glActiveTextureARB( GL_TEXTURE0_ARB );
    }

#ifdef OGL_DEBUG
    if ( OGLRender.NumberOfTriangles > OGLRender.MaxSequencedTriangles )
    {
        OGLRender.MaxSequencedTriangles = OGLRender.NumberOfTriangles;
    }

    GLErro( "Render::DrawTriangles" );
#endif

    OGLRender.NumberOfTriangles = 0;
}


// Primary and secondary colour for a sum of 0..2: each stays inside 0..1, the sum stays exact.
static inline void SplitColorSum( float &primary, float &secondary )
{
    float sum = primary + secondary;
    primary = ( sum < 0.0f ) ? 0.0f : ( ( sum > 1.0f ) ? 1.0f : sum );
    secondary = ( sum > 1.0f ) ? sum - 1.0f : 0.0f;
}

void RenderAddTriangle( const GrVertex *a, const GrVertex *b, const GrVertex *c, bool unsnap )
{
    pC = &OGLRender.TColor[ OGLRender.NumberOfTriangles ];
    pC2 = &OGLRender.TColor2[ OGLRender.NumberOfTriangles ];
    pV = &OGLRender.TVertex[ OGLRender.NumberOfTriangles ];
    pTS = &OGLRender.TTexture[ OGLRender.NumberOfTriangles ];

    ZeroMemory( pC2, sizeof( TColorStruct ) );

    if ( ( Glide.State.STWHint & GR_STWHINT_W_DIFF_TMU0 ) == 0 )
    {
        atmuoow = a->oow;
        btmuoow = b->oow;
        ctmuoow = c->oow;
    }
    else
    {
        atmuoow = a->tmuvtx[ 0 ].oow;
        btmuoow = b->tmuvtx[ 0 ].oow;
        ctmuoow = c->tmuvtx[ 0 ].oow;
    }

    if ( Glide.ALocal )
    {
        switch ( Glide.State.AlphaLocal )
        {
        case GR_COMBINE_LOCAL_ITERATED:
            Local.aa = a->a * D1OVER255;
            Local.ba = b->a * D1OVER255;
            Local.ca = c->a * D1OVER255;
            break;

        case GR_COMBINE_LOCAL_CONSTANT:
            Local.aa = Local.ba = Local.ca = OpenGL.ConstantColor[ 3 ];
            break;

        case GR_COMBINE_LOCAL_DEPTH:
            Local.aa = a->z * D1OVER255;
            Local.ba = b->z * D1OVER255;
            Local.ca = c->z * D1OVER255;
            break;
        }
    }

    if ( Glide.AOther )
    {
        switch ( Glide.State.AlphaOther )
        {
        case GR_COMBINE_OTHER_ITERATED:
            Other.aa = a->a * D1OVER255;
            Other.ba = b->a * D1OVER255;
            Other.ca = c->a * D1OVER255;
            break;

        case GR_COMBINE_OTHER_CONSTANT:
            Other.aa = Other.ba = Other.ca = OpenGL.ConstantColor[ 3 ];
            break;

        case GR_COMBINE_OTHER_TEXTURE:
            if ( OpenGL.Texture )
            {
                Other.aa = Other.ba = Other.ca = 1.0f;
            }
            else
            {
                Other.aa = Other.ba = Other.ca = 0.0f;
            }
            break;
        }
    }

    if ( Glide.CLocal )
    {
        switch ( Glide.State.ColorCombineLocal )
        {
        case GR_COMBINE_LOCAL_ITERATED:
            Local.ar = a->r * D1OVER255;
            Local.ag = a->g * D1OVER255;
            Local.ab = a->b * D1OVER255;
            Local.br = b->r * D1OVER255;
            Local.bg = b->g * D1OVER255;
            Local.bb = b->b * D1OVER255;
            Local.cr = c->r * D1OVER255;
            Local.cg = c->g * D1OVER255;
            Local.cb = c->b * D1OVER255;
            break;

        case GR_COMBINE_LOCAL_CONSTANT:
            Local.ar = Local.br = Local.cr = OpenGL.ConstantColor[ 0 ];
            Local.ag = Local.bg = Local.cg = OpenGL.ConstantColor[ 1 ];
            Local.ab = Local.bb = Local.cb = OpenGL.ConstantColor[ 2 ];
            break;
        }
    }

    if ( Glide.COther )
    {
        switch ( Glide.State.ColorCombineOther )
        {
        case GR_COMBINE_OTHER_ITERATED:
            Other.ar = a->r * D1OVER255;
            Other.ag = a->g * D1OVER255;
            Other.ab = a->b * D1OVER255;
            Other.br = b->r * D1OVER255;
            Other.bg = b->g * D1OVER255;
            Other.bb = b->b * D1OVER255;
            Other.cr = c->r * D1OVER255;
            Other.cg = c->g * D1OVER255;
            Other.cb = c->b * D1OVER255;
            break;

        case GR_COMBINE_OTHER_CONSTANT:
            Other.ar = Other.br = Other.cr = OpenGL.ConstantColor[ 0 ];
            Other.ag = Other.bg = Other.cg = OpenGL.ConstantColor[ 1 ];
            Other.ab = Other.bb = Other.cb = OpenGL.ConstantColor[ 2 ];
            break;

        case GR_COMBINE_OTHER_TEXTURE:
            if ( OpenGL.Texture )
            {
                Other.ar = Other.ag = Other.ab = 1.0f;
                Other.br = Other.bg = Other.bb = 1.0f;
                Other.cr = Other.cg = Other.cb = 1.0f;
            }
            else
            {
                Other.ar = Other.ag = Other.ab = 0.0f;
                Other.br = Other.bg = Other.bb = 0.0f;
                Other.cr = Other.cg = Other.cb = 0.0f;
            }
            break;
        }
    }

    ColorFunctionFunc( pC, pC2, &Local, &Other );

    // Without the texture in the colour, primary plus secondary colour is the whole result. OpenGL clamps both at the vertices,
    // so a negative primary part, such as other below local in a blend, is lost there. Split the sum anew: the primary colour
    // takes it up to 1, the secondary colour the rest up to 2, and the sum stays exact.
    if ( !OpenGL.ColorTexture )
    {
        SplitColorSum( pC->ar, pC2->ar );
        SplitColorSum( pC->ag, pC2->ag );
        SplitColorSum( pC->ab, pC2->ab );
        SplitColorSum( pC->br, pC2->br );
        SplitColorSum( pC->bg, pC2->bg );
        SplitColorSum( pC->bb, pC2->bb );
        SplitColorSum( pC->cr, pC2->cr );
        SplitColorSum( pC->cg, pC2->cg );
        SplitColorSum( pC->cb, pC2->cb );
    }

    switch ( Glide.State.AlphaFunction )
    {
    case GR_COMBINE_FUNCTION_ZERO:
        pC->aa = pC->ba = pC->ca = 0.0f;
        break;

    case GR_COMBINE_FUNCTION_LOCAL:
    case GR_COMBINE_FUNCTION_LOCAL_ALPHA:
        pC->aa = Local.aa;
        pC->ba = Local.ba;
        pC->ca = Local.ca;
        break;

    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL:
    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        pC->aa = ( 1.0f - AlphaFactorFunc( Local.aa, Other.aa ) ) * Local.aa;
        pC->ba = ( 1.0f - AlphaFactorFunc( Local.ba, Other.ba ) ) * Local.ba;
        pC->ca = ( 1.0f - AlphaFactorFunc( Local.ca, Other.ca ) ) * Local.ca;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER:
        pC->aa = AlphaFactorFunc( Local.aa, Other.aa ) * Other.aa;
        pC->ba = AlphaFactorFunc( Local.ba, Other.ba ) * Other.ba;
        pC->ca = AlphaFactorFunc( Local.ca, Other.ca ) * Other.ca;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL:
    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA:
        pC->aa = AlphaFactorFunc( Local.aa, Other.aa ) * Other.aa + Local.aa;
        pC->ba = AlphaFactorFunc( Local.ba, Other.ba ) * Other.ba + Local.ba;
        pC->ca = AlphaFactorFunc( Local.ca, Other.ca ) * Other.ca + Local.ca;
//      pC2->aa =  Local.aa;
//      pC2->ba =  Local.ba;
//      pC2->ca =  Local.ca;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL:
        pC->aa = AlphaFactorFunc( Local.aa, Other.aa ) * ( Other.aa - Local.aa );
        pC->ba = AlphaFactorFunc( Local.ba, Other.ba ) * ( Other.ba - Local.ba );
        pC->ca = AlphaFactorFunc( Local.ca, Other.ca ) * ( Other.ca - Local.ca );
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL:
    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        pC->aa = AlphaFactorFunc( Local.aa, Other.aa ) * ( Other.aa - Local.aa ) + Local.aa;
        pC->ba = AlphaFactorFunc( Local.ba, Other.ba ) * ( Other.ba - Local.ba ) + Local.ba;
        pC->ca = AlphaFactorFunc( Local.ca, Other.ca ) * ( Other.ca - Local.ca ) + Local.ca;
//      pC2->aa =  Local.ba;
//      pC2->ba =  Local.ba;
//      pC2->ca =  Local.ca;
        break;
    }

    if ( Glide.State.ColorCombineInvert )
    {
        pC->ar = 1.0f - pC->ar - pC2->ar;
        pC->ag = 1.0f - pC->ag - pC2->ag;
        pC->ab = 1.0f - pC->ab - pC2->ab;
        pC->br = 1.0f - pC->br - pC2->br;
        pC->bg = 1.0f - pC->bg - pC2->bg;
        pC->bb = 1.0f - pC->bb - pC2->bb;
        pC->cr = 1.0f - pC->cr - pC2->cr;
        pC->cg = 1.0f - pC->cg - pC2->cg;
        pC->cb = 1.0f - pC->cb - pC2->cb;
        pC2->ar = pC2->ag = pC2->ab = 0.0f;
        pC2->br = pC2->bg = pC2->bb = 0.0f;
        pC2->cr = pC2->cg = pC2->cb = 0.0f;
    }

    if ( Glide.State.AlphaInvert )
    {
        pC->aa = 1.0f - pC->aa - pC2->aa;
        pC->ba = 1.0f - pC->ba - pC2->ba;
        pC->ca = 1.0f - pC->ca - pC2->ca;
        pC2->aa = pC2->ba = pC2->ca = 0.0f;
    }
    
    // Z-Buffering
    if ( ( Glide.State.DepthBufferMode == GR_DEPTHBUFFER_DISABLE ) || 
         ( Glide.State.DepthFunction == GR_CMP_ALWAYS ) )
    {
        pV->az = 0.0f;
        pV->bz = 0.0f;
        pV->cz = 0.0f;
    }
    else
    if ( OpenGL.DepthBufferType )
    {
        pV->az = a->ooz * D1OVER65535;
        pV->bz = b->ooz * D1OVER65535;
        pV->cz = c->ooz * D1OVER65535;
    }
    else
    {
       /*
        * For silly values of oow, depth buffering doesn't
        * seem to work, so map them to a sensible z.  When
        * games use these silly values, they probably don't
        * use z buffering anyway.
        */
        if ( a->oow > 1.0f )
        {
            pV->az = pV->bz = pV->cz = 1.0f;
        }
        else 
        if ( InternalConfig.PrecisionFix )
        {   // Fix precision to 16 integer bits.
            FxU16 w;
            w      = (FxU16)((a->oow / D1OVER65535) + 0.5f);
            pV->az = (float)w * D1OVER65535;
            w      = (FxU16)((b->oow / D1OVER65535) + 0.5f);
            pV->bz = (float)w * D1OVER65535;
            w      = (FxU16)((c->oow / D1OVER65535) + 0.5f);
            pV->cz = (float)w * D1OVER65535;
        }
        else
        {
            pV->az = a->oow;
            pV->bz = b->oow;
            pV->cz = c->oow;
        }
    }

    if ( ( unsnap ) &&
         ( a->x > vertex_snap_compare ) )
    {
        pV->ax = a->x - vertex_snap;
        pV->ay = a->y - vertex_snap;
        pV->bx = b->x - vertex_snap;
        pV->by = b->y - vertex_snap;
        pV->cx = c->x - vertex_snap;
        pV->cy = c->y - vertex_snap;
    }
    else
    {
        pV->ax = a->x;
        pV->ay = a->y;
        pV->bx = b->x;
        pV->by = b->y;
        pV->cx = c->x;
        pV->cy = c->y;
    }

    if ( OpenGL.Texture )
    {
        maxoow = 1.0f / max( atmuoow, max( btmuoow, ctmuoow ) );

        Textures[ GR_TMU0 ]->GetAspect( &hAspect, &wAspect );

        pTS->as = a->tmuvtx[ 0 ].sow * wAspect * maxoow;
        pTS->at = a->tmuvtx[ 0 ].tow * hAspect * maxoow;
        pTS->bs = b->tmuvtx[ 0 ].sow * wAspect * maxoow;
        pTS->bt = b->tmuvtx[ 0 ].tow * hAspect * maxoow;
        pTS->cs = c->tmuvtx[ 0 ].sow * wAspect * maxoow;
        pTS->ct = c->tmuvtx[ 0 ].tow * hAspect * maxoow;

        pTS->aq = pTS->bq = pTS->cq = 0.0f;
        pTS->aoow = atmuoow * maxoow;
        pTS->boow = btmuoow * maxoow;
        pTS->coow = ctmuoow * maxoow;

        TTextureStruct *pTS1 = &OGLRender.TTexture1[ OGLRender.NumberOfTriangles ];
        if ( TextureStageActive( ) )
        {
            Tmu1TriangleCoordinates( pTS1, a, b, c, atmuoow, btmuoow, ctmuoow );
        }
        else if ( InternalConfig.EXT_paletted_texture )
        {
            // AP88's alpha texture on GL unit 1 samples where TMU0 does.
            *pTS1 = *pTS;
        }
    }

    if( InternalConfig.FogEnable )
    {
        pF = &OGLRender.TFog[ OGLRender.NumberOfTriangles ];
        if ( Glide.State.FogMode == GR_FOG_WITH_TABLE )
//        if ( Glide.State.FogMode & GR_FOG_WITH_TABLE )
        {
            pF->af = OGLFogDistance( (a->oow)? (1.f / a->oow):65535.f );
            pF->bf = OGLFogDistance( (b->oow)? (1.f / b->oow):65535.f );
            pF->cf = OGLFogDistance( (c->oow)? (1.f / c->oow):65535.f );
        }
        else
        {
            pF->af = a->a * D1OVER255;
            pF->bf = b->a * D1OVER255;
            pF->cf = c->a * D1OVER255;
        }
//        if ( Glide.State.FogMode & GR_FOG_ADD2 )
//        {
//            pF->af = 1.0f - pF->af;
//            pF->bf = 1.0f - pF->bf;
//            pF->cf = 1.0f - pF->cf;
//        }

        // GR_FOG_MULT2 leaves the incoming colour out: fogging from black gives fog alpha times the fog colour.
        if ( OpenGL.Fog && OpenGL.FogWithoutColor )
        {
            pC->ar = pC->ag = pC->ab = 0.0f;
            pC->br = pC->bg = pC->bb = 0.0f;
            pC->cr = pC->cg = pC->cb = 0.0f;
            pC2->ar = pC2->ag = pC2->ab = 0.0f;
            pC2->br = pC2->bg = pC2->bb = 0.0f;
            pC2->cr = pC2->cg = pC2->cb = 0.0f;
        }
        
    #ifdef OGL_DEBUG
       DEBUG_MIN_MAX( pF->af, OGLRender.MaxF, OGLRender.MinF );
       DEBUG_MIN_MAX( pF->bf, OGLRender.MaxF, OGLRender.MinF );
       DEBUG_MIN_MAX( pF->bf, OGLRender.MaxF, OGLRender.MinF );
    #endif
    }

#ifdef OGL_DEBUG
    DEBUG_MIN_MAX( pC->ar, OGLRender.MaxR, OGLRender.MinR );
    DEBUG_MIN_MAX( pC->br, OGLRender.MaxR, OGLRender.MinR );
    DEBUG_MIN_MAX( pC->cr, OGLRender.MaxR, OGLRender.MinR );
    
    DEBUG_MIN_MAX( pC->ag, OGLRender.MaxG, OGLRender.MinG );
    DEBUG_MIN_MAX( pC->bg, OGLRender.MaxG, OGLRender.MinG );
    DEBUG_MIN_MAX( pC->cg, OGLRender.MaxG, OGLRender.MinG );

    DEBUG_MIN_MAX( pC->ab, OGLRender.MaxB, OGLRender.MinB );
    DEBUG_MIN_MAX( pC->bb, OGLRender.MaxB, OGLRender.MinB );
    DEBUG_MIN_MAX( pC->cb, OGLRender.MaxB, OGLRender.MinB );

    DEBUG_MIN_MAX( pC->aa, OGLRender.MaxA, OGLRender.MinA );
    DEBUG_MIN_MAX( pC->ba, OGLRender.MaxA, OGLRender.MinA );
    DEBUG_MIN_MAX( pC->ca, OGLRender.MaxA, OGLRender.MinA );

    DEBUG_MIN_MAX( pV->az, OGLRender.MaxZ, OGLRender.MinZ );
    DEBUG_MIN_MAX( pV->bz, OGLRender.MaxZ, OGLRender.MinZ );
    DEBUG_MIN_MAX( pV->cz, OGLRender.MaxZ, OGLRender.MinZ );

    DEBUG_MIN_MAX( pV->ax, OGLRender.MaxX, OGLRender.MinX );
    DEBUG_MIN_MAX( pV->bx, OGLRender.MaxX, OGLRender.MinX );
    DEBUG_MIN_MAX( pV->cx, OGLRender.MaxX, OGLRender.MinX );

    DEBUG_MIN_MAX( pV->ay, OGLRender.MaxY, OGLRender.MinY );
    DEBUG_MIN_MAX( pV->by, OGLRender.MaxY, OGLRender.MinY );
    DEBUG_MIN_MAX( pV->cy, OGLRender.MaxY, OGLRender.MinY );

    DEBUG_MIN_MAX( pTS->as, OGLRender.MaxS, OGLRender.MinS );
    DEBUG_MIN_MAX( pTS->bs, OGLRender.MaxS, OGLRender.MinS );
    DEBUG_MIN_MAX( pTS->cs, OGLRender.MaxS, OGLRender.MinS );

    DEBUG_MIN_MAX( pTS->at, OGLRender.MaxT, OGLRender.MinT );
    DEBUG_MIN_MAX( pTS->bt, OGLRender.MaxT, OGLRender.MinT );
    DEBUG_MIN_MAX( pTS->ct, OGLRender.MaxT, OGLRender.MinT );

    OGLRender.FrameTriangles++;
#endif
    
    OGLRender.NumberOfTriangles++;

    if ( OGLRender.NumberOfTriangles >= ( MAXTRIANGLES - 1 ) )
    {
        RenderDrawTriangles( );
    }

#ifdef OPENGL_DEBUG
    GLErro( "Render::AddTriangle" );
#endif
}

void RenderAddLine( const GrVertex *a, const GrVertex *b, bool unsnap )
{
    pC  = &OGLRender.TColor[ MAXTRIANGLES ];
    pC2 = &OGLRender.TColor2[ MAXTRIANGLES ];
    pV  = &OGLRender.TVertex[ MAXTRIANGLES ];
    pTS = &OGLRender.TTexture[ MAXTRIANGLES ];
    pF  = &OGLRender.TFog[ MAXTRIANGLES ];

    // Color Stuff, need to optimize it
    ZeroMemory( pC2, sizeof( TColorStruct ) );

    if ( ( Glide.State.STWHint & GR_STWHINT_W_DIFF_TMU0 ) == 0 )
    {
        atmuoow = a->oow;
        btmuoow = b->oow;
    }
    else
    {
        atmuoow = a->tmuvtx[ 0 ].oow;
        btmuoow = b->tmuvtx[ 0 ].oow;
    }

    if ( Glide.ALocal )
    {
        switch ( Glide.State.AlphaLocal )
        {
        case GR_COMBINE_LOCAL_ITERATED:
            Local.aa = a->a * D1OVER255;
            Local.ba = b->a * D1OVER255;
            break;

        case GR_COMBINE_LOCAL_CONSTANT:
            Local.aa = Local.ba = OpenGL.ConstantColor[3];
            break;

        case GR_COMBINE_LOCAL_DEPTH:
            Local.aa = a->z;
            Local.ba = b->z;
            break;
        }
    }

    if ( Glide.AOther )
    {
        switch ( Glide.State.AlphaOther )
        {
        case GR_COMBINE_OTHER_ITERATED:
            Other.aa = a->a * D1OVER255;
            Other.ba = b->a * D1OVER255;
            break;

        case GR_COMBINE_OTHER_CONSTANT:
            Other.aa = Other.ba = OpenGL.ConstantColor[3];
            break;

        case GR_COMBINE_OTHER_TEXTURE:
            Other.aa = Other.ba = 1.0f;
            break;
        }
    }

    if ( Glide.CLocal )
    {
        switch ( Glide.State.ColorCombineLocal )
        {
        case GR_COMBINE_LOCAL_ITERATED:
            Local.ar = a->r * D1OVER255;
            Local.ag = a->g * D1OVER255;
            Local.ab = a->b * D1OVER255;
            Local.br = b->r * D1OVER255;
            Local.bg = b->g * D1OVER255;
            Local.bb = b->b * D1OVER255;
            break;

        case GR_COMBINE_LOCAL_CONSTANT:
            Local.ar = Local.br = OpenGL.ConstantColor[0];
            Local.ag = Local.bg = OpenGL.ConstantColor[1];
            Local.ab = Local.bb = OpenGL.ConstantColor[2];
            break;
        }
    }

    if ( Glide.COther )
    {
        switch ( Glide.State.ColorCombineOther )
        {
        case GR_COMBINE_OTHER_ITERATED:
            Other.ar = a->r * D1OVER255;
            Other.ag = a->g * D1OVER255;
            Other.ab = a->b * D1OVER255;
            Other.br = b->r * D1OVER255;
            Other.bg = b->g * D1OVER255;
            Other.bb = b->b * D1OVER255;
            break;

        case GR_COMBINE_OTHER_CONSTANT:
            Other.ar = Other.br = OpenGL.ConstantColor[0];
            Other.ag = Other.bg = OpenGL.ConstantColor[1];
            Other.ab = Other.bb = OpenGL.ConstantColor[2];
            break;

        case GR_COMBINE_OTHER_TEXTURE:
            Other.ar = Other.ag = Other.ab = 1.0f;
            Other.br = Other.bg = Other.bb = 1.0f;
            break;
        }
    }

    switch ( Glide.State.ColorCombineFunction )
    {
    case GR_COMBINE_FUNCTION_ZERO:
        pC->ar = pC->ag = pC->ab = 0.0f; 
        pC->br = pC->bg = pC->bb = 0.0f; 
        break;

    case GR_COMBINE_FUNCTION_LOCAL:
        pC->ar = Local.ar;
        pC->ag = Local.ag;
        pC->ab = Local.ab;
        pC->br = Local.br;
        pC->bg = Local.bg;
        pC->bb = Local.bb;
        break;

    case GR_COMBINE_FUNCTION_LOCAL_ALPHA:
        pC->ar = pC->ag = pC->ab = Local.aa;
        pC->br = pC->bg = pC->bb = Local.ba;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = CFactor.ar * Other.ar;
        pC->ag = CFactor.ag * Other.ag;
        pC->ab = CFactor.ab * Other.ab;
        pC->br = CFactor.br * Other.br;
        pC->bg = CFactor.bg * Other.bg;
        pC->bb = CFactor.bb * Other.bb;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = CFactor.ar * Other.ar;
        pC->ag = CFactor.ag * Other.ag;
        pC->ab = CFactor.ab * Other.ab;
        pC->br = CFactor.br * Other.br;
        pC->bg = CFactor.bg * Other.bg;
        pC->bb = CFactor.bb * Other.bb;
        pC2->ar = Local.ar;
        pC2->ag = Local.ag;
        pC2->ab = Local.ab;
        pC2->br = Local.br;
        pC2->bg = Local.bg;
        pC2->bb = Local.bb;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = CFactor.ar * Other.ar;
        pC->ag = CFactor.ag * Other.ag;
        pC->ab = CFactor.ab * Other.ab;
        pC->br = CFactor.br * Other.br;
        pC->bg = CFactor.bg * Other.bg;
        pC->bb = CFactor.bb * Other.bb;
        pC2->ar = pC2->ag = pC2->ab = Local.aa;
        pC2->br = pC2->bg = pC2->bb = Local.ba;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = CFactor.ar * (Other.ar - Local.ar);
        pC->ag = CFactor.ag * (Other.ag - Local.ag);
        pC->ab = CFactor.ab * (Other.ab - Local.ab);
        pC->br = CFactor.br * (Other.br - Local.br);
        pC->bg = CFactor.bg * (Other.bg - Local.bg);
        pC->bb = CFactor.bb * (Other.bb - Local.bb);
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL:
        if ((( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_ALPHA ) ||
            ( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_RGB )) &&
            (  Glide.State.ColorCombineOther == GR_COMBINE_OTHER_TEXTURE ) )
        {
            pC->ar = Local.ar;
            pC->ag = Local.ag;
            pC->ab = Local.ab;
            pC->br = Local.br;
            pC->bg = Local.bg;
            pC->bb = Local.bb;
        }
        else
        {
            ColorFactor3Func( &CFactor, &Local, &Other );
            pC->ar = CFactor.ar * (Other.ar - Local.ar);
            pC->ag = CFactor.ag * (Other.ag - Local.ag);
            pC->ab = CFactor.ab * (Other.ab - Local.ab);
            pC->br = CFactor.br * (Other.br - Local.br);
            pC->bg = CFactor.bg * (Other.bg - Local.bg);
            pC->bb = CFactor.bb * (Other.bb - Local.bb);
            pC2->ar = Local.ar;
            pC2->ag = Local.ag;
            pC2->ab = Local.ab;
            pC2->br = Local.br;
            pC2->bg = Local.bg;
            pC2->bb = Local.bb;
        }
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        if ((( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_ALPHA ) ||
            ( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_RGB )) &&
            (  Glide.State.ColorCombineOther == GR_COMBINE_OTHER_TEXTURE ) )
        {
            pC->ar = pC->ag = pC->ab = Local.aa;
            pC->br = pC->bg = pC->bb = Local.ba;
        }
        else
        {
            ColorFactor3Func( &CFactor, &Local, &Other );
            pC->ar = CFactor.ar * (Other.ar - Local.ar);
            pC->ag = CFactor.ag * (Other.ag - Local.ag);
            pC->ab = CFactor.ab * (Other.ab - Local.ab);
            pC->br = CFactor.br * (Other.br - Local.br);
            pC->bg = CFactor.bg * (Other.bg - Local.bg);
            pC->bb = CFactor.bb * (Other.bb - Local.bb);
            pC2->ar = pC2->ag = pC2->ab = Local.aa;
            pC2->br = pC2->bg = pC2->bb = Local.ba;
        }
        break;

    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = ( 1.0f - CFactor.ar ) * Local.ar;
        pC->ag = ( 1.0f - CFactor.ag ) * Local.ag;
        pC->ab = ( 1.0f - CFactor.ab ) * Local.ab;
        pC->br = ( 1.0f - CFactor.br ) * Local.br;
        pC->bg = ( 1.0f - CFactor.bg ) * Local.bg;
        pC->bb = ( 1.0f - CFactor.bb ) * Local.bb;
        pC2->ar = Local.ar;
        pC2->ag = Local.ag;
        pC2->ab = Local.ab;
        pC2->br = Local.br;
        pC2->bg = Local.bg;
        pC2->bb = Local.bb;
        break;

    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = CFactor.ar * -Local.ar;
        pC->ag = CFactor.ag * -Local.ag;
        pC->ab = CFactor.ab * -Local.ab;
        pC->br = CFactor.br * -Local.br;
        pC->bg = CFactor.bg * -Local.bg;
        pC->bb = CFactor.bb * -Local.bb;
        pC2->ar = pC2->ag = pC2->ab = Local.aa;
        pC2->br = pC2->bg = pC2->bb = Local.ba;
        break;
    }

    switch ( Glide.State.AlphaFunction )
    {
    case GR_COMBINE_FUNCTION_ZERO:
        pC->aa = pC->ba = 0.0f;
        break;

    case GR_COMBINE_FUNCTION_LOCAL:
    case GR_COMBINE_FUNCTION_LOCAL_ALPHA:
        pC->aa = Local.aa;
        pC->ba = Local.ba;
        break;

    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL:
    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        pC->aa = ((1.0f - AlphaFactorFunc( Local.aa, Other.aa )) * Local.aa);
        pC->ba = ((1.0f - AlphaFactorFunc( Local.ba, Other.ba )) * Local.ba);
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER:
        pC->aa = (AlphaFactorFunc( Local.aa, Other.aa ) * Other.aa);
        pC->ba = (AlphaFactorFunc( Local.ba, Other.ba ) * Other.ba);
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL:
    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA:
        pC->aa = (AlphaFactorFunc( Local.aa, Other.aa ) * Other.aa + Local.aa);
        pC->ba = (AlphaFactorFunc( Local.ba, Other.ba ) * Other.ba + Local.ba);
//      pC2->aa =  Local.aa;
//      pC2->ba =  Local.ba;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL:
        pC->aa = (AlphaFactorFunc( Local.aa, Other.aa ) * ( Other.aa - Local.aa ));
        pC->ba = (AlphaFactorFunc( Local.ba, Other.ba ) * ( Other.ba - Local.ba ));
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL:
    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        pC->aa = (AlphaFactorFunc( Local.aa, Other.aa ) * ( Other.aa - Local.aa ) + Local.aa);
        pC->ba = (AlphaFactorFunc( Local.ba, Other.ba ) * ( Other.ba - Local.ba ) + Local.ba);
//      pC2->aa =  Local.aa;
//      pC2->ba =  Local.ba;
        break;
    }

    if ( Glide.State.ColorCombineInvert )
    {
        pC->ar = 1.0f - pC->ar - pC2->ar;
        pC->ag = 1.0f - pC->ag - pC2->ag;
        pC->ab = 1.0f - pC->ab - pC2->ab;
        pC->br = 1.0f - pC->br - pC2->br;
        pC->bg = 1.0f - pC->bg - pC2->bg;
        pC->bb = 1.0f - pC->bb - pC2->bb;
        pC2->ar = pC2->ag = pC2->ab = 0.0f;
        pC2->br = pC2->bg = pC2->bb = 0.0f;
    }

    if ( Glide.State.AlphaInvert )
    {
        pC->aa = 1.0f - pC->aa - pC2->aa;
        pC->ba = 1.0f - pC->ba - pC2->ba;
        pC2->aa = pC2->ba = 0.0f;
    }
    
    // Z-Buffering
    if ( ( Glide.State.DepthBufferMode == GR_DEPTHBUFFER_DISABLE ) || 
         ( Glide.State.DepthBufferMode == GR_CMP_ALWAYS ) )
    {
        pV->az = 0.0f;
        pV->bz = 0.0f;
    }
    else 
    if ( OpenGL.DepthBufferType )
    {
        pV->az = a->ooz * D1OVER65535;
        pV->bz = b->ooz * D1OVER65535;
    }
    else
    {
       /*
        * For silly values of oow, depth buffering doesn't
        * seem to work, so map them to a sensible z.  When
        * games use these silly values, they probably don't
        * use z buffering anyway.
        */
        if ( a->oow > 1.0f )
        {
            pV->az = pV->bz = 1.0f;
        }
        else 
        if ( InternalConfig.PrecisionFix )
        {   // Fix precision to 16 integer bits.
            FxU16 w;
            w      = (FxU16)((a->oow / D1OVER65535) + 0.5f);
            pV->az = (float)w * D1OVER65535;
            w      = (FxU16)((b->oow / D1OVER65535) + 0.5f);
            pV->bz = (float)w / D1OVER65535;
        }
        else
        {
            pV->az = a->oow;
            pV->bz = b->oow;
        }
    }

    if ( ( unsnap ) &&
         ( a->x > vertex_snap_compare ) )
    {
        pV->ax = a->x - vertex_snap;
        pV->ay = a->y - vertex_snap;
        pV->bx = b->x - vertex_snap;
        pV->by = b->y - vertex_snap;
    }
    else
    {
        pV->ax = a->x;
        pV->ay = a->y;
        pV->bx = b->x;
        pV->by = b->y;
    }

    bool texture_stage = TextureStageActive( );
    TTextureStruct *pTS1 = &OGLRender.TTexture1[ MAXTRIANGLES ];

    if ( OpenGL.Texture )
    {
        Textures[ GR_TMU0 ]->GetAspect( &hAspect, &wAspect );

        pTS->as = a->tmuvtx[0].sow * wAspect; // / a->oow;
        pTS->at = a->tmuvtx[0].tow * hAspect; // / a->oow;
        pTS->bs = b->tmuvtx[0].sow * wAspect; // / b->oow;
        pTS->bt = b->tmuvtx[0].tow * hAspect; // / b->oow;

        pTS->aq = pTS->bq = 0.0f;
        pTS->aoow = atmuoow;
        pTS->boow = btmuoow;
    }

    if ( texture_stage )
    {
        float tmu1HAspect;
        float tmu1WAspect;
        Textures[ GR_TMU1 ]->GetAspect( &tmu1HAspect, &tmu1WAspect );
        Tmu1Coordinate( a, atmuoow, tmu1HAspect, tmu1WAspect, &pTS1->as );
        Tmu1Coordinate( b, btmuoow, tmu1HAspect, tmu1WAspect, &pTS1->bs );
    }

    if ( InternalConfig.FogEnable )
    {
        pF->af = OGLFogDistance( (a->oow)? (1.f / a->oow):65535.f );
        pF->bf = OGLFogDistance( (b->oow)? (1.f / b->oow):65535.f );

    #ifdef OGL_DEBUG
        DEBUG_MIN_MAX( pF->af, OGLRender.MaxF, OGLRender.MinF );
        DEBUG_MIN_MAX( pF->bf, OGLRender.MaxF, OGLRender.MinF );
    #endif
    }

#ifdef OGL_DEBUG
    DEBUG_MIN_MAX( pC->ar, OGLRender.MaxR, OGLRender.MinR );
    DEBUG_MIN_MAX( pC->br, OGLRender.MaxR, OGLRender.MinR );
    
    DEBUG_MIN_MAX( pC->ag, OGLRender.MaxG, OGLRender.MinG );
    DEBUG_MIN_MAX( pC->bg, OGLRender.MaxG, OGLRender.MinG );

    DEBUG_MIN_MAX( pC->ab, OGLRender.MaxB, OGLRender.MinB );
    DEBUG_MIN_MAX( pC->bb, OGLRender.MaxB, OGLRender.MinB );

    DEBUG_MIN_MAX( pC->aa, OGLRender.MaxA, OGLRender.MinA );
    DEBUG_MIN_MAX( pC->ba, OGLRender.MaxA, OGLRender.MinA );

    DEBUG_MIN_MAX( pV->az, OGLRender.MaxZ, OGLRender.MinZ );
    DEBUG_MIN_MAX( pV->bz, OGLRender.MaxZ, OGLRender.MinZ );

    DEBUG_MIN_MAX( pV->ax, OGLRender.MaxX, OGLRender.MinX );
    DEBUG_MIN_MAX( pV->bx, OGLRender.MaxX, OGLRender.MinX );

    DEBUG_MIN_MAX( pV->ay, OGLRender.MaxY, OGLRender.MinY );
    DEBUG_MIN_MAX( pV->by, OGLRender.MaxY, OGLRender.MinY );

    DEBUG_MIN_MAX( pTS->as, OGLRender.MaxS, OGLRender.MinS );
    DEBUG_MIN_MAX( pTS->bs, OGLRender.MaxS, OGLRender.MinS );

    DEBUG_MIN_MAX( pTS->at, OGLRender.MaxT, OGLRender.MinT );
    DEBUG_MIN_MAX( pTS->bt, OGLRender.MaxT, OGLRender.MinT );
#endif

    if ( OpenGL.Texture )
    {
        glEnable( GL_TEXTURE_2D );

        Textures[ GR_TMU0 ]->MakeReady( );
    }
    else
    {
        glDisable( GL_TEXTURE_2D );
    }

    if ( texture_stage )
    {
        BeginTextureStage( );
    }

    if ( OpenGL.Blend )
    {
        glEnable( GL_BLEND );
    }
    else
    {
        glDisable( GL_BLEND );
    }

    // A Voodoo tests the combined alpha whatever its source; the test used to be dropped unless the alpha came from the texture.
    if ( Glide.State.AlphaTestFunction != GR_CMP_ALWAYS )
    {
        glEnable( GL_ALPHA_TEST );
    }
    else
    {
        glDisable( GL_ALPHA_TEST );
    }
    
    glBegin( GL_LINES );
        glColor4fv( &pC->ar );
        p_glSecondaryColor3fvEXT( &pC2->ar );
        glTexCoord4fv( &pTS->as );
        if ( texture_stage )
        {
            p_glMultiTexCoord4fvARB( GL_TEXTURE1_ARB, &pTS1->as );
        }
        p_glFogCoordfEXT( pF->af );
        glVertex3fv( &pV->ax );

        glColor4fv( &pC->br );
        p_glSecondaryColor3fvEXT( &pC2->br );
        glTexCoord4fv( &pTS->bs );
        if ( texture_stage )
        {
            p_glMultiTexCoord4fvARB( GL_TEXTURE1_ARB, &pTS1->bs );
        }
        p_glFogCoordfEXT( pF->bf );
        glVertex3fv( &pV->bx );
    glEnd();

    if ( texture_stage )
    {
        EndTextureStage( );
    }

#ifdef OPENGL_DEBUG
    GLErro( "Render::AddLine" );
#endif
}

void RenderAddPoint( const GrVertex *a, bool unsnap )
{
    pC  = &OGLRender.TColor[ MAXTRIANGLES ];
    pC2 = &OGLRender.TColor2[ MAXTRIANGLES ];
    pV  = &OGLRender.TVertex[ MAXTRIANGLES ];
    pTS = &OGLRender.TTexture[ MAXTRIANGLES ];
    pF  = &OGLRender.TFog[ MAXTRIANGLES ];

    // Color Stuff, need to optimize it
    ZeroMemory( pC2, sizeof( TColorStruct ) );

    if ( Glide.ALocal )
    {
        switch ( Glide.State.AlphaLocal )
        {
        case GR_COMBINE_LOCAL_ITERATED:
            Local.aa = a->a * D1OVER255;
            break;

        case GR_COMBINE_LOCAL_CONSTANT:
            Local.aa = OpenGL.ConstantColor[3];
            break;

        case GR_COMBINE_LOCAL_DEPTH:
            Local.aa = a->z;
            break;
        }
    }

    if ( Glide.AOther )
    {
        switch ( Glide.State.AlphaOther )
        {
        case GR_COMBINE_OTHER_ITERATED:
            Other.aa = a->a * D1OVER255;
            break;

        case GR_COMBINE_OTHER_CONSTANT:
            Other.aa = OpenGL.ConstantColor[3];
            break;

        case GR_COMBINE_OTHER_TEXTURE:
            Other.aa = 1.0f;
            break;
        }
    }

    if ( Glide.CLocal )
    {
        switch ( Glide.State.ColorCombineLocal )
        {
        case GR_COMBINE_LOCAL_ITERATED:
            Local.ar = a->r * D1OVER255;
            Local.ag = a->g * D1OVER255;
            Local.ab = a->b * D1OVER255;
            break;

        case GR_COMBINE_LOCAL_CONSTANT:
            Local.ar = OpenGL.ConstantColor[0];
            Local.ag = OpenGL.ConstantColor[1];
            Local.ab = OpenGL.ConstantColor[2];
            break;
        }
    }

    if ( Glide.COther )
    {
        switch ( Glide.State.ColorCombineOther )
        {
        case GR_COMBINE_OTHER_ITERATED:
            Other.ar = a->r * D1OVER255;
            Other.ag = a->g * D1OVER255;
            Other.ab = a->b * D1OVER255;
            break;

        case GR_COMBINE_OTHER_CONSTANT:
            Other.ar = OpenGL.ConstantColor[0];
            Other.ag = OpenGL.ConstantColor[1];
            Other.ab = OpenGL.ConstantColor[2];
            break;

        case GR_COMBINE_OTHER_TEXTURE:
            Other.ar = Other.ag = Other.ab = 1.0f;
            break;
        }
    }

    switch ( Glide.State.ColorCombineFunction )
    {
    case GR_COMBINE_FUNCTION_ZERO:
        pC->ar = pC->ag = pC->ab = 0.0f; 
        break;

    case GR_COMBINE_FUNCTION_LOCAL:
        pC->ar = Local.ar;
        pC->ag = Local.ag;
        pC->ab = Local.ab;
        break;

    case GR_COMBINE_FUNCTION_LOCAL_ALPHA:
        pC->ar = pC->ag = pC->ab = Local.aa;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = CFactor.ar * Other.ar;
        pC->ag = CFactor.ag * Other.ag;
        pC->ab = CFactor.ab * Other.ab;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = CFactor.ar * Other.ar;
        pC->ag = CFactor.ag * Other.ag;
        pC->ab = CFactor.ab * Other.ab;
        pC2->ar = Local.ar;
        pC2->ag = Local.ag;
        pC2->ab = Local.ab;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = CFactor.ar * Other.ar;
        pC->ag = CFactor.ag * Other.ag;
        pC->ab = CFactor.ab * Other.ab;
        pC2->ar = pC2->ag = pC2->ab = Local.aa;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = CFactor.ar * (Other.ar - Local.ar);
        pC->ag = CFactor.ag * (Other.ag - Local.ag);
        pC->ab = CFactor.ab * (Other.ab - Local.ab);
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL:
        if ((( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_ALPHA ) ||
            ( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_RGB )) &&
            (  Glide.State.ColorCombineOther == GR_COMBINE_OTHER_TEXTURE ) )
        {
            pC->ar = Local.ar;
            pC->ag = Local.ag;
            pC->ab = Local.ab;
        }
        else
        {
            ColorFactor3Func( &CFactor, &Local, &Other );
            pC->ar = CFactor.ar * (Other.ar - Local.ar);
            pC->ag = CFactor.ag * (Other.ag - Local.ag);
            pC->ab = CFactor.ab * (Other.ab - Local.ab);
            pC2->ar = Local.ar;
            pC2->ag = Local.ag;
            pC2->ab = Local.ab;
        }
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        if ((( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_ALPHA ) ||
            ( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_RGB )) &&
            (  Glide.State.ColorCombineOther == GR_COMBINE_OTHER_TEXTURE ) )
        {
            pC->ar = pC->ag = pC->ab = Local.aa;
        }
        else
        {
            ColorFactor3Func( &CFactor, &Local, &Other );
            pC->ar = CFactor.ar * (Other.ar - Local.ar);
            pC->ag = CFactor.ag * (Other.ag - Local.ag);
            pC->ab = CFactor.ab * (Other.ab - Local.ab);
            pC2->ar = pC2->ag = pC2->ab = Local.aa;
        }
        break;

    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = ( 1.0f - CFactor.ar ) * Local.ar;
        pC->ag = ( 1.0f - CFactor.ag ) * Local.ag;
        pC->ab = ( 1.0f - CFactor.ab ) * Local.ab;
        pC2->ar = Local.ar;
        pC2->ag = Local.ag;
        pC2->ab = Local.ab;
        break;

    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        ColorFactor3Func( &CFactor, &Local, &Other );
        pC->ar = CFactor.ar * -Local.ar;
        pC->ag = CFactor.ag * -Local.ag;
        pC->ab = CFactor.ab * -Local.ab;
        pC2->ar = pC2->ag = pC2->ab = Local.aa;
        break;
    }

    switch ( Glide.State.AlphaFunction )
    {
    case GR_COMBINE_FUNCTION_ZERO:
        pC->aa = 0.0f;
        break;

    case GR_COMBINE_FUNCTION_LOCAL:
    case GR_COMBINE_FUNCTION_LOCAL_ALPHA:
        pC->aa = Local.aa;
        break;

    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL:
    case GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        pC->aa = ((1.0f - AlphaFactorFunc( Local.aa, Other.aa )) * Local.aa);
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER:
        pC->aa = (AlphaFactorFunc( Local.aa, Other.aa ) * Other.aa);
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL:
    case GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA:
        pC->aa = AlphaFactorFunc( Local.aa, Other.aa ) * Other.aa + Local.aa;
//      pC2->aa =  Local.aa;
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL:
        pC->aa = (AlphaFactorFunc( Local.aa, Other.aa ) * ( Other.aa - Local.aa ));
        break;

    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL:
    case GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA:
        pC->aa = AlphaFactorFunc( Local.aa, Other.aa ) * ( Other.aa - Local.aa ) + Local.aa;
//      pC2->aa =  Local.aa;
        break;
    }

    if ( Glide.State.ColorCombineInvert )
    {
        pC->ar = 1.0f - pC->ar - pC2->ar;
        pC->ag = 1.0f - pC->ag - pC2->ag;
        pC->ab = 1.0f - pC->ab - pC2->ab;
        pC2->ar = pC2->ag = pC2->ab = 0.0f;
    }

    if ( Glide.State.AlphaInvert )
    {
        pC->aa = 1.0f - pC->aa - pC2->aa;
        pC2->aa = 0.0f;
    }
    
    // Z-Buffering
    if ( ( Glide.State.DepthBufferMode == GR_DEPTHBUFFER_DISABLE ) || 
         ( Glide.State.DepthBufferMode == GR_CMP_ALWAYS ) )
    {
        pV->az = 0.0f;
    }
    else 
    if ( OpenGL.DepthBufferType )
    {
        pV->az = a->ooz * D1OVER65535;
    }
    else
    {
        if ( a->oow > 1.0f )
        {
            pV->az = 1.0f;
        }
        else 
        if ( InternalConfig.PrecisionFix )
        {   // Fix precision to 16 integer bits.
            FxU16 w = (FxU16)((a->oow / D1OVER65535) + 0.5f);
            pV->az  = (float)w * D1OVER65535;
        }
        else
        {
            pV->az = a->oow;
        }
    }

    if ( ( unsnap ) &&
         ( a->x > vertex_snap_compare ) )
    {
        pV->ax = a->x - vertex_snap;
        pV->ay = a->y - vertex_snap;
    }
    else
    {
        pV->ax = a->x;
        pV->ay = a->y;
    }

    bool texture_stage = TextureStageActive( );
    TTextureStruct *pTS1 = &OGLRender.TTexture1[ MAXTRIANGLES ];

    if ( OpenGL.Texture )
    {
        Textures[ GR_TMU0 ]->GetAspect( &hAspect, &wAspect );

        pTS->as = a->tmuvtx[0].sow * wAspect;
        pTS->at = a->tmuvtx[0].tow * hAspect;

        pTS->aq = 0.0f;
        pTS->aoow = a->oow;
    }

    if ( texture_stage )
    {
        float tmu1HAspect;
        float tmu1WAspect;
        Textures[ GR_TMU1 ]->GetAspect( &tmu1HAspect, &tmu1WAspect );
        Tmu1Coordinate( a, a->oow, tmu1HAspect, tmu1WAspect, &pTS1->as );
    }

    if( InternalConfig.FogEnable )
    {
        pF->af = OGLFogDistance( (a->oow)? (1.f / a->oow):65535.f );

    #ifdef OGL_DEBUG
        DEBUG_MIN_MAX( pF->af, OGLRender.MaxF, OGLRender.MinF );
    #endif
    }

#ifdef OGL_DEBUG
    DEBUG_MIN_MAX( pC->ar, OGLRender.MaxR, OGLRender.MinR );
    
    DEBUG_MIN_MAX( pC->ag, OGLRender.MaxG, OGLRender.MinG );

    DEBUG_MIN_MAX( pC->ab, OGLRender.MaxB, OGLRender.MinB );

    DEBUG_MIN_MAX( pC->aa, OGLRender.MaxA, OGLRender.MinA );

    DEBUG_MIN_MAX( pV->az, OGLRender.MaxZ, OGLRender.MinZ );

    DEBUG_MIN_MAX( pV->ax, OGLRender.MaxX, OGLRender.MinX );

    DEBUG_MIN_MAX( pV->ay, OGLRender.MaxY, OGLRender.MinY );

    DEBUG_MIN_MAX( pTS->as, OGLRender.MaxS, OGLRender.MinS );

    DEBUG_MIN_MAX( pTS->at, OGLRender.MaxT, OGLRender.MinT );
#endif

    if ( OpenGL.Texture )
    {
        glEnable( GL_TEXTURE_2D );

        Textures[ GR_TMU0 ]->MakeReady( );
    }
    else
    {
        glDisable( GL_TEXTURE_2D );
    }

    if ( texture_stage )
    {
        BeginTextureStage( );
    }

    if ( OpenGL.Blend )
    {
        glEnable( GL_BLEND );
    }
    else
    {
        glDisable( GL_BLEND );
    }

    // A Voodoo tests the combined alpha whatever its source; the test used to be dropped unless the alpha came from the texture.
    if ( Glide.State.AlphaTestFunction != GR_CMP_ALWAYS )
    {
        glEnable( GL_ALPHA_TEST );
    }
    else
    {
        glDisable( GL_ALPHA_TEST );
    }
    
    glBegin( GL_POINTS );
        glColor4fv( &pC->ar );
        p_glSecondaryColor3fvEXT( &pC2->ar );
        glTexCoord4fv( &pTS->as );
        if ( texture_stage )
        {
            p_glMultiTexCoord4fvARB( GL_TEXTURE1_ARB, &pTS1->as );
        }
        p_glFogCoordfEXT( pF->af );
        glVertex3fv( &pV->ax );
    glEnd();

    if ( texture_stage )
    {
        EndTextureStage( );
    }

#ifdef OPENGL_DEBUG
    GLErro( "Render::AddPoint" );
#endif
}

// Color Factor functions

void FASTCALL ColorFactor3Zero( TColorStruct *Result, TColorStruct *ColorComponent, TColorStruct *OtherAlpha )
{
    Result->ar = Result->ag = Result->ab = 0.0f;
    Result->br = Result->bg = Result->bb = 0.0f;
    Result->cr = Result->cg = Result->cb = 0.0f;
}

void FASTCALL ColorFactor3Local( TColorStruct *Result, TColorStruct *ColorComponent, TColorStruct *OtherAlpha )
{
    Result->ar = ColorComponent->ar;
    Result->ag = ColorComponent->ag;
    Result->ab = ColorComponent->ab;
    Result->br = ColorComponent->br;
    Result->bg = ColorComponent->bg;
    Result->bb = ColorComponent->bb;
    Result->cr = ColorComponent->cr;
    Result->cg = ColorComponent->cg;
    Result->cb = ColorComponent->cb;
}

void FASTCALL ColorFactor3OtherAlpha( TColorStruct *Result, TColorStruct *ColorComponent, TColorStruct *OtherAlpha )
{
    Result->ar = Result->ag = Result->ab = OtherAlpha->aa;
    Result->br = Result->bg = Result->bb = OtherAlpha->ba;
    Result->cr = Result->cg = Result->cb = OtherAlpha->ca;
}

void FASTCALL ColorFactor3LocalAlpha( TColorStruct *Result, TColorStruct *ColorComponent, TColorStruct *OtherAlpha )
{
    Result->ar = Result->ag = Result->ab = ColorComponent->aa;
    Result->br = Result->bg = Result->bb = ColorComponent->ba;
    Result->cr = Result->cg = Result->cb = ColorComponent->ca;
}

void FASTCALL ColorFactor3OneMinusLocal( TColorStruct *Result, TColorStruct *ColorComponent, TColorStruct *OtherAlpha )
{
    Result->ar = 1.0f - ColorComponent->ar;
    Result->ag = 1.0f - ColorComponent->ag;
    Result->ab = 1.0f - ColorComponent->ab;
    Result->br = 1.0f - ColorComponent->br;
    Result->bg = 1.0f - ColorComponent->bg;
    Result->bb = 1.0f - ColorComponent->bb;
    Result->cr = 1.0f - ColorComponent->cr;
    Result->cg = 1.0f - ColorComponent->cg;
    Result->cb = 1.0f - ColorComponent->cb;
}

void FASTCALL ColorFactor3OneMinusOtherAlpha( TColorStruct *Result, TColorStruct *ColorComponent, TColorStruct *OtherAlpha )
{
    Result->ar = Result->ag = Result->ab = 1.0f - OtherAlpha->aa;
    Result->br = Result->bg = Result->bb = 1.0f - OtherAlpha->ba;
    Result->cr = Result->cg = Result->cb = 1.0f - OtherAlpha->ca;
}

void FASTCALL ColorFactor3OneMinusLocalAlpha( TColorStruct *Result, TColorStruct *ColorComponent, TColorStruct *OtherAlpha )
{
    Result->ar = Result->ag = Result->ab = 1.0f - ColorComponent->aa;
    Result->br = Result->bg = Result->bb = 1.0f - ColorComponent->ba;
    Result->cr = Result->cg = Result->cb = 1.0f - ColorComponent->ca;
}

void FASTCALL ColorFactor3One( TColorStruct *Result, TColorStruct *ColorComponent, TColorStruct *OtherAlpha )
{
    Result->ar = Result->ag = Result->ab = 1.0f;
    Result->br = Result->bg = Result->bb = 1.0f;
    Result->cr = Result->cg = Result->cb = 1.0f;
}

// Alpha Factor functions

float AlphaFactorZero( float LocalAlpha, float OtherAlpha )
{
    return 0.0f;
}

float AlphaFactorLocal( float LocalAlpha, float OtherAlpha )
{
    return LocalAlpha;
}

float AlphaFactorOther( float LocalAlpha, float OtherAlpha )
{
    return OtherAlpha;
}

float AlphaFactorOneMinusLocal( float LocalAlpha, float OtherAlpha )
{
    return 1.0f - LocalAlpha;
}

float AlphaFactorOneMinusOther( float LocalAlpha, float OtherAlpha )
{
    return 1.0f - OtherAlpha;
}

float AlphaFactorOne( float LocalAlpha, float OtherAlpha )
{
    return 1.0f;
}

// Color functions

void ColorFunctionZero( TColorStruct * pC, TColorStruct * pC2, TColorStruct * Local, TColorStruct * Other )
{
    pC->ar = pC->ag = pC->ab = 0.0f; 
    pC->br = pC->bg = pC->bb = 0.0f; 
    pC->cr = pC->cg = pC->cb = 0.0f; 
}

void ColorFunctionLocal( TColorStruct * pC, TColorStruct * pC2, TColorStruct * Local, TColorStruct * Other )
{
    pC->ar = Local->ar;
    pC->ag = Local->ag;
    pC->ab = Local->ab;
    pC->br = Local->br;
    pC->bg = Local->bg;
    pC->bb = Local->bb;
    pC->cr = Local->cr;
    pC->cg = Local->cg;
    pC->cb = Local->cb;
}

void ColorFunctionLocalAlpha( TColorStruct * pC, TColorStruct * pC2, TColorStruct * Local, TColorStruct * Other )
{
    pC->ar = pC->ag = pC->ab = Local->aa;
    pC->br = pC->bg = pC->bb = Local->ba;
    pC->cr = pC->cg = pC->cb = Local->ca;
}

void ColorFunctionScaleOther( TColorStruct * pC, TColorStruct * pC2, TColorStruct * Local, TColorStruct * Other )
{
    ColorFactor3Func( &CFactor, Local, Other );
    pC->ar = CFactor.ar * Other->ar;
    pC->ag = CFactor.ag * Other->ag;
    pC->ab = CFactor.ab * Other->ab;
    pC->br = CFactor.br * Other->br;
    pC->bg = CFactor.bg * Other->bg;
    pC->bb = CFactor.bb * Other->bb;
    pC->cr = CFactor.cr * Other->cr;
    pC->cg = CFactor.cg * Other->cg;
    pC->cb = CFactor.cb * Other->cb;
}

void ColorFunctionScaleOtherAddLocal( TColorStruct * pC, TColorStruct * pC2, TColorStruct * Local, TColorStruct * Other )
{
    ColorFactor3Func( &CFactor, Local, Other );
    pC->ar = CFactor.ar * Other->ar;
    pC->ag = CFactor.ag * Other->ag;
    pC->ab = CFactor.ab * Other->ab;
    pC->br = CFactor.br * Other->br;
    pC->bg = CFactor.bg * Other->bg;
    pC->bb = CFactor.bb * Other->bb;
    pC->cr = CFactor.cr * Other->cr;
    pC->cg = CFactor.cg * Other->cg;
    pC->cb = CFactor.cb * Other->cb;
    pC2->ar = Local->ar;
    pC2->ag = Local->ag;
    pC2->ab = Local->ab;
    pC2->br = Local->br;
    pC2->bg = Local->bg;
    pC2->bb = Local->bb;
    pC2->cr = Local->cr;
    pC2->cg = Local->cg;
    pC2->cb = Local->cb;
}

void ColorFunctionScaleOtherAddLocalAlpha( TColorStruct * pC, TColorStruct * pC2, TColorStruct * Local, TColorStruct * Other )
{
    ColorFactor3Func( &CFactor, Local, Other );
    pC->ar = CFactor.ar * Other->ar;
    pC->ag = CFactor.ag * Other->ag;
    pC->ab = CFactor.ab * Other->ab;
    pC->br = CFactor.br * Other->br;
    pC->bg = CFactor.bg * Other->bg;
    pC->bb = CFactor.bb * Other->bb;
    pC->cr = CFactor.cr * Other->cr;
    pC->cg = CFactor.cg * Other->cg;
    pC->cb = CFactor.cb * Other->cb;
    pC2->ar = pC2->ag = pC2->ab = Local->aa;
    pC2->br = pC2->bg = pC2->bb = Local->ba;
    pC2->cr = pC2->cg = pC2->cb = Local->ca;
}

void ColorFunctionScaleOtherMinusLocal( TColorStruct * pC, TColorStruct * pC2, TColorStruct * Local, TColorStruct * Other )
{
    ColorFactor3Func( &CFactor, Local, Other );
    pC->ar = CFactor.ar * (Other->ar - Local->ar);
    pC->ag = CFactor.ag * (Other->ag - Local->ag);
    pC->ab = CFactor.ab * (Other->ab - Local->ab);
    pC->br = CFactor.br * (Other->br - Local->br);
    pC->bg = CFactor.bg * (Other->bg - Local->bg);
    pC->bb = CFactor.bb * (Other->bb - Local->bb);
    pC->cr = CFactor.cr * (Other->cr - Local->cr);
    pC->cg = CFactor.cg * (Other->cg - Local->cg);
    pC->cb = CFactor.cb * (Other->cb - Local->cb);
}

void ColorFunctionScaleOtherMinusLocalAddLocal( TColorStruct * pC, TColorStruct * pC2, TColorStruct * Local, TColorStruct * Other )
{
    if ((( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_ALPHA ) ||
        ( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_RGB )) &&
        (  Glide.State.ColorCombineOther == GR_COMBINE_OTHER_TEXTURE ) )
    {
        pC->ar = Local->ar;
        pC->ag = Local->ag;
        pC->ab = Local->ab;
        pC->br = Local->br;
        pC->bg = Local->bg;
        pC->bb = Local->bb;
        pC->cr = Local->cr;
        pC->cg = Local->cg;
        pC->cb = Local->cb;
    }
    else
    {
        ColorFactor3Func( &CFactor, Local, Other );
        pC->ar = CFactor.ar * (Other->ar - Local->ar);
        pC->ag = CFactor.ag * (Other->ag - Local->ag);
        pC->ab = CFactor.ab * (Other->ab - Local->ab);
        pC->br = CFactor.br * (Other->br - Local->br);
        pC->bg = CFactor.bg * (Other->bg - Local->bg);
        pC->bb = CFactor.bb * (Other->bb - Local->bb);
        pC->cr = CFactor.cr * (Other->cr - Local->cr);
        pC->cg = CFactor.cg * (Other->cg - Local->cg);
        pC->cb = CFactor.cb * (Other->cb - Local->cb);
        pC2->ar = Local->ar;
        pC2->ag = Local->ag;
        pC2->ab = Local->ab;
        pC2->br = Local->br;
        pC2->bg = Local->bg;
        pC2->bb = Local->bb;
        pC2->cr = Local->cr;
        pC2->cg = Local->cg;
        pC2->cb = Local->cb;
    }
}

void ColorFunctionScaleOtherMinusLocalAddLocalAlpha( TColorStruct * pC, TColorStruct * pC2, TColorStruct * Local, TColorStruct * Other )
{
    if ((( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_ALPHA ) ||
        ( Glide.State.ColorCombineFactor == GR_COMBINE_FACTOR_TEXTURE_RGB )) &&
        (  Glide.State.ColorCombineOther == GR_COMBINE_OTHER_TEXTURE ) )
    {
        pC->ar = pC->ag = pC->ab = Local->aa;
        pC->br = pC->bg = pC->bb = Local->ba;
        pC->cr = pC->cg = pC->cb = Local->ca;
    }
    else
    {
        ColorFactor3Func( &CFactor, Local, Other );
        pC->ar = CFactor.ar * (Other->ar - Local->ar);
        pC->ag = CFactor.ag * (Other->ag - Local->ag);
        pC->ab = CFactor.ab * (Other->ab - Local->ab);
        pC->br = CFactor.br * (Other->br - Local->br);
        pC->bg = CFactor.bg * (Other->bg - Local->bg);
        pC->bb = CFactor.bb * (Other->bb - Local->bb);
        pC->cr = CFactor.cr * (Other->cr - Local->cr);
        pC->cg = CFactor.cg * (Other->cg - Local->cg);
        pC->cb = CFactor.cb * (Other->cb - Local->cb);
        pC2->ar = pC2->ag = pC2->ab = Local->aa;
        pC2->br = pC2->bg = pC2->bb = Local->ba;
        pC2->cr = pC2->cg = pC2->cb = Local->ca;
    }
}

void ColorFunctionMinusLocalAddLocal( TColorStruct * pC, TColorStruct * pC2, TColorStruct * Local, TColorStruct * Other )
{
    ColorFactor3Func( &CFactor, Local, Other );
    pC->ar = ( 1.0f - CFactor.ar ) * Local->ar;
    pC->ag = ( 1.0f - CFactor.ag ) * Local->ag;
    pC->ab = ( 1.0f - CFactor.ab ) * Local->ab;
    pC->br = ( 1.0f - CFactor.br ) * Local->br;
    pC->bg = ( 1.0f - CFactor.bg ) * Local->bg;
    pC->bb = ( 1.0f - CFactor.bb ) * Local->bb;
    pC->cr = ( 1.0f - CFactor.cr ) * Local->cr;
    pC->cg = ( 1.0f - CFactor.cg ) * Local->cg;
    pC->cb = ( 1.0f - CFactor.cb ) * Local->cb;
    // ( 1 - factor ) * local is already the whole result; local must not be added a second time.
    pC2->ar = pC2->ag = pC2->ab = 0.0f;
    pC2->br = pC2->bg = pC2->bb = 0.0f;
    pC2->cr = pC2->cg = pC2->cb = 0.0f;
}

void ColorFunctionMinusLocalAddLocalAlpha( TColorStruct * pC, TColorStruct * pC2, TColorStruct * Local, TColorStruct * Other )
{
    ColorFactor3Func( &CFactor, Local, Other );
    pC->ar = CFactor.ar * -Local->ar;
    pC->ag = CFactor.ag * -Local->ag;
    pC->ab = CFactor.ab * -Local->ab;
    pC->br = CFactor.br * -Local->br;
    pC->bg = CFactor.bg * -Local->bg;
    pC->bb = CFactor.bb * -Local->bb;
    pC->cr = CFactor.cr * -Local->cr;
    pC->cg = CFactor.cg * -Local->cg;
    pC->cb = CFactor.cb * -Local->cb;
    pC2->ar = pC2->ag = pC2->ab = Local->aa;
    pC2->br = pC2->bg = pC2->bb = Local->ba;
    pC2->cr = pC2->cg = pC2->cb = Local->ca;
}
