#pragma once

// Direct3D 7 to 9 vocabulary for the legacy core, in Direct3D 9's numbering
// (Direct3D 7 and 8 share most of it). The core does not include the old SDK
// headers, so that each front end can include its own (d3d.h and d3d9.h
// declare the same names with different values).

#include <cstdint>

namespace dxup::legacy {

  // D3DFORMAT
  enum Format : uint32_t {
    FormatUnknown   = 0,
    FormatR8G8B8    = 20,
    FormatA8R8G8B8  = 21,
    FormatX8R8G8B8  = 22,
    FormatR5G6B5    = 23,
    FormatX1R5G5B5  = 24,
    FormatA1R5G5B5  = 25,
    FormatA4R4G4B4  = 26,
    FormatR3G3B2    = 27,
    FormatA8        = 28,
    FormatX4R4G4B4  = 30,
    FormatA8B8G8R8  = 32,
    FormatX8B8G8R8  = 33,
    FormatP8        = 41,
    FormatL8        = 50,
    FormatA8L8      = 51,
    FormatA4L4      = 52,
    FormatV8U8      = 60,
    FormatD16Lockable = 70,
    FormatD32       = 71,
    FormatD15S1     = 73,
    FormatD24S8     = 75,
    FormatD24X8     = 77,
    FormatD24X4S4   = 79,
    FormatD16       = 80,
    FormatIndex16   = 101,
    FormatIndex32   = 102,
    FormatDXT1      = 0x31545844,
    FormatDXT2      = 0x32545844,
    FormatDXT3      = 0x33545844,
    FormatDXT4      = 0x34545844,
    FormatDXT5      = 0x35545844,
    // Direct3D 9
    FormatA2B10G10R10 = 31,
    FormatG16R16    = 34,
    FormatA2R10G10B10 = 35,
    FormatA16B16G16R16 = 36,
    FormatL16       = 81,
    FormatQ8W8V8U8  = 63,
    FormatV16U16    = 64,
    FormatD32FLockable = 82,
    FormatD24FS8    = 83,
    FormatQ16W16V16U16 = 110,
    FormatR16F      = 111,
    FormatG16R16F   = 112,
    FormatA16B16G16R16F = 113,
    FormatR32F      = 114,
    FormatG32R32F   = 115,
    FormatA32B32G32R32F = 116,
    FormatINTZ      = 0x5a544e49,   // depth, sampled as depth
    FormatDF24      = 0x34324644,
    FormatDF16      = 0x36314644,
    FormatRAWZ      = 0x5a574152,
    FormatNULL      = 0x4c4c554e,   // render target without memory
    FormatATI1      = 0x31495441,   // BC4
    FormatATI2      = 0x32495441,   // BC5
  };

  // D3DPRIMITIVETYPE
  enum PrimitiveType : uint32_t {
    PrimitivePointList     = 1,
    PrimitiveLineList      = 2,
    PrimitiveLineStrip     = 3,
    PrimitiveTriangleList  = 4,
    PrimitiveTriangleStrip = 5,
    PrimitiveTriangleFan   = 6,
  };

  // D3DRENDERSTATETYPE (Direct3D 9 values; Direct3D 7's coincide where they exist).
  enum RenderState : uint32_t {
    RsZEnable              = 7,
    RsFillMode             = 8,
    RsShadeMode            = 9,
    RsZWriteEnable         = 14,
    RsAlphaTestEnable      = 15,
    RsLastPixel            = 16,
    RsSrcBlend             = 19,
    RsDestBlend            = 20,
    RsCullMode             = 22,
    RsZFunc                = 23,
    RsAlphaRef             = 24,
    RsAlphaFunc            = 25,
    RsDitherEnable         = 26,
    RsAlphaBlendEnable     = 27,
    RsFogEnable            = 28,
    RsSpecularEnable       = 29,
    RsFogColor             = 34,
    RsFogTableMode         = 35,
    RsFogStart             = 36,
    RsFogEnd               = 37,
    RsFogDensity           = 38,
    RsZBias                = 47,   // Direct3D 7/8 integer depth bias
    RsRangeFogEnable       = 48,
    RsStencilEnable        = 52,
    RsStencilFail          = 53,
    RsStencilZFail         = 54,
    RsStencilPass          = 55,
    RsStencilFunc          = 56,
    RsStencilRef           = 57,
    RsStencilMask          = 58,
    RsStencilWriteMask     = 59,
    RsTextureFactor        = 60,
    RsClipping             = 136,
    RsLighting             = 137,
    RsAmbient              = 139,
    RsFogVertexMode        = 140,
    RsColorVertex          = 141,
    RsLocalViewer          = 142,
    RsNormalizeNormals     = 143,
    RsDiffuseMaterialSource  = 145,
    RsSpecularMaterialSource = 146,
    RsAmbientMaterialSource  = 147,
    RsEmissiveMaterialSource = 148,
    RsVertexBlend          = 151,
    RsClipPlaneEnable      = 152,
    RsMultisampleAntialias = 161,
    RsColorWriteEnable     = 168,
    RsBlendOp              = 171,
    RsScissorTestEnable    = 174,
    RsSlopeScaleDepthBias  = 175,
    RsTwoSidedStencilMode  = 185,
    RsCcwStencilFail       = 186,
    RsCcwStencilZFail      = 187,
    RsCcwStencilPass       = 188,
    RsCcwStencilFunc       = 189,
    RsPointSize            = 154,
    RsPointSizeMin         = 155,
    RsPointSpriteEnable    = 156,
    RsPointScaleEnable     = 157,
    RsPointSizeMax         = 166,
    RsColorWriteEnable1    = 190,
    RsColorWriteEnable2    = 191,
    RsColorWriteEnable3    = 192,
    RsBlendFactor          = 193,
    RsSrgbWriteEnable      = 194,
    RsDepthBias            = 195,
    RsSeparateAlphaBlendEnable = 206,
    RsSrcBlendAlpha        = 207,
    RsDestBlendAlpha       = 208,
    RsBlendOpAlpha         = 209,
    RsIndexedVertexBlendEnable = 167,
    RsCount                = 210,
  };

