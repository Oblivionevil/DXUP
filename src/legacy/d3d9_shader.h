#pragma once

// Direct3D 8/9 shaders (vs_1_1 to vs_3_0, ps_1_1 to ps_3_0) as HLSL for
// shader model 5, compiled once and kept: the token stream is read into
// an instruction list and written out as HLSL statements on register
// arrays, with Direct3D 9's semantics (rsq of |x|, lit, sincos, cmp, the
// partial-precision and shift modifiers, texture projection, relative
// addressing, loops over integer constants, predicates).
//
// Stages link through fixed varying slots: a usage and index has one slot
// (varyingSlot9), vertex shaders write the slots below their count and
// pixel shaders declare the slots below theirs, in slot order. Direct3D 12
// links by register, so a pixel shader needs a vertex shader with at least
// its count: the vertex shader is translated again with minVaryings then.

#include <cstdint>
#include <string>
#include <vector>

namespace dxup::legacy {

  enum class ShaderStage9 { Vertex, Pixel };

  // D3DDECLUSAGE
  enum DeclUsage : uint8_t {
    UsagePosition = 0, UsageBlendWeight = 1, UsageBlendIndices = 2, UsageNormal = 3, UsagePSize = 4,
    UsageTexCoord = 5, UsageTangent = 6, UsageBinormal = 7, UsageTessFactor = 8, UsagePositionT = 9,
    UsageColor = 10, UsageFog = 11, UsageDepth = 12, UsageSample = 13,
  };

  struct ShaderIo9 {
    uint32_t reg;          // register number
    uint8_t  usage;        // DeclUsage
    uint8_t  usageIndex;
    uint8_t  mask;         // components declared (xyzw bits)
  };

  // Texture types of samplers (D3DSAMPLER_TEXTURE_TYPE >> 27).
  enum SamplerType9 : uint8_t { Sampler2D = 2, SamplerCube = 3, SamplerVolume = 4 };

  struct ShaderInfo9 {
    ShaderStage9 stage = ShaderStage9::Vertex;
    uint32_t major = 0, minor = 0;
    std::vector<ShaderIo9> inputs;     // vertex shader: vertex elements by usage
    std::vector<ShaderIo9> outputs;    // vertex shader: varyings by usage
    uint8_t  samplerTypes[16] = {};    // pixel samplers (vertex: 0-3), 0 = unused
    uint32_t floatConstantCount = 0;   // highest float constant read + 1 (relative addressing: all)
    uint32_t varyingCount = 0;         // varying slots written (vertex) or declared (pixel)
    bool     writesPointSize = false;  // vertex: point size in slot 0's z (w = 1)
    bool     relativeConstants = false;
    bool     writesDepth = false;
    uint32_t colorOutputs = 0;         // bitmask of oC0-3
  };

  struct TranslateOptions9 {
    // Types of samplers a shader does not declare (shader model 1 pixel
    // shaders sample whatever is bound), 16 entries or null.
    const uint8_t* boundSamplerTypes = nullptr;
    // Vertex shaders: write at least this many varying slots.
    uint32_t minVaryings = 0;
    // Pixel shaders: samplers of depth textures compared with the third
    // coordinate (shadow maps), the result in all components.
    uint16_t shadowSamplers = 0;
    // Vertex shaders: user clip planes enabled (clip space, cbuffer b3).
    uint8_t clipPlanes = 0;
    // 0 times infinity or NaN gives 0 (as Direct3D 9 drivers do) in mul and
    // mad (floatEmulation), and in dp2add, dp3, dp4 and the matrix products
    // (dotEmulation); IEEE gives NaN.
    bool floatEmulation = false;
    bool dotEmulation = false;
    // Vertex shaders: a NaN depth becomes 0 (drawn at depth 0, as Intel's
    // driver does; Direct3D 11 and NVIDIA's Direct3D 9 drop the primitive).
    bool nanDepthZero = false;
  };

  constexpr uint32_t kVaryingSlots9 = 30;

  // The slot of a varying, -1 for usages that cannot link.
  int varyingSlot9(uint8_t usage, uint8_t usageIndex);
  // The semantic of a slot.
  std::string varyingSemantic9(uint32_t slot);

  // Writes the HLSL of a shader.
  // The shader as DXBC (shader model 5.0) without HLSL or the HLSL compiler
  // (d3d9_dxbc.cpp): the same registers, linkage and info as the HLSL. False
  // (error says why) for what the emitter does not handle.
  bool compileShader9(const uint32_t* tokens, size_t tokenCount, const TranslateOptions9& options,
                      std::vector<uint8_t>& dxbc, ShaderInfo9& info, std::string& error);

  bool translateShader9(const uint32_t* tokens, size_t tokenCount, const TranslateOptions9& options,
                        std::string& hlsl, ShaderInfo9& info, std::string& error);

  // The semantic Direct3D 11 stages and input layouts use for a usage.
  std::string semantic9(uint8_t usage, uint8_t usageIndex);
  // Its name alone (input layouts give the index apart).
  const char* semanticName9(uint8_t usage);

}
