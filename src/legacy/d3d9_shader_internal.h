#pragma once

// The Direct3D 9 shader translator's program model, shared by its HLSL
// writer (d3d9_shader.cpp) and its DXBC emitter (d3d9_dxbc.cpp).

#include "d3d9_shader.h"

#include <array>
#include <map>
#include <vector>

namespace dxup::legacy::sm9 {

  enum Opcode : uint32_t {
    OpNop = 0, OpMov, OpAdd, OpSub, OpMad, OpMul, OpRcp, OpRsq, OpDp3, OpDp4, OpMin, OpMax, OpSlt, OpSge, OpExp,
    OpLog, OpLit, OpDst, OpLrp, OpFrc, OpM4x4, OpM4x3, OpM3x4, OpM3x3, OpM3x2, OpCall, OpCallNz, OpLoop, OpRet,
    OpEndLoop, OpLabel, OpDcl, OpPow, OpCrs, OpSgn, OpAbs, OpNrm, OpSinCos, OpRep, OpEndRep, OpIf, OpIfC, OpElse,
    OpEndIf, OpBreak, OpBreakC, OpMova, OpDefB, OpDefI,
    OpTexCoord = 64, OpTexKill, OpTex, OpTexBem, OpTexBemL, OpTexReg2Ar, OpTexReg2Gb, OpTexM3x2Pad, OpTexM3x2Tex,
    OpTexM3x3Pad, OpTexM3x3Tex, OpReserved0, OpTexM3x3Spec, OpTexM3x3VSpec, OpExpP, OpLogP, OpCnd, OpDef,
    OpTexReg2Rgb, OpTexDp3Tex, OpTexM3x2Depth, OpTexDp3, OpTexM3x3, OpTexDepth, OpCmp, OpBem, OpDp2Add, OpDsx, OpDsy,
    OpTexLdd, OpSetP, OpTexLdl, OpBreakP,
    OpPhase = 0xfffd, OpComment = 0xfffe, OpEnd = 0xffff,
  };

  enum RegType : uint32_t {
    RegTemp = 0, RegInput = 1, RegConst = 2, RegAddr = 3 /* texture in pixel shaders */, RegRastOut = 4,
    RegAttrOut = 5, RegOutput = 6 /* texture coordinates before 3.0 */, RegConstInt = 7, RegColorOut = 8,
    RegDepthOut = 9, RegSampler = 10, RegConst2 = 11, RegConst3 = 12, RegConst4 = 13, RegConstBool = 14,
    RegLoop = 15, RegTempFloat16 = 16, RegMisc = 17, RegLabel = 18, RegPredicate = 19,
  };

  constexpr uint32_t kModifierNot = 13;

  struct Operand {
    uint32_t type = 0;
    uint32_t index = 0;
    uint32_t swizzle = 0xe4;   // xyzw
    uint32_t mask = 0xf;
    uint32_t modifier = 0;     // source modifier
    uint32_t resultModifier = 0;
    int32_t  shift = 0;
    bool     relative = false;
    uint32_t relType = RegAddr;
    uint32_t relIndex = 0;
    uint32_t relComponent = 0;
  };

  struct Instruction {
    uint32_t opcode = 0;
    uint32_t controls = 0;
    bool     predicated = false;
    Operand  predicate;
    std::vector<Operand> dst;
    std::vector<Operand> src;
    uint32_t literal[4] = {};
    uint32_t declToken = 0;
  };

  // Varying slots: register i + 1 of a vertex shader's outputs and a pixel
  // shader's inputs (0 is the position), the same for the fixed function.
  struct SlotUsage { uint8_t usage, index; };
  extern const SlotUsage kSlots[kVaryingSlots9];
  constexpr uint32_t kSlotFog = 0, kSlotColor = 1, kSlotTexCoord = 3;

  // The program and what of it the HLSL translation uses: the DXBC emitter
  // declares and links exactly the same registers.
  struct Layout9 {
    std::vector<Instruction> instructions;
    std::map<uint32_t, std::array<uint32_t, 4>> floatDefs;   // c#: the literal's bits
    std::map<uint32_t, std::array<int32_t, 4>>  intDefs;
    std::map<uint32_t, bool>                    boolDefs;
    bool     relativeInputs = false, relativeOutputs = false;
    uint32_t usedInputs = 0, usedOutputs = 0, usedT = 0, usedTc = 0, usedTm = 0, usedColors = 0;
    uint32_t colorInputs = 0;       // pixel shaders before 3.0: v0/v1 read
    uint32_t texCoordInputs = 0;    // pixel shaders before 3.0: texture coordinates read
    uint32_t colorOutputMask = 0;   // vertex shaders before 3.0: oD written
    uint32_t texCoordOutputs = 0;   // vertex shaders before 3.0: oT written
    bool     writesPointSize = false;
    bool     usesVFace = false;
    uint32_t maxTemp = 0;
  };

  // translateShader9 that also gives the layout.
  bool translateShader9(const uint32_t* tokens, size_t tokenCount, const TranslateOptions9& options,
                        std::string& hlsl, ShaderInfo9& info, std::string& error, Layout9* layout);

}