  // D3DTEXTURESTAGESTATETYPE (Direct3D 9).
  enum TextureStageState : uint32_t {
    TssColorOp       = 1,
    TssColorArg1     = 2,
    TssColorArg2     = 3,
    TssAlphaOp       = 4,
    TssAlphaArg1     = 5,
    TssAlphaArg2     = 6,
    TssBumpEnvMat00  = 7,
    TssBumpEnvMat01  = 8,
    TssBumpEnvMat10  = 9,
    TssBumpEnvMat11  = 10,
    TssTexCoordIndex = 11,
    TssBumpEnvLScale = 22,
    TssBumpEnvLOffset = 23,
    TssTextureTransformFlags = 24,
    TssColorArg0     = 26,
    TssAlphaArg0     = 27,
    TssResultArg     = 28,
    TssConstant      = 32,
    TssCount         = 33,
  };

  // D3DSAMPLERSTATETYPE (Direct3D 9).
  enum SamplerState : uint32_t {
    SampAddressU     = 1,
    SampAddressV     = 2,
    SampAddressW     = 3,
    SampBorderColor  = 4,
    SampMagFilter    = 5,
    SampMinFilter    = 6,
    SampMipFilter    = 7,
    SampMipMapLodBias = 8,
    SampMaxMipLevel  = 9,
    SampMaxAnisotropy = 10,
    SampSrgbTexture  = 11,
    SampCount        = 14,
  };

  // Texture stages of the fixed function, samplers of pixel shaders, and
  // the four of vertex shaders after them (D3DVERTEXTEXTURESAMPLER0-3).
  constexpr uint32_t kFixedStages = 8;
  constexpr uint32_t kPixelSamplers = 16;
  constexpr uint32_t kVertexSamplerBase = 16;
  constexpr uint32_t kSamplerCount = 20;
  constexpr uint32_t kMaxRenderTargets = 4;
  constexpr uint32_t kMaxStreams = 16;

  // D3DDECLTYPE
  enum DeclType : uint8_t {
    DeclFloat1 = 0, DeclFloat2, DeclFloat3, DeclFloat4, DeclD3DColor, DeclUByte4, DeclShort2, DeclShort4,
    DeclUByte4N, DeclShort2N, DeclShort4N, DeclUShort2N, DeclUShort4N, DeclUDec3, DeclDec3N, DeclFloat16x2,
    DeclFloat16x4, DeclUnused,
  };

  // D3DVERTEXELEMENT9
  struct VertexElement {
    uint16_t stream;
    uint16_t offset;
    uint8_t  type;
    uint8_t  method;
    uint8_t  usage;
    uint8_t  usageIndex;
  };

  // D3DTRANSFORMSTATETYPE
  enum TransformState : uint32_t {
    TransformView       = 2,
    TransformProjection = 3,
    TransformTexture0   = 16,
    TransformWorld      = 256,
  };

  struct Color { float r, g, b, a; };
  struct Vector { float x, y, z; };

  // D3DMATERIAL7/8/9
  struct Material {
    Color diffuse;
    Color ambient;
    Color specular;
    Color emissive;
    float power;
  };

  // D3DLIGHT7/8/9 (type: 1 point, 2 spot, 3 directional)
  struct Light {
    uint32_t type;
    Color    diffuse;
    Color    specular;
    Color    ambient;
    Vector   position;
    Vector   direction;
    float    range;
    float    falloff;
    float    attenuation0;
    float    attenuation1;
    float    attenuation2;
    float    theta;
    float    phi;
  };

  struct Viewport {
    uint32_t x, y, width, height;
    float minZ, maxZ;
  };

  struct Rect { int32_t left, top, right, bottom; };

  // Clear flags (D3DCLEAR_*).
  constexpr uint32_t ClearTarget  = 1;
  constexpr uint32_t ClearZBuffer = 2;
  constexpr uint32_t ClearStencil = 4;

  // Flexible vertex format bits (D3DFVF_*), the same in Direct3D 7 to 9.
  constexpr uint32_t FvfPositionMask = 0x400e;
  constexpr uint32_t FvfXyz      = 0x002;
  constexpr uint32_t FvfXyzRhw   = 0x004;
  constexpr uint32_t FvfXyzB1    = 0x006;
  constexpr uint32_t FvfXyzB5    = 0x00e;
  constexpr uint32_t FvfXyzW     = 0x4002;
  constexpr uint32_t FvfNormal   = 0x010;
  constexpr uint32_t FvfPSize    = 0x020;
  constexpr uint32_t FvfDiffuse  = 0x040;
  constexpr uint32_t FvfSpecular = 0x080;
  constexpr uint32_t FvfTexCountMask  = 0xf00;
  constexpr uint32_t FvfTexCountShift = 8;
  constexpr uint32_t FvfLastBetaUByte4  = 0x1000;
  constexpr uint32_t FvfLastBetaD3DColor = 0x8000;

  // Size in bytes of one vertex of a flexible vertex format.
  uint32_t fvfStride(uint32_t fvf);

}
