// Direct3D 9 shaders straight to DXBC (shader model 5.0), without HLSL and
// the HLSL compiler: what the HLSL path (d3d9_shader.cpp) writes, as DXBC
// instructions. fxc takes 20 to 120 ms for a large shader, this a fraction
// of a millisecond, so shaders compile when the game creates them, as with
// DXVK. The layout (registers, varying slots, ShaderInfo9) comes from the
// HLSL translation, so both paths link the same; what this emitter does not
// handle fails, and the HLSL path takes the shader.

#include "d3d9_shader_internal.h"
#include "../d3d12/d3d12_dxbc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace dxup::legacy {

  namespace {

    using namespace sm9;

    // ---- DXBC tokens (d3d11TokenizedProgramFormat) ----

    enum DxOpcode : uint32_t {
      DxAdd = 0, DxAnd = 1, DxBreak = 2, DxBreakC = 3, DxCall = 4, DxCallC = 5, DxDiscard = 13, DxDiv = 14, DxDp2 = 15,
      DxDp3 = 16, DxDp4 = 17, DxElse = 18, DxEndIf = 21, DxEndLoop = 22, DxEq = 24, DxExp = 25, DxFrc = 26, DxFtoI = 27,
      DxGe = 29, DxIAdd = 30, DxIf = 31, DxIEq = 32, DxIGe = 33, DxILt = 34, DxINe = 39, DxItoF = 43, DxLabel = 44,
      DxLog = 47, DxLoop = 48, DxLt = 49, DxMad = 50, DxMin = 51, DxMax = 52, DxMov = 54, DxMovC = 55, DxMul = 56,
      DxNe = 57, DxOr = 60, DxRet = 62, DxRoundNe = 64, DxRoundNi = 65, DxRsq = 68, DxSample = 69, DxSampleCLz = 71,
      DxSampleL = 72, DxSampleD = 73, DxSampleB = 74, DxSqrt = 75, DxSinCos = 77, DxULt = 79, DxUMin = 84, DxUShr = 85, DxUtoF = 86,
      DxDclResource = 88, DxDclConstantBuffer = 89, DxDclSampler = 90, DxDclInput = 95, DxDclInputPs = 98,
      DxDclInputPsSgv = 99, DxDclInputPsSiv = 100, DxDclOutput = 101, DxDclOutputSiv = 103, DxDclTemps = 104,
      DxDclIndexableTemp = 105, DxDclGlobalFlags = 106, DxDerivRtxCoarse = 122, DxDerivRtyCoarse = 124, DxUBfe = 138,
      DxIBfe = 139,
    };

    enum DxOperandType : uint32_t {
      OtTemp = 0, OtInput = 1, OtOutput = 2, OtIndexableTemp = 3, OtImmediate = 4, OtSampler = 6, OtResource = 7,
      OtConstantBuffer = 8, OtLabel = 10, OtOutputDepth = 12, OtNull = 13,
    };

    constexpr uint32_t kTestNonzero = 1u << 18;
    constexpr uint32_t kSaturate = 1u << 13;
    constexpr uint32_t kFltMax = 0x7f7fffffu;   // 3.402823466e38f
    constexpr uint32_t kOne = 0x3f800000u;
    constexpr uint32_t kSwizzleXyzw = 0xe4;

    uint32_t floatBits(float f) {
      uint32_t bits;
      std::memcpy(&bits, &f, 4);
      return bits;
    }

    // Swizzle s applied after swizzle t: component i of the result reads t's
    // component s[i].
    uint32_t composeSwizzle(uint32_t t, uint32_t s) {
      uint32_t r = 0;
      for (int i = 0; i < 4; i++)
        r |= ((t >> (2 * ((s >> (2 * i)) & 3))) & 3) << (2 * i);
      return r;
    }

    uint32_t replicate(uint32_t c) { return c * 0x55; }

    struct Op {
      uint32_t type = OtTemp;
      uint32_t comps = 2;       // 0 none, 1 one, 2 four
      uint32_t mode = 1;        // four: 0 mask, 1 swizzle, 2 select one
      uint32_t sel = kSwizzleXyzw;
      uint32_t dims = 1;
      uint32_t index[2] = {};
      int      relDim = -1;     // dimension with a relative part
      bool     relImm = false;  // immediate plus relative
      uint32_t relReg = 0, relComp = 0;
      uint32_t mod = 0;         // 1 negate, 2 abs, 3 negated abs
      uint32_t imm[4] = {};

      bool immediate() const { return type == OtImmediate; }

      void encode(std::vector<uint32_t>& out) const {
        uint32_t token = comps;
        if (comps == 2)
          token |= (mode << 2) | (sel << 4);
        token |= type << 12;
        token |= dims << 20;
        for (uint32_t d = 0; d < dims; d++)
          if (int(d) == relDim)
            token |= (relImm ? 3u : 2u) << (22 + 3 * d);
        if (mod)
          token |= 1u << 31;
        out.push_back(token);
        if (mod)
          out.push_back(1u | (mod << 6));
        if (type == OtImmediate) {
          for (uint32_t i = 0; i < (comps == 1 ? 1u : 4u); i++)
            out.push_back(imm[i]);
          return;
        }
        for (uint32_t d = 0; d < dims; d++) {
          if (int(d) != relDim || relImm)
            out.push_back(index[d]);
          if (int(d) == relDim) {
            // The relative part: a temp's component.
            out.push_back(2u | (2u << 2) | (relComp << 4) | (OtTemp << 12) | (1u << 20));
            out.push_back(relReg);
          }
        }
      }
    };

    // ---- Operand constructors ----

    Op reg(uint32_t type, uint32_t index, uint32_t swizzle = kSwizzleXyzw) {
      Op o;
      o.type = type;
      o.index[0] = index;
      o.sel = swizzle;
      return o;
    }
    Op temp(uint32_t r, uint32_t swizzle = kSwizzleXyzw) { return reg(OtTemp, r, swizzle); }
    Op dstOf(Op o, uint32_t mask) {
      o.mode = 0;
      o.sel = mask & 0xf;
      o.mod = 0;
      return o;
    }
    Op tempDst(uint32_t r, uint32_t mask = 0xf) { return dstOf(temp(r), mask); }
    Op immBits(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
      Op o;
      o.type = OtImmediate;
      o.dims = 0;
      o.imm[0] = a; o.imm[1] = b; o.imm[2] = c; o.imm[3] = d;
      return o;
    }
    Op immF(float a, float b, float c, float d) { return immBits(floatBits(a), floatBits(b), floatBits(c), floatBits(d)); }
    Op immF(float a) { return immF(a, a, a, a); }
    Op immU(uint32_t a) { return immBits(a, a, a, a); }
    Op cbuf(uint32_t slot, uint32_t index, uint32_t swizzle = kSwizzleXyzw) {
      Op o;
      o.type = OtConstantBuffer;
      o.dims = 2;
      o.index[0] = slot;
      o.index[1] = index;
      o.sel = swizzle;
      return o;
    }
    Op xtemp(uint32_t array, uint32_t index) {
      Op o;
      o.type = OtIndexableTemp;
      o.dims = 2;
      o.index[0] = array;
      o.index[1] = index;
      return o;
    }
    Op label(uint32_t index) {
      Op o;
      o.type = OtLabel;
      o.comps = 0;
      o.index[0] = index;
      return o;
    }
    Op nullOp() {
      Op o;
      o.type = OtNull;
      o.comps = 0;
      o.dims = 0;
      return o;
    }

    // The operand with swizzle s on top.
    Op swz(Op o, uint32_t s) {
      if (o.immediate()) {
        uint32_t v[4];
        for (int i = 0; i < 4; i++)
          v[i] = o.imm[(s >> (2 * i)) & 3];
        std::memcpy(o.imm, v, sizeof(v));
        return o;
      }
      if (o.mode == 1)
        o.sel = composeSwizzle(o.sel, s);
      return o;
    }
    // Component c of the operand, in all four.
    Op comp(Op o, uint32_t c) { return swz(o, replicate(c)); }
    // Component c as a scalar operand (flow control, select one).
    Op scalar(Op o, uint32_t c = 0) {
      o = comp(o, c);
      if (o.immediate()) {
        o.comps = 1;
        return o;
      }
      o.mode = 2;
      o.sel = o.sel & 3;
      return o;
    }
    Op neg(Op o) {
      if (o.immediate()) {
        for (uint32_t& v : o.imm) v ^= 0x80000000u;
        return o;
      }
      o.mod = o.mod == 0 ? 1 : o.mod == 1 ? 0 : o.mod == 2 ? 3 : 2;
      return o;
    }
    Op absOf(Op o) {
      if (o.immediate()) {
        for (uint32_t& v : o.imm) v &= 0x7fffffffu;
        return o;
      }
      o.mod = 2;
      return o;
    }

    uint32_t maskOf(uint32_t mask) { return mask ? mask : 0xf; }

    // ---- The emitter ----

    struct Emitter {
      const Layout9& L;
      ShaderInfo9& info;
      const TranslateOptions9& options;
      bool vertex;
      uint32_t major, minor;
      std::vector<uint32_t> code;
      std::string error;

      // Registers (temps unless noted).
      uint32_t nextTemp = 0;
      uint32_t rBase = 0;
      uint32_t vMap[16] = {};
      uint32_t oMap[12] = {};
      static constexpr uint32_t kInputArray = 0, kOutputArray = 1;
      uint32_t A0 = 0, AL = 0, P0 = 0;
      uint32_t OPOS = 0, OFOG = 0, OPTS = 0, OD[2] = {};
      uint32_t OC[4] = {}, T[8] = {}, TC[8] = {}, TM[2] = {}, ODEPTH = 0, VPOS = 0, VFACE = 0;
      std::vector<uint32_t> loopTemps;   // per depth: counter, count, step, saved aL
      uint32_t scratchBase = 0, scratchNext = 0, maxTemps = 0;

      // Flow control.
      uint32_t loopDepth = 0, ifDepth = 0;
      std::vector<uint32_t> loopKinds;   // OpRep or OpLoop per depth
      bool inFunction = false, mainDone = false, functionOpen = false;
      bool usesRelativeConstants = false;
      bool readsVPos = false;

      // Pixel shader inputs read (masks per register, 0 position).
      uint32_t inputRead[kVaryingSlots9 + 2] = {};
      uint32_t frontFaceRegister = 0;

      Emitter(const Layout9& layout, ShaderInfo9& i, const TranslateOptions9& o)
      : L(layout), info(i), options(o), vertex(i.stage == ShaderStage9::Vertex), major(i.major), minor(i.minor) {}

      bool ps1() const { return !vertex && major == 1; }
      bool texCoordRegisters() const { return !vertex && (major == 2 || (major == 1 && minor == 4)); }

      uint32_t allocate(uint32_t count = 1) {
        const uint32_t r = nextTemp;
        nextTemp += count;
        return r;
      }

      uint32_t scratch() {
        const uint32_t r = scratchNext++;
        maxTemps = (std::max)(maxTemps, scratchNext);
        return r;
      }

      void emit(uint32_t opcode, std::initializer_list<Op> ops, uint32_t controls = 0) {
        const size_t start = code.size();
        code.push_back(0);
        for (const Op& o : ops)
          o.encode(code);
        code[start] = opcode | controls | (uint32_t(code.size() - start) << 24);
      }

      void fail(const std::string& why) {
        if (error.empty())
          error = why;
      }

      // ---- Register allocation, as the HLSL declares its statics ----

      void allocateRegisters() {
        rBase = allocate((std::max)(L.maxTemp, 1u));
        if (!L.relativeInputs)
          for (uint32_t i = 0; i < 16; i++)
            if ((L.usedInputs >> i) & 1) vMap[i] = allocate();
        A0 = allocate();
        AL = allocate();
        P0 = allocate();
        if (vertex) {
          OPOS = allocate(); OFOG = allocate(); OPTS = allocate(); OD[0] = allocate(); OD[1] = allocate();
          if (!L.relativeOutputs)
            for (uint32_t i = 0; i < 12; i++)
              if ((L.usedOutputs >> i) & 1) oMap[i] = allocate();
        } else {
          for (uint32_t i = 0; i < 4; i++) if ((L.usedColors >> i) & 1) OC[i] = allocate();
          for (uint32_t i = 0; i < 8; i++) if ((L.usedT >> i) & 1) T[i] = allocate();
          for (uint32_t i = 0; i < 8; i++) if ((L.usedTc >> i) & 1) TC[i] = allocate();
          if (L.usedTm & 0x0f) TM[0] = allocate();
          if (L.usedTm & 0xf0) TM[1] = allocate();
          ODEPTH = allocate(); VPOS = allocate(); VFACE = allocate();
        }
        uint32_t depth = 0, maxDepth = 0;
        for (const auto& ins : L.instructions) {
          if (ins.opcode == OpRep || ins.opcode == OpLoop) maxDepth = (std::max)(maxDepth, ++depth);
          if ((ins.opcode == OpEndRep || ins.opcode == OpEndLoop) && depth) depth--;
          for (const auto* ops : { &ins.dst, &ins.src })
            for (const Operand& op : *ops)
              if (op.type == RegMisc && op.index == 0) readsVPos = true;
        }
        for (uint32_t i = 0; i < maxDepth; i++)
          loopTemps.push_back(allocate());
        scratchBase = scratchNext = maxTemps = nextTemp;
      }

      // A def constant as its literal: infinity the largest float, NaN 0
      // (as the HLSL literal).
      Op defLiteral(const std::array<uint32_t, 4>& bits) {
        uint32_t v[4];
        for (int i = 0; i < 4; i++) {
          const uint32_t b = bits[i];
          v[i] = (b & 0x7f800000u) == 0x7f800000u ? ((b & 0x007fffffu) ? 0u : (b & 0x80000000u) | kFltMax) : b;
        }
        return immBits(v[0], v[1], v[2], v[3]);
      }

      Op relativeIndex(const Operand& op) const {
        Op r;
        if (op.relType == RegLoop) { r.relReg = AL; r.relComp = 0; }
        else { r.relReg = A0; r.relComp = op.relComponent & 3; }
        return r;
      }

      Op arrayElement(uint32_t array, const Operand& op) const {
        Op o = xtemp(array, op.index);
        if (op.relative) {
          const Op r = relativeIndex(op);
          o.relDim = 1;
          o.relImm = true;
          o.relReg = r.relReg;
          o.relComp = r.relComp;
        }
        return o;
      }

      uint32_t texCoordTemp(uint32_t reg) const { return TC[reg & 7]; }

      Op tmComponent(uint32_t reg) const { return comp(temp(TM[(reg & 7) >> 2]), reg & 3); }

      // A register as it is read (the HLSL registerName), before the
      // instruction's swizzle and modifier.
      Op regRead(const Operand& op) {
        switch (op.type) {
          case RegTemp: case RegTempFloat16:
            return temp(rBase + op.index);
          case RegInput:
            if (op.relative || L.relativeInputs)
              return arrayElement(kInputArray, op);
            return temp(vMap[op.index & 15]);
          case RegConst: case RegConst2: case RegConst3: case RegConst4: {
            const uint32_t index = op.index + (op.type == RegConst2 ? 2048 : op.type == RegConst3 ? 4096
              : op.type == RegConst4 ? 6144 : 0);
            if (op.relative) {
              // The last index from the buffer; negative indices wrap to it.
              usesRelativeConstants = true;
              const uint32_t s = scratch();
              const Op r = relativeIndex(op);
              emit(DxIAdd, { tempDst(s, 1), scalar(temp(r.relReg), r.relComp), immU(index) });
              emit(DxUMin, { tempDst(s, 1), scalar(temp(s)), scalar(cbuf(2, 20), 0) });
              // The shader's own def constants are in the buffer too
              // (ShaderInfo9::relativeDefs).
              Op o = cbuf(0, 0);
              o.relDim = 1;
              o.relImm = false;
              o.relReg = s;
              o.relComp = 0;
              return o;
            }
            auto def = L.floatDefs.find(index);
            if (def != L.floatDefs.end())
              return defLiteral(def->second);
            if (index >= 256) {
              fail("constant beyond 255");
              return immU(0);
            }
            if (ps1()) {
              // Pixel shader 1.x constants are clamped to [-1, 1].
              const uint32_t s = scratch();
              emit(DxMax, { tempDst(s), cbuf(0, index), immF(-1.0f) });
              emit(DxMin, { tempDst(s), temp(s), immF(1.0f) });
              return temp(s);
            }
            return cbuf(0, index);
          }
          case RegAddr:
            if (vertex) {
              const uint32_t s = scratch();
              emit(DxItoF, { tempDst(s), temp(A0) });
              return temp(s);
            }
            return temp(T[op.index & 7]);
          case RegRastOut:
            return temp(op.index == 0 ? OPOS : op.index == 1 ? OFOG : OPTS);
          case RegAttrOut:
            return temp(OD[op.index & 1]);
          case RegOutput:
            if (op.relative || L.relativeOutputs)
              return arrayElement(kOutputArray, op);
            return temp(oMap[op.index % 12]);
          case RegConstInt: {
            auto def = L.intDefs.find(op.index);
            if (def != L.intDefs.end())
              return immF(float(def->second[0]), float(def->second[1]), float(def->second[2]), float(def->second[3]));
            const uint32_t s = scratch();
            emit(DxItoF, { tempDst(s), cbuf(2, op.index & 15) });
            return temp(s);
          }
          case RegColorOut:
            return temp(OC[op.index & 3]);
          case RegDepthOut:
            return temp(ODEPTH);
          case RegConstBool: {
            auto def = L.boolDefs.find(op.index);
            if (def != L.boolDefs.end())
              return immF(def->second ? 1.0f : 0.0f);
            const uint32_t s = scratch();
            emit(DxMovC, { tempDst(s), comp(cbuf(2, 16 + ((op.index & 15) >> 2)), op.index & 3), immF(1.0f), immF(0.0f) });
            return temp(s);
          }
          case RegLoop: {
            const uint32_t s = scratch();
            emit(DxItoF, { tempDst(s), comp(temp(AL), 0) });
            return temp(s);
          }
          case RegMisc:
            return temp(op.index == 0 ? VPOS : VFACE);
          case RegPredicate: {
            const uint32_t s = scratch();
            emit(DxAnd, { tempDst(s), temp(P0), immU(kOne) });
            return temp(s);
          }
          default:
            fail("unsupported register type " + std::to_string(op.type));
            return immU(0);
        }
      }

      // A register as a destination (mask applied by the caller).
      Op regWrite(const Operand& d) {
        switch (d.type) {
          case RegTemp: case RegTempFloat16: return temp(rBase + d.index);
          case RegAddr: return temp(T[d.index & 7]);
          case RegRastOut: return temp(d.index == 0 ? OPOS : d.index == 1 ? OFOG : OPTS);
          case RegAttrOut: return temp(OD[d.index & 1]);
          case RegOutput:
            if (d.relative || L.relativeOutputs)
              return arrayElement(kOutputArray, d);
            return temp(oMap[d.index % 12]);
          case RegColorOut: return temp(OC[d.index & 3]);
          case RegDepthOut: return temp(ODEPTH);
          default:
            fail("unsupported destination register type " + std::to_string(d.type));
            return temp(rBase);
        }
      }

      // A source operand: register, swizzle, modifier (the HLSL source()).
      Op source(const Operand& op) {
        Op value = regRead(op);
        if (op.type == RegConstBool)
          return value;
        if (op.swizzle != kSwizzleXyzw)
          value = swz(value, op.swizzle);
        switch (op.modifier) {
          case 0: return value;
          case 1: return neg(value);
          case 2: { const uint32_t s = scratch(); emit(DxAdd, { tempDst(s), value, immF(-0.5f) }); return temp(s); }
          case 3: { const uint32_t s = scratch(); emit(DxAdd, { tempDst(s), neg(value), immF(0.5f) }); return temp(s); }
          case 4: { const uint32_t s = scratch(); emit(DxMad, { tempDst(s), value, immF(2.0f), immF(-1.0f) }); return temp(s); }
          case 5: { const uint32_t s = scratch(); emit(DxMad, { tempDst(s), value, immF(-2.0f), immF(1.0f) }); return temp(s); }
          case 6: { const uint32_t s = scratch(); emit(DxAdd, { tempDst(s), neg(value), immF(1.0f) }); return temp(s); }
          case 7: { const uint32_t s = scratch(); emit(DxMul, { tempDst(s), value, immF(2.0f) }); return temp(s); }
          case 8: { const uint32_t s = scratch(); emit(DxMul, { tempDst(s), value, immF(-2.0f) }); return temp(s); }
          case 9: case 10: {
            const uint32_t s = scratch();
            emit(DxDiv, { tempDst(s), value, comp(value, op.modifier == 9 ? 2 : 3) });
            return temp(s);
          }
          case 11: return absOf(value);
          case 12: return neg(absOf(value));
          default: return value;
        }
      }

      Op src(const Instruction& ins, size_t i) {
        return i < ins.src.size() ? source(ins.src[i]) : immU(0);
      }
      // The x component of a source.
      Op one(const Instruction& ins, size_t i) { return comp(src(ins, i), 0); }

      // A scalar condition register: nonzero is true. not: test zero.
      struct Condition { Op op; bool zero = false; };
      Condition condition(const Operand& op) {
        if (op.type == RegPredicate)
          return { scalar(temp(P0), op.swizzle & 3), op.modifier == kModifierNot };
        if (op.type == RegConstBool && !L.boolDefs.count(op.index))
          return { scalar(cbuf(2, 16 + ((op.index & 15) >> 2)), op.index & 3), false };
        const uint32_t s = scratch();
        emit(DxNe, { tempDst(s, 1), comp(source(op), 0), immF(0.0f) });
        return { scalar(temp(s)), false };
      }

      // a OP b by a D3D9 comparison (controls), into s's mask components.
      void compare(uint32_t controls, Op dst, Op a, Op b) {
        switch (controls & 7) {
          case 1: emit(DxLt, { dst, b, a }); return;   // >
          case 2: emit(DxEq, { dst, a, b }); return;   // ==
          case 3: emit(DxGe, { dst, a, b }); return;   // >=
          case 4: emit(DxLt, { dst, a, b }); return;   // <
          case 6: emit(DxGe, { dst, b, a }); return;   // <=
          default: emit(DxNe, { dst, a, b }); return;  // !=
        }
      }

      // dst = value, with shift, saturate, mask and predicate (the HLSL write()).
      void write(const Instruction& ins, Op value) {
        if (ins.dst.empty())
          return;
        const Operand& d = ins.dst[0];
        const uint32_t mask = maskOf(d.mask);
        if (d.shift) {
          const uint32_t s = scratch();
          const float scale = d.shift > 0 ? float(1u << d.shift) : 1.0f / float(1u << (-d.shift));
          emit(DxMul, { tempDst(s), value, immF(scale) });
          value = temp(s);
        }
        const bool sat = (d.resultModifier & 1) != 0;
        if (d.type == RegAddr && vertex) {
          // a0 holds integers: vs_1_1 truncates down, later models round.
          const uint32_t s = scratch();
          if (sat) {
            emit(DxMov, { tempDst(s), value }, kSaturate);
            value = temp(s);
          }
          emit(major >= 2 ? DxRoundNe : DxRoundNi, { tempDst(s), value });
          emit(DxFtoI, { tempDst(A0, mask), temp(s) });
          return;
        }
        if (d.type == RegPredicate) {
          Op v = value;
          if (sat) {
            const uint32_t s = scratch();
            emit(DxMov, { tempDst(s), value }, kSaturate);
            v = temp(s);
          }
          emit(DxNe, { tempDst(P0, mask), v, immF(0.0f) });
          return;
        }
        const Op target = regWrite(d);
        if (ins.predicated) {
          Op v = value;
          if (sat) {
            const uint32_t s = scratch();
            emit(DxMov, { tempDst(s), value }, kSaturate);
            v = temp(s);
          }
          const Op predicate = swz(temp(P0), ins.predicate.swizzle);
          const bool notted = ins.predicate.modifier == kModifierNot;
          emit(DxMovC, { dstOf(target, mask), predicate, notted ? target : v, notted ? v : target });
          return;
        }
        emit(DxMov, { dstOf(target, mask), value }, sat ? kSaturate : 0);
      }

      // ---- Arithmetic helpers (the HLSL dxup_* functions) ----

      // 0 times anything is 0 (Direct3D 9): per component.
      Op legacyMul(Op a, Op b) {
        const uint32_t z = scratch(), m = scratch();
        emit(DxEq, { tempDst(z), a, immF(0.0f) });
        emit(DxEq, { tempDst(m), b, immF(0.0f) });
        emit(DxOr, { tempDst(z), temp(z), temp(m) });
        emit(DxMul, { tempDst(m), a, b });
        emit(DxMovC, { tempDst(m), temp(z), immF(0.0f), temp(m) });
        return temp(m);
      }

      Op mul(Op a, Op b) {
        if (options.floatEmulation)
          return legacyMul(a, b);
        const uint32_t s = scratch();
        emit(DxMul, { tempDst(s), a, b });
        return temp(s);
      }

      // (m.x + m.y) + (m.z + m.w) of the legacy products: a scalar in x.
      Op legacyDot(Op a, Op b, uint32_t width) {
        Op a4 = a;
        if (width < 4) {
          const uint32_t s = scratch();
          emit(DxMov, { tempDst(s), a });
          emit(DxMov, { tempDst(s, width == 3 ? 0x8 : 0xc), immF(0.0f) });
          a4 = temp(s);
        }
        const Op m = legacyMul(a4, b);
        const uint32_t s = scratch();
        emit(DxAdd, { tempDst(s, 0x3), swz(m, 0x08 /* x z */ | (0 << 4)), swz(m, 0x0d) });
        // s.x = m.x + m.y, s.y = m.z + m.w
        emit(DxAdd, { tempDst(s, 0x1), comp(temp(s), 0), comp(temp(s), 1) });
        return comp(temp(s), 0);
      }

      Op dot(Op a, Op b, uint32_t width) {
        if (options.dotEmulation)
          return legacyDot(a, b, width);
        const uint32_t s = scratch();
        emit(width == 4 ? DxDp4 : width == 3 ? DxDp3 : DxDp2, { tempDst(s, 1), a, b });
        return comp(temp(s), 0);
      }

      // x == 0 ? FLT_MAX : 1 / x (a scalar in x).
      Op rcp(Op x) {
        const uint32_t s = scratch();
        emit(DxEq, { tempDst(s, 2), x, immF(0.0f) });
        emit(DxDiv, { tempDst(s, 1), immF(1.0f), x });
        emit(DxMovC, { tempDst(s, 1), comp(temp(s), 1), immU(kFltMax), comp(temp(s), 0) });
        return comp(temp(s), 0);
      }

      // x == 0 ? -FLT_MAX : log2(abs(x)).
      Op log(Op x) {
        const uint32_t s = scratch();
        emit(DxLog, { tempDst(s, 1), absOf(x) });
        emit(DxEq, { tempDst(s, 2), x, immF(0.0f) });
        emit(DxMovC, { tempDst(s, 1), comp(temp(s), 1), immU(kFltMax | 0x80000000u), comp(temp(s), 0) });
        return comp(temp(s), 0);
      }

      // y == 0 ? 1 : exp2(y * log(x)).
      Op pow(Op x, Op y) {
        const Op l = log(x);
        const uint32_t s = scratch();
        emit(DxMul, { tempDst(s, 1), y, l });
        emit(DxExp, { tempDst(s, 1), comp(temp(s), 0) });
        emit(DxEq, { tempDst(s, 2), y, immF(0.0f) });
        emit(DxMovC, { tempDst(s, 1), comp(temp(s), 1), immF(1.0f), comp(temp(s), 0) });
        return comp(temp(s), 0);
      }

      // ---- Sampling (the HLSL sampleFor) ----

      uint8_t samplerType(uint32_t sampler) const {
        uint8_t type = info.samplerTypes[sampler & 15];
        if (!type && options.boundSamplerTypes)
          type = options.boundSamplerTypes[sampler & 15];
        return type ? type : uint8_t(Sampler2D);
      }

      bool shadow(uint32_t sampler) const {
        return !vertex && samplerType(sampler) == Sampler2D && ((options.shadowSamplers >> (sampler & 15)) & 1);
      }

      enum class Mode { Plain, Bias, Level, Grad };

      Op sample(uint32_t sampler, Op coord, Mode mode = Mode::Plain, Op extra = immF(0.0f), Op extra2 = immF(0.0f)) {
        sampler &= 15;
        const Op resource = reg(OtResource, sampler);
        Op samplerOp = reg(OtSampler, sampler);
        samplerOp.comps = 0;
        const uint32_t s = scratch();
        if (shadow(sampler)) {
          // Depth compared with z: 1 where the reference is not farther.
          const uint32_t ref = scratch();
          emit(DxMov, { tempDst(ref, 1), comp(coord, 2) }, kSaturate);
          emit(DxSampleCLz, { tempDst(s, 1), coord, swz(resource, 0), samplerOp, scalar(temp(ref)) });
          return comp(temp(s), 0);
        }
        if (mode == Mode::Level || vertex) {
          // Vertex shaders: the level of texldl, the bias of texldb, or 0.
          if (mode == Mode::Grad)
            fail("texldd in a vertex shader");
          const Op lod = mode == Mode::Plain ? immF(0.0f) : extra;
          emit(DxSampleL, { tempDst(s), coord, resource, samplerOp, scalar(lod) });
        } else if (mode == Mode::Bias) {
          const uint32_t b = scratch();
          emit(DxMax, { tempDst(b, 1), extra, immF(-15.99f) });
          emit(DxMin, { tempDst(b, 1), comp(temp(b), 0), immF(15.99f) });
          emit(DxSampleB, { tempDst(s), coord, resource, samplerOp, scalar(temp(b)) });
        } else if (mode == Mode::Grad) {
          emit(DxSampleD, { tempDst(s), coord, resource, samplerOp, extra, extra2 });
        } else {
          emit(DxSample, { tempDst(s), coord, resource, samplerOp });
        }
        return temp(s);
      }

      // ---- Instructions ----

      Op bumpRow(uint32_t stage, uint32_t swizzle) { return swz(cbuf(1, 4 + (stage & 7)), swizzle); }

      // (dot(bm.xz, v.xy), dot(bm.yw, v.xy)) + base.xy, z = w = 0.
      Op bump(uint32_t stage, Op base, Op v) {
        const uint32_t s = scratch();
        emit(DxDp2, { tempDst(s, 1), bumpRow(stage, 0x08 /* x z */), v });
        emit(DxDp2, { tempDst(s, 2), bumpRow(stage, 0x0d /* y w */), v });
        emit(DxAdd, { tempDst(s, 3), base, temp(s) });
        emit(DxMov, { tempDst(s, 0xc), immF(0.0f) });
        return temp(s);
      }

      // dot(tc#.xyz, v.xyz) into a scalar.
      Op texDot(uint32_t reg, Op v) {
        const uint32_t s = scratch();
        emit(DxDp3, { tempDst(s, 1), temp(texCoordTemp(reg)), v });
        return comp(temp(s), 0);
      }

      bool instruction(const Instruction& ins) {
        scratchNext = scratchBase;
        const auto& s = ins.src;
        const uint32_t reg = ins.dst.empty() ? 0 : ins.dst[0].index;
        switch (ins.opcode) {
          case OpNop: case OpPhase: case OpDcl: case OpDef: case OpDefI: case OpDefB:
            return true;
          case OpMov: case OpMova: write(ins, src(ins, 0)); return true;
          case OpAdd: { const uint32_t t = scratch(); emit(DxAdd, { tempDst(t), src(ins, 0), src(ins, 1) }); write(ins, temp(t)); return true; }
          case OpSub: { const uint32_t t = scratch(); emit(DxAdd, { tempDst(t), src(ins, 0), neg(src(ins, 1)) }); write(ins, temp(t)); return true; }
          case OpMul: write(ins, mul(src(ins, 0), src(ins, 1))); return true;
          case OpMad: {
            const Op a = src(ins, 0), b = src(ins, 1), c = src(ins, 2);
            const uint32_t t = scratch();
            if (options.floatEmulation)
              emit(DxAdd, { tempDst(t), legacyMul(a, b), c });
            else
              emit(DxMad, { tempDst(t), a, b, c });
            write(ins, temp(t));
            return true;
          }
          case OpMin: { const uint32_t t = scratch(); emit(DxMin, { tempDst(t), src(ins, 0), src(ins, 1) }); write(ins, temp(t)); return true; }
          case OpMax: { const uint32_t t = scratch(); emit(DxMax, { tempDst(t), src(ins, 0), src(ins, 1) }); write(ins, temp(t)); return true; }
          case OpSlt: case OpSge: {
            const uint32_t t = scratch();
            emit(ins.opcode == OpSlt ? DxLt : DxGe, { tempDst(t), src(ins, 0), src(ins, 1) });
            emit(DxAnd, { tempDst(t), temp(t), immU(kOne) });
            write(ins, temp(t));
            return true;
          }
          case OpRcp: write(ins, rcp(one(ins, 0))); return true;
          case OpRsq: {
            const uint32_t t = scratch();
            emit(DxSqrt, { tempDst(t, 1), absOf(one(ins, 0)) });
            write(ins, rcp(comp(temp(t), 0)));
            return true;
          }
          case OpExpP:
            if (vertex && major == 1) {
              // 2^floor(x), the fraction, 2^x, 1.
              const Op x = one(ins, 0);
              const uint32_t t = scratch(), f = scratch();
              emit(DxRoundNi, { tempDst(f, 1), x });
              emit(DxExp, { tempDst(t, 1), comp(temp(f), 0) });
              emit(DxAdd, { tempDst(t, 2), x, neg(comp(temp(f), 0)) });
              emit(DxExp, { tempDst(t, 4), x });
              emit(DxMov, { tempDst(t, 8), immF(1.0f) });
              write(ins, temp(t));
              return true;
            }
            [[fallthrough]];
          case OpExp: {
            const uint32_t t = scratch();
            emit(DxExp, { tempDst(t, 1), one(ins, 0) });
            write(ins, comp(temp(t), 0));
            return true;
          }
          case OpLogP:
            if (vertex && major == 1) {
              // Exponent, mantissa, log2 |x|, 1; |x| == 0: -max, 1, -max, 1.
              const Op x = one(ins, 0);
              const uint32_t a = scratch(), t = scratch();
              emit(DxMov, { tempDst(a, 1), absOf(x) });
              emit(DxUShr, { tempDst(t, 1), comp(temp(a), 0), immU(23) });
              emit(DxIAdd, { tempDst(t, 1), comp(temp(t), 0), immU(uint32_t(-127)) });
              emit(DxItoF, { tempDst(t, 1), comp(temp(t), 0) });
              emit(DxAnd, { tempDst(t, 2), comp(temp(a), 0), immU(0x7fffffu) });
              emit(DxOr, { tempDst(t, 2), comp(temp(t), 1), immU(0x3f800000u) });
              emit(DxLog, { tempDst(t, 4), comp(temp(a), 0) });
              emit(DxMov, { tempDst(t, 8), immF(1.0f) });
              emit(DxEq, { tempDst(a, 2), comp(temp(a), 0), immF(0.0f) });
              emit(DxMovC, { tempDst(t), comp(temp(a), 1), immBits(kFltMax | 0x80000000u, kOne, kFltMax | 0x80000000u, kOne), temp(t) });
              write(ins, temp(t));
              return true;
            }
            [[fallthrough]];
          case OpLog: write(ins, log(one(ins, 0))); return true;
          case OpPow: write(ins, pow(one(ins, 0), one(ins, 1))); return true;
          case OpDp3: write(ins, dot(src(ins, 0), src(ins, 1), 3)); return true;
          case OpDp4: write(ins, dot(src(ins, 0), src(ins, 1), 4)); return true;
          case OpDp2Add: {
            const Op d = dot(src(ins, 0), src(ins, 1), 2);
            const uint32_t t = scratch();
            emit(DxAdd, { tempDst(t, 1), d, one(ins, 2) });
            write(ins, comp(temp(t), 0));
            return true;
          }
          case OpFrc: { const uint32_t t = scratch(); emit(DxFrc, { tempDst(t), src(ins, 0) }); write(ins, temp(t)); return true; }
          case OpAbs: write(ins, absOf(src(ins, 0))); return true;
          case OpSgn: {
            // (0 < x) - (x < 0) as integers.
            const Op x = src(ins, 0);
            const uint32_t a = scratch(), b = scratch();
            emit(DxLt, { tempDst(a), immF(0.0f), x });
            emit(DxLt, { tempDst(b), x, immF(0.0f) });
            emit(DxIAdd, { tempDst(a), neg(temp(a)), temp(b) });
            emit(DxItoF, { tempDst(a), temp(a) });
            write(ins, temp(a));
            return true;
          }
          case OpNrm: {
            // l = dot(x.xyz, x.xyz); l > 0 ? x * rsqrt(l) : 0.
            const Op x = src(ins, 0);
            const uint32_t l = scratch(), t = scratch();
            emit(DxDp3, { tempDst(l, 1), x, x });
            emit(DxLt, { tempDst(l, 2), immF(0.0f), comp(temp(l), 0) });
            emit(DxRsq, { tempDst(l, 1), comp(temp(l), 0) });
            emit(DxMul, { tempDst(t), x, comp(temp(l), 0) });
            emit(DxAnd, { tempDst(t), temp(t), comp(temp(l), 1) });
            write(ins, temp(t));
            return true;
          }
          case OpCrs: {
            // a.yzx * b.zxy - a.zxy * b.yzx, w = 0.
            const Op a = src(ins, 0), b = src(ins, 1);
            const uint32_t t = scratch(), r = scratch();
            emit(DxMul, { tempDst(t, 7), swz(a, 0x12 /* z x y */), swz(b, 0x09 /* y z x */) });
            emit(DxMad, { tempDst(r, 7), swz(a, 0x09), swz(b, 0x12), neg(temp(t)) });
            emit(DxMov, { tempDst(r, 8), immF(0.0f) });
            write(ins, temp(r));
            return true;
          }
          case OpLrp: {
            // src2 + src0 * (src1 - src2).
            const Op a = src(ins, 0), b = src(ins, 1), c = src(ins, 2);
            const uint32_t t = scratch();
            emit(DxAdd, { tempDst(t), b, neg(c) });
            emit(DxMad, { tempDst(t), a, temp(t), c });
            write(ins, temp(t));
            return true;
          }
          case OpCmp: {
            const Op a = src(ins, 0), b = src(ins, 1), c = src(ins, 2);
            const uint32_t t = scratch();
            emit(DxGe, { tempDst(t), a, immF(0.0f) });
            emit(DxMovC, { tempDst(t), temp(t), b, c });
            write(ins, temp(t));
            return true;
          }
          case OpCnd: {
            const Op a = src(ins, 0), b = src(ins, 1), c = src(ins, 2);
            const uint32_t t = scratch();
            if (ps1() && minor < 4) {
              // ps_1_1 to ps_1_3 compare r0.a only.
              emit(DxLt, { tempDst(t, 1), immF(0.5f), comp(a, 3) });
              emit(DxMovC, { tempDst(t), comp(temp(t), 0), b, c });
            } else {
              emit(DxLt, { tempDst(t), immF(0.5f), a });
              emit(DxMovC, { tempDst(t), temp(t), b, c });
            }
            write(ins, temp(t));
            return true;
          }
          case OpLit: {
            // (1, max(x, 0), x > 0 && y > 0 ? pow(y, clamp(w, +-127.9961)) : 0, 1).
            const Op v = src(ins, 0);
            const uint32_t p = scratch();
            emit(DxMax, { tempDst(p, 1), comp(v, 3), immF(-127.9961f) });
            emit(DxMin, { tempDst(p, 1), comp(temp(p), 0), immF(127.9961f) });
            const Op powered = pow(comp(v, 1), comp(temp(p), 0));
            const uint32_t c = scratch(), r = scratch();
            emit(DxLt, { tempDst(c, 3), immF(0.0f), swz(v, 0x04 /* x y */) });
            emit(DxAnd, { tempDst(c, 1), comp(temp(c), 0), comp(temp(c), 1) });
            emit(DxMov, { tempDst(r, 0x9), immF(1.0f) });
            emit(DxMax, { tempDst(r, 2), comp(v, 0), immF(0.0f) });
            emit(DxAnd, { tempDst(r, 4), comp(temp(c), 0), powered });
            write(ins, temp(r));
            return true;
          }
          case OpDst: {
            const Op a = src(ins, 0), b = src(ins, 1);
            const uint32_t t = scratch();
            emit(DxMov, { tempDst(t, 1), immF(1.0f) });
            emit(DxMul, { tempDst(t, 2), a, b });
            emit(DxMov, { tempDst(t, 4), a });
            emit(DxMov, { tempDst(t, 8), b });
            write(ins, temp(t));
            return true;
          }
          case OpSinCos: {
            const Op a = one(ins, 0);
            const uint32_t t = scratch();
            emit(DxSinCos, { tempDst(t, 2), tempDst(t, 1), a });
            emit(DxMov, { tempDst(t, 0xc), immF(0.0f) });
            write(ins, temp(t));
            return true;
          }
          case OpM4x4: case OpM4x3: case OpM3x4: case OpM3x3: case OpM3x2: {
            const uint32_t width = ins.opcode == OpM4x4 || ins.opcode == OpM4x3 ? 4 : 3;
            const uint32_t rows = ins.opcode == OpM4x4 || ins.opcode == OpM3x4 ? 4 : ins.opcode == OpM3x2 ? 2 : 3;
            const Op v = src(ins, 0);
            const uint32_t t = scratch();
            emit(DxMov, { tempDst(t), immF(0.0f) });
            for (uint32_t i = 0; i < rows && s.size() > 1; i++) {
              Operand row = s[1];
              row.index += i;
              const Op d = dot(v, source(row), width);
              emit(DxMov, { tempDst(t, 1u << i), d });
            }
            write(ins, temp(t));
            return true;
          }
          case OpDsx: case OpDsy: {
            const uint32_t t = scratch();
            emit(ins.opcode == OpDsx ? DxDerivRtxCoarse : DxDerivRtyCoarse, { tempDst(t), src(ins, 0) });
            write(ins, temp(t));
            return true;
          }
          case OpSetP: {
            const uint32_t t = scratch();
            compare(ins.controls, tempDst(t), src(ins, 0), src(ins, 1));
            emit(DxAnd, { tempDst(t), temp(t), immU(kOne) });
            write(ins, temp(t));
            return true;
          }

          // ---- Textures ----
          case OpTex:
            if (ps1() && minor < 4) {
              const uint32_t projected = (options.projected >> ((reg & 7) * 2)) & 3;
              if (projected) {
                const uint32_t t = scratch();
                const Op coord = temp(texCoordTemp(reg));
                emit(DxDiv, { tempDst(t), coord, comp(coord, projected) });
                write(ins, sample(reg, temp(t)));
              } else {
                write(ins, sample(reg, temp(texCoordTemp(reg))));
              }
            } else if (ps1()) {
              write(ins, sample(reg, src(ins, 0)));
            } else {
              const uint32_t sampler = s.size() > 1 ? s[1].index : 0;
              const Op coord = src(ins, 0);
              if (ins.controls == 1) {
                const uint32_t t = scratch();
                emit(DxDiv, { tempDst(t), coord, comp(coord, 3) });
                write(ins, sample(sampler, temp(t)));
              } else if (ins.controls == 2) {
                write(ins, sample(sampler, coord, Mode::Bias, comp(coord, 3)));
              } else {
                write(ins, sample(sampler, coord));
              }
            }
            return true;
          case OpTexLdl: {
            const Op coord = src(ins, 0);
            write(ins, sample(s.size() > 1 ? s[1].index : 0, coord, Mode::Level, comp(coord, 3)));
            return true;
          }
          case OpTexLdd:
            write(ins, sample(s.size() > 1 ? s[1].index : 0, src(ins, 0), Mode::Grad, src(ins, 2), src(ins, 3)));
            return true;
          case OpTexCoord: {
            const uint32_t t = scratch();
            if (ps1() && minor < 4)
              emit(DxMov, { tempDst(t, 7), temp(texCoordTemp(reg)) }, kSaturate);
            else
              emit(DxMov, { tempDst(t, 7), src(ins, 0) });
            emit(DxMov, { tempDst(t, 8), immF(1.0f) });
            write(ins, temp(t));
            return true;
          }
          case OpTexKill: {
            // ps_1_1 to ps_1_3 test the texture coordinates of t#.
            const Op value = ps1() && minor < 4 && ins.dst[0].type == RegAddr ? temp(texCoordTemp(reg)) : regRead(ins.dst[0]);
            const uint32_t mask = major >= 2 ? maskOf(ins.dst[0].mask) : 0x7;
            const uint32_t t = scratch();
            emit(DxLt, { tempDst(t, mask), value, immF(0.0f) });
            uint32_t first = ~0u;
            for (uint32_t c = 0; c < 4; c++) {
              if (!((mask >> c) & 1)) continue;
              if (first == ~0u) { first = c; continue; }
              emit(DxOr, { tempDst(t, 1u << first), comp(temp(t), first), comp(temp(t), c) });
            }
            emit(DxDiscard, { scalar(temp(t), first) }, kTestNonzero);
            return true;
          }
          case OpTexBem: case OpTexBemL: {
            const Op t = src(ins, 0);
            const Op coord = bump(reg, temp(texCoordTemp(reg)), t);
            Op c = sample(reg, coord);
            if (ins.opcode == OpTexBemL) {
              // rgb * saturate(t.z * scale + offset).
              const uint32_t l = scratch(), r = scratch();
              const Op lum = cbuf(1, 12 + (reg & 7));
              emit(DxMad, { tempDst(l, 1), comp(t, 2), comp(lum, 0), comp(lum, 1) }, kSaturate);
              emit(DxMul, { tempDst(r, 7), c, comp(temp(l), 0) });
              emit(DxMov, { tempDst(r, 8), c });
              c = temp(r);
            }
            write(ins, c);
            return true;
          }
          case OpTexReg2Ar: write(ins, sample(reg, swz(src(ins, 0), 0xf3 /* w x w w */))); return true;
          case OpTexReg2Gb: write(ins, sample(reg, swz(src(ins, 0), 0xa9 /* y z z z */))); return true;
          case OpTexReg2Rgb: write(ins, sample(reg, swz(src(ins, 0), 0xa4 /* x y z z */))); return true;
          case OpTexM3x2Pad: case OpTexM3x3Pad: {
            const Op d = texDot(reg, src(ins, 0));
            emit(DxMov, { tempDst(TM[(reg & 7) >> 2], 1u << (reg & 3)), d });
            return true;
          }
          case OpTexM3x2Tex: case OpTexM3x2Depth: {
            const Op u = tmComponent(reg - 1);
            const Op v = texDot(reg, src(ins, 0));
            if (ins.opcode == OpTexM3x2Depth) {
              // u / v, 1 where v is 0.
              const uint32_t t = scratch();
              emit(DxNe, { tempDst(t, 2), v, immF(0.0f) });
              emit(DxDiv, { tempDst(t, 1), u, v });
              emit(DxMovC, { tempDst(ODEPTH), comp(temp(t), 1), comp(temp(t), 0), immF(1.0f) });
            } else {
              const uint32_t t = scratch();
              emit(DxMov, { tempDst(t, 1), u });
              emit(DxMov, { tempDst(t, 2), v });
              emit(DxMov, { tempDst(t, 0xc), immF(0.0f) });
              write(ins, sample(reg, temp(t)));
            }
            return true;
          }
          case OpTexM3x3Tex: case OpTexM3x3: case OpTexM3x3Spec: case OpTexM3x3VSpec: {
            const uint32_t n = scratch();
            emit(DxMov, { tempDst(n, 1), tmComponent(reg - 2) });
            emit(DxMov, { tempDst(n, 2), tmComponent(reg - 1) });
            emit(DxMov, { tempDst(n, 4), texDot(reg, src(ins, 0)) });
            if (ins.opcode == OpTexM3x3) {
              emit(DxMov, { tempDst(n, 8), immF(1.0f) });
              write(ins, temp(n));
            } else if (ins.opcode == OpTexM3x3Tex) {
              emit(DxMov, { tempDst(n, 8), immF(0.0f) });
              write(ins, sample(reg, temp(n)));
            } else {
              // 2 * dot(n, e) / dot(n, n) * n - e.
              Op eye;
              if (ins.opcode == OpTexM3x3Spec) {
                eye = src(ins, 1);
              } else {
                const uint32_t e = scratch();
                emit(DxMov, { tempDst(e, 1), comp(temp(texCoordTemp(reg - 2)), 3) });
                emit(DxMov, { tempDst(e, 2), comp(temp(texCoordTemp(reg - 1)), 3) });
                emit(DxMov, { tempDst(e, 4), comp(temp(texCoordTemp(reg)), 3) });
                eye = temp(e);
              }
              const uint32_t d = scratch(), r = scratch();
              emit(DxDp3, { tempDst(d, 1), temp(n), eye });
              emit(DxMul, { tempDst(d, 1), comp(temp(d), 0), immF(2.0f) });
              emit(DxDp3, { tempDst(d, 2), temp(n), temp(n) });
              emit(DxDiv, { tempDst(d, 1), comp(temp(d), 0), comp(temp(d), 1) });
              emit(DxMad, { tempDst(r, 7), comp(temp(d), 0), temp(n), neg(eye) });
              emit(DxMov, { tempDst(r, 8), immF(0.0f) });
              write(ins, sample(reg, temp(r)));
            }
            return true;
          }
          case OpTexDp3Tex: {
            const uint32_t t = scratch();
            emit(DxMov, { tempDst(t), immF(0.0f) });
            emit(DxMov, { tempDst(t, 1), texDot(reg, src(ins, 0)) });
            write(ins, sample(reg, temp(t)));
            return true;
          }
          case OpTexDp3:
            write(ins, texDot(reg, src(ins, 0)));
            return true;
          case OpTexDepth: {
            const Op r5 = regRead(ins.dst[0]);
            const uint32_t t = scratch();
            emit(DxNe, { tempDst(t, 2), comp(r5, 1), immF(0.0f) });
            emit(DxDiv, { tempDst(t, 1), comp(r5, 0), comp(r5, 1) });
            emit(DxMovC, { tempDst(ODEPTH), comp(temp(t), 1), comp(temp(t), 0), immF(1.0f) });
            return true;
          }
          case OpBem:
            write(ins, bump(reg, src(ins, 0), src(ins, 1)));
            return true;

          // ---- Flow control ----
          case OpIf: {
            const Condition c = condition(s[0]);
            emit(DxIf, { c.op }, c.zero ? 0 : kTestNonzero);
            ifDepth++;
            return true;
          }
          case OpIfC: {
            const uint32_t t = scratch();
            compare(ins.controls, tempDst(t, 1), one(ins, 0), one(ins, 1));
            emit(DxIf, { scalar(temp(t)) }, kTestNonzero);
            ifDepth++;
            return true;
          }
          case OpElse: emit(DxElse, {}); return true;
          case OpEndIf:
            if (ifDepth) ifDepth--;
            emit(DxEndIf, {});
            return true;
          case OpRep: {
            // for (counter = 0; counter < int(i.x); counter++)
            const uint32_t lc = loopTemps[loopDepth++];
            loopKinds.push_back(OpRep);
            emit(DxFtoI, { tempDst(lc, 2), one(ins, 0) });
            emit(DxMov, { tempDst(lc, 1), immU(0) });
            emit(DxLoop, {});
            const uint32_t t = scratch();
            emit(DxIGe, { tempDst(t, 1), comp(temp(lc), 0), comp(temp(lc), 1) });
            emit(DxBreakC, { scalar(temp(t)) }, kTestNonzero);
            return true;
          }
          case OpEndRep: case OpEndLoop: {
            if (!loopDepth) {
              fail("unbalanced loop");
              return false;
            }
            const uint32_t lc = loopTemps[--loopDepth];
            const uint32_t kind = loopKinds.back();
            loopKinds.pop_back();
            emit(DxIAdd, { tempDst(lc, 1), comp(temp(lc), 0), immU(1) });
            if (kind == OpLoop)
              emit(DxIAdd, { tempDst(AL, 1), comp(temp(AL), 0), comp(temp(lc), 2) });
            emit(DxEndLoop, {});
            if (kind == OpLoop)
              emit(DxMov, { tempDst(AL, 1), comp(temp(lc), 3) });
            return true;
          }
          case OpLoop: {
            // loop aL, i#: count, start, step; aL restored after the loop.
            const uint32_t lc = loopTemps[loopDepth++];
            loopKinds.push_back(OpLoop);
            const uint32_t t = scratch();
            emit(DxFtoI, { tempDst(t, 7), src(ins, 1) });
            emit(DxMov, { tempDst(lc, 8), comp(temp(AL), 0) });
            emit(DxMov, { tempDst(lc, 2), comp(temp(t), 0) });
            emit(DxMov, { tempDst(lc, 4), comp(temp(t), 2) });
            emit(DxMov, { tempDst(AL, 1), comp(temp(t), 1) });
            emit(DxMov, { tempDst(lc, 1), immU(0) });
            emit(DxLoop, {});
            const uint32_t b = scratch();
            emit(DxIGe, { tempDst(b, 1), comp(temp(lc), 0), comp(temp(lc), 1) });
            emit(DxBreakC, { scalar(temp(b)) }, kTestNonzero);
            return true;
          }
          case OpBreak: emit(DxBreak, {}); return true;
          case OpBreakC: {
            const uint32_t t = scratch();
            compare(ins.controls, tempDst(t, 1), one(ins, 0), one(ins, 1));
            emit(DxBreakC, { scalar(temp(t)) }, kTestNonzero);
            return true;
          }
          case OpBreakP: {
            const Condition c = condition(s[0]);
            emit(DxBreakC, { c.op }, c.zero ? 0 : kTestNonzero);
            return true;
          }
          case OpCall:
            emit(DxCall, { label(s[0].index) });
            return true;
          case OpCallNz: {
            const Condition c = condition(s[1]);
            emit(DxCallC, { c.op, label(s[0].index) }, c.zero ? 0 : kTestNonzero);
            return true;
          }
          case OpRet:
            if (inFunction) {
              emit(DxRet, {});
              functionOpen = false;
              inFunction = false;
            } else if (ifDepth || loopDepth || !lastInMain(&ins)) {
              // A return into the epilogue from inside the program.
              fail("ret inside the main program");
              return false;
            }
            return true;
          case OpLabel:
            closeMain();
            if (functionOpen)
              emit(DxRet, {});
            emit(DxLabel, { label(s[0].index) });
            inFunction = functionOpen = true;
            return true;
          default:
            fail("unsupported instruction " + std::to_string(ins.opcode));
            return false;
        }
      }

      // Whether ret is the main program's last instruction (before a label
      // or the end).
      bool lastInMain(const Instruction* ret) const {
        const Instruction* end = L.instructions.data() + L.instructions.size();
        for (const Instruction* i = ret + 1; i < end; i++) {
          if (i->opcode == OpLabel) return true;
          if (i->opcode != OpNop && i->opcode != OpComment && i->opcode != OpDcl && i->opcode != OpDef
           && i->opcode != OpDefI && i->opcode != OpDefB)
            return false;
        }
        return true;
      }

      // ---- Prologue and epilogue ----

      uint32_t slotRegister(uint32_t slot) const { return slot + 1; }

      Op inputReg(uint32_t r, uint32_t swizzle = kSwizzleXyzw) { return reg(OtInput, r, swizzle); }

      void readInput(uint32_t r, uint32_t mask) { inputRead[r] |= mask; }

      // Registers as the HLSL initializes its statics.
      void prologue() {
        for (uint32_t i = 0; i < (std::max)(L.maxTemp, 1u); i++)
          emit(DxMov, { tempDst(rBase + i), immF(0.0f) });
        if (L.relativeInputs) {
          for (uint32_t i = 0; i < 16; i++)
            emit(DxMov, { dstOf(xtemp(kInputArray, i), 0xf), immF(0.0f) });
        } else {
          for (uint32_t i = 0; i < 16; i++)
            if ((L.usedInputs >> i) & 1) emit(DxMov, { tempDst(vMap[i]), immF(0.0f) });
        }
        emit(DxMov, { tempDst(A0), immU(0) });
        emit(DxMov, { tempDst(AL), immU(0) });
        emit(DxMov, { tempDst(P0), immU(0) });
        if (vertex) {
          emit(DxMov, { tempDst(OPOS), immF(0.0f, 0.0f, 0.0f, 1.0f) });
          emit(DxMov, { tempDst(OFOG), immF(1.0f) });
          emit(DxMov, { tempDst(OPTS), immF(0.0f) });
          emit(DxMov, { tempDst(OD[0]), immF(1.0f) });
          emit(DxMov, { tempDst(OD[1]), immF(1.0f) });
          if (L.relativeOutputs) {
            for (uint32_t i = 0; i < 12; i++)
              emit(DxMov, { dstOf(xtemp(kOutputArray, i), 0xf), immF(0.0f, 0.0f, 0.0f, 1.0f) });
          } else {
            for (uint32_t i = 0; i < 12; i++)
              if ((L.usedOutputs >> i) & 1) emit(DxMov, { tempDst(oMap[i]), immF(0.0f, 0.0f, 0.0f, 1.0f) });
          }
          loadVertexInputs();
        } else {
          for (uint32_t i = 0; i < 4; i++) if ((L.usedColors >> i) & 1) emit(DxMov, { tempDst(OC[i]), immF(0.0f) });
          for (uint32_t i = 0; i < 8; i++) if ((L.usedT >> i) & 1) emit(DxMov, { tempDst(T[i]), immF(0.0f) });
          for (uint32_t i = 0; i < 8; i++) if ((L.usedTc >> i) & 1) emit(DxMov, { tempDst(TC[i]), immF(0.0f) });
          if (L.usedTm & 0x0f) emit(DxMov, { tempDst(TM[0]), immF(0.0f) });
          if (L.usedTm & 0xf0) emit(DxMov, { tempDst(TM[1]), immF(0.0f) });
          if (info.writesDepth) {
            emit(DxMov, { tempDst(ODEPTH), comp(inputReg(0), 2) });
            readInput(0, 4);
          }
          emit(DxMov, { tempDst(VFACE), immF(1.0f) });
          loadPixelInputs();
        }
      }

      Op vRegister(uint32_t r) {
        if (L.relativeInputs)
          return xtemp(kInputArray, r);
        return temp(vMap[r & 15]);
      }

      void loadVertexInputs() {
        // Inputs as their bits, floats as the declaration's types say
        // (dxup_inputModes: 0 float, 1 int, 2 uint, 3 signed 10-bit
        // normalized; dxup_inputWOne: w = 1).
        for (size_t i = 0; i < info.inputs.size(); i++) {
          const uint32_t r = info.inputs[i].reg;
          scratchNext = scratchBase;
          const uint32_t m = scratch(), u = scratch(), f = scratch();
          const Op bits = inputReg(uint32_t(i));
          emit(DxUBfe, { tempDst(m, 1), immU(2), immU(r * 2), scalar(cbuf(2, 20), 1) });
          emit(DxIEq, { tempDst(m, 7), comp(temp(m), 0), immBits(1, 2, 3, 0) });
          emit(DxUtoF, { tempDst(u), bits });
          emit(DxMovC, { tempDst(f), comp(temp(m), 1), temp(u), bits });
          emit(DxItoF, { tempDst(u), bits });
          emit(DxMovC, { tempDst(f), comp(temp(m), 0), temp(u), temp(f) });
          emit(DxIBfe, { tempDst(u), immU(10), immU(0), bits });
          emit(DxItoF, { tempDst(u), temp(u) });
          emit(DxMul, { tempDst(u), temp(u), immF(1.0f / 511.0f) });
          emit(DxMax, { tempDst(u), temp(u), immF(-1.0f) });
          emit(DxMovC, { tempDst(f), comp(temp(m), 2), temp(u), temp(f) });
          emit(DxUBfe, { tempDst(m, 1), immU(1), immU(r), scalar(cbuf(2, 20), 2) });
          emit(DxMovC, { tempDst(f, 8), comp(temp(m), 0), immF(1.0f), temp(f) });
          emit(DxMov, { dstOf(vRegister(r), 0xf), temp(f) });
        }
      }

      void loadPixelInputs() {
        uint32_t count = 0;
        if (major >= 3) {
          for (const auto& in : info.inputs) {
            const uint32_t mask = maskOf(in.mask);
            const Op target = dstOf(vRegister(in.reg), mask);
            if (in.usage == UsagePosition && in.usageIndex == 0) {
              emit(DxMov, { target, inputReg(0) });
              readInput(0, mask);
              continue;
            }
            const int slot = varyingSlot9(in.usage, in.usageIndex);
            if (slot < 0) {
              emit(DxMov, { target, immF(0.0f) });
              continue;
            }
            emit(DxMov, { target, inputReg(slotRegister(uint32_t(slot))) });
            readInput(slotRegister(uint32_t(slot)), mask);
            count = (std::max)(count, uint32_t(slot) + 1);
          }
        } else {
          count = 1;
          for (uint32_t i = 0; i < 2; i++) {
            if (L.colorInputs & (1u << i)) {
              emit(DxMov, { dstOf(vRegister(i), 0xf), inputReg(slotRegister(kSlotColor + i)) });
              readInput(slotRegister(kSlotColor + i), 0xf);
              count = (std::max)(count, kSlotColor + i + 1);
            }
          }
          for (uint32_t i = 0; i < 8; i++) {
            if (L.texCoordInputs & (1u << i)) {
              const uint32_t r = slotRegister(kSlotTexCoord + i);
              emit(DxMov, { tempDst(TC[i]), inputReg(r) });
              if (texCoordRegisters())
                emit(DxMov, { tempDst(T[i]), inputReg(r) });
              readInput(r, 0xf);
              count = (std::max)(count, kSlotTexCoord + i + 1);
            }
          }
        }
        if (count != info.varyingCount)
          fail("pixel input layout differs");
        if (readsVPos) {
          emit(DxAdd, { tempDst(VPOS, 3), inputReg(0), immF(-0.5f) });
          emit(DxMov, { tempDst(VPOS, 0xc), immF(0.0f) });
          readInput(0, 3);
        }
        if (L.usesVFace) {
          frontFaceRegister = info.varyingCount + 1;
          emit(DxMovC, { tempDst(VFACE), scalar(inputReg(frontFaceRegister)), immF(1.0f), immF(-1.0f) });
        }
      }

      Op oRegister(uint32_t r) {
        if (L.relativeOutputs)
          return xtemp(kOutputArray, r);
        return temp(oMap[r % 12]);
      }

      void vertexEpilogue() {
        scratchNext = scratchBase;
        const uint32_t pos = scratch();
        const uint32_t count = info.varyingCount;
        std::vector<Op> slots(count);
        std::vector<uint32_t> slotTemps(count, ~0u);
        if (major >= 3) {
          bool position = false;
          int pointSizeReg = -1;
          uint32_t pointSizeComp = 0;
          for (uint32_t i = 0; i < count; i++) {
            slotTemps[i] = scratch();
            emit(DxMov, { tempDst(slotTemps[i]), immF(0.0f) });
          }
          for (const auto& o : info.outputs) {
            if (o.usage == UsagePosition && o.usageIndex == 0) {
              if (!position)
                emit(DxMov, { tempDst(pos), oRegister(o.reg) });
              position = true;
              continue;
            }
            if (o.usage == UsagePSize) {
              pointSizeReg = int(o.reg);
              pointSizeComp = o.mask & 1 ? 0 : o.mask & 2 ? 1 : o.mask & 4 ? 2 : 3;
              continue;
            }
            const int slot = varyingSlot9(o.usage, o.usageIndex);
            if (slot < 0 || uint32_t(slot) >= count)
              continue;
            emit(DxMov, { tempDst(slotTemps[slot], maskOf(o.mask)), oRegister(o.reg) });
          }
          if (!position)
            emit(DxMov, { tempDst(pos), immF(0.0f, 0.0f, 0.0f, 1.0f) });
          if (pointSizeReg >= 0 && count) {
            emit(DxMov, { tempDst(slotTemps[0], 4), comp(oRegister(uint32_t(pointSizeReg)), pointSizeComp) });
            emit(DxMov, { tempDst(slotTemps[0], 8), immF(1.0f) });
          }
          for (uint32_t i = 0; i < count; i++)
            slots[i] = temp(slotTemps[i]);
        } else {
          emit(DxMov, { tempDst(pos), temp(OPOS) });
          for (uint32_t i = 0; i < count; i++) {
            if (i == kSlotFog) {
              // Fog, the depth of table fog, the point size (w = 1 when written).
              const uint32_t f = scratch();
              emit(DxMov, { tempDst(f, 1), comp(temp(OFOG), 0) });
              emit(DxMov, { tempDst(f, 2), comp(temp(OPOS), 3) });
              if (L.writesPointSize) {
                emit(DxMov, { tempDst(f, 4), comp(temp(OPTS), 0) });
                emit(DxMov, { tempDst(f, 8), immF(1.0f) });
              } else {
                emit(DxMov, { tempDst(f, 0xc), immF(0.0f) });
              }
              slots[i] = temp(f);
            } else if (i >= kSlotColor && i < kSlotColor + 2) {
              const uint32_t c = i - kSlotColor;
              if (L.colorOutputMask & (1u << c)) {
                const uint32_t t = scratch();
                emit(DxMov, { tempDst(t), temp(OD[c]) }, kSaturate);
                slots[i] = temp(t);
              } else {
                slots[i] = immF(1.0f);
              }
            } else if (i >= kSlotTexCoord && i < kSlotTexCoord + 8 && (L.texCoordOutputs & (1u << (i - kSlotTexCoord)))) {
              slots[i] = oRegister(i - kSlotTexCoord);
            } else {
              slots[i] = immF(0.0f, 0.0f, 0.0f, 1.0f);
            }
          }
        }
        if (options.nanDepthZero) {
          // A NaN depth becomes 0 (Intel's Direct3D 9 driver draws it so).
          const uint32_t t = scratch();
          emit(DxAnd, { tempDst(t, 1), comp(temp(pos), 2), immU(0x7fffffffu) });
          emit(DxULt, { tempDst(t, 1), immU(0x7f800000u), comp(temp(t), 0) });   // NaN bits above infinity's
          emit(DxMovC, { tempDst(pos, 4), comp(temp(t), 0), immF(0.0f), comp(temp(pos), 2) });
        }
        emit(DxMov, { dstOf(reg(OtOutput, 0), 0xf), temp(pos) });
        for (uint32_t i = 0; i < count; i++)
          emit(DxMov, { dstOf(reg(OtOutput, slotRegister(i)), 0xf), slots[i] });
        if (options.clipPlanes) {
          // User clip planes in clip space, after the varyings.
          for (uint32_t plane = 0; plane < 6; plane++) {
            const Op target = dstOf(reg(OtOutput, count + 1 + plane / 4), 1u << (plane & 3));
            if ((options.clipPlanes >> plane) & 1)
              emit(DxDp4, { target, temp(pos), cbuf(3, plane) });
            else
              emit(DxMov, { target, immF(1.0f) });
          }
        }
      }

      void pixelEpilogue() {
        scratchNext = scratchBase;
        const uint32_t color = scratch();
        if (major == 1)
          emit(DxMov, { tempDst(color), temp(rBase) }, kSaturate);
        else
          emit(DxMov, { tempDst(color), temp(OC[0]) });
        if (major < 3) {
          // Fixed-function fog: the vertex's or by depth.
          readInput(slotRegister(kSlotFog), 3);
          const Op fog = inputReg(slotRegister(kSlotFog));
          const uint32_t f = scratch(), t = scratch();
          emit(DxIf, { scalar(cbuf(1, 3), 0) }, kTestNonzero);
          // x = depth * density; f = mode 0: vertex fog; 3: (end - depth) / (end - start); else exp(-x or -x^2).
          emit(DxMul, { tempDst(t, 1), comp(fog, 1), comp(cbuf(1, 2), 2) });
          emit(DxIEq, { tempDst(t, 2), scalar(cbuf(1, 3), 1), immU(2) });
          emit(DxMul, { tempDst(t, 4), comp(temp(t), 0), comp(temp(t), 0) });
          emit(DxMovC, { tempDst(t, 1), comp(temp(t), 1), comp(temp(t), 2), comp(temp(t), 0) });
          emit(DxMul, { tempDst(t, 1), comp(temp(t), 0), immF(-1.44269502f) });
          emit(DxExp, { tempDst(f, 1), comp(temp(t), 0) });
          emit(DxAdd, { tempDst(t, 1), comp(cbuf(1, 2), 1), neg(comp(fog, 1)) });
          emit(DxMul, { tempDst(t, 1), comp(temp(t), 0), comp(cbuf(1, 2), 3) });
          emit(DxIEq, { tempDst(t, 2), scalar(cbuf(1, 3), 1), immU(3) });
          emit(DxMovC, { tempDst(f, 1), comp(temp(t), 1), comp(temp(t), 0), comp(temp(f), 0) });
          emit(DxIEq, { tempDst(t, 2), scalar(cbuf(1, 3), 1), immU(0) });
          emit(DxMovC, { tempDst(f, 1), comp(temp(t), 1), comp(fog, 0), comp(temp(f), 0) });
          emit(DxMov, { tempDst(f, 1), comp(temp(f), 0) }, kSaturate);
          // lerp(fogColor, color, f) = fogColor + f * (color - fogColor).
          emit(DxAdd, { tempDst(t, 7), temp(color), neg(cbuf(1, 1)) });
          emit(DxMad, { tempDst(color, 7), comp(temp(f), 0), temp(t), cbuf(1, 1) });
          emit(DxEndIf, {});
        }
        {
          // Alpha test: the comparisons that pass (less, equal, greater, unordered bits).
          const uint32_t t = scratch();
          emit(DxIf, { scalar(cbuf(1, 3), 2) }, kTestNonzero);
          const Op a = comp(temp(color), 3), ref = comp(cbuf(1, 0), 0);
          emit(DxLt, { tempDst(t, 1), a, ref });
          emit(DxEq, { tempDst(t, 2), a, ref });
          emit(DxLt, { tempDst(t, 4), ref, a });
          emit(DxMovC, { tempDst(t, 8), comp(temp(t), 2), immU(4), immU(8) });
          emit(DxMovC, { tempDst(t, 8), comp(temp(t), 1), immU(2), comp(temp(t), 3) });
          emit(DxMovC, { tempDst(t, 8), comp(temp(t), 0), immU(1), comp(temp(t), 3) });
          emit(DxAnd, { tempDst(t, 8), comp(temp(t), 3), scalar(cbuf(1, 3), 2) });
          emit(DxDiscard, { scalar(temp(t), 3) }, 0);
          emit(DxEndIf, {});
        }
        for (uint32_t i = 0; i < 4; i++)
          if (info.colorOutputs & (1u << i))
            emit(DxMov, { dstOf(reg(OtOutput, i), 0xf), i == 0 ? temp(color) : temp(OC[i]) });
        if (info.writesDepth) {
          Op depth;
          depth.type = OtOutputDepth;
          depth.comps = 1;
          depth.dims = 0;
          emit(DxMov, { depth, comp(temp(ODEPTH), 0) });
        }
      }

      void closeMain() {
        if (mainDone)
          return;
        mainDone = true;
        if (vertex)
          vertexEpilogue();
        else
          pixelEpilogue();
        emit(DxRet, {});
      }

      bool run() {
        allocateRegisters();
        prologue();
        for (const auto& ins : L.instructions) {
          if (!instruction(ins) || !error.empty())
            return false;
        }
        if (loopDepth || ifDepth) {
          fail("unbalanced flow control");
          return false;
        }
        closeMain();
        if (functionOpen)
          emit(DxRet, {});
        return error.empty();
      }

      // ---- Declarations, signatures, container ----

      std::vector<uint32_t> declarations() {
        std::vector<uint32_t> d;
        auto add = [&](uint32_t opcode, std::initializer_list<Op> ops, std::initializer_list<uint32_t> extra, uint32_t controls = 0) {
          const size_t start = d.size();
          d.push_back(0);
          for (const Op& o : ops) o.encode(d);
          for (uint32_t e : extra) d.push_back(e);
          d[start] = opcode | controls | (uint32_t(d.size() - start) << 24);
        };
        add(DxDclGlobalFlags, {}, {}, 1u << 11);   // refactoringAllowed
        auto cbDecl = [&](uint32_t slot, uint32_t size, bool dynamic) {
          add(DxDclConstantBuffer, { cbuf(slot, size) }, {}, dynamic ? 1u << 11 : 0);
        };
        cbDecl(0, 256, usesRelativeConstants);
        if (!vertex)
          cbDecl(1, 20, false);
        cbDecl(2, 21, false);
        if (vertex && options.clipPlanes)
          cbDecl(3, 6, false);
        for (uint32_t i = 0; i < 16; i++) {
          const uint8_t type = info.samplerTypes[i];
          if (!type)
            continue;
          Op s = reg(OtSampler, i);
          s.comps = 0;
          const bool compare = !vertex && type == Sampler2D && ((options.shadowSamplers >> i) & 1);
          add(DxDclSampler, { s }, {}, compare ? 1u << 11 : 0);
          Op r = reg(OtResource, i);
          r.comps = 0;
          const uint32_t dimension = type == SamplerCube ? 6 : type == SamplerVolume ? 5 : 3;
          add(DxDclResource, { r }, { 0x5555 }, dimension << 11);
        }
        if (vertex) {
          for (size_t i = 0; i < info.inputs.size(); i++)
            add(DxDclInput, { dstOf(reg(OtInput, uint32_t(i)), 0xf) }, {});
          add(DxDclOutputSiv, { dstOf(reg(OtOutput, 0), 0xf) }, { 1 });
          for (uint32_t i = 0; i < info.varyingCount; i++)
            add(DxDclOutput, { dstOf(reg(OtOutput, slotRegister(i)), 0xf) }, {});
          if (options.clipPlanes) {
            add(DxDclOutputSiv, { dstOf(reg(OtOutput, info.varyingCount + 1), 0xf) }, { 2 });
            add(DxDclOutputSiv, { dstOf(reg(OtOutput, info.varyingCount + 2), 0x3) }, { 2 });
          }
        } else {
          if (inputRead[0])
            add(DxDclInputPsSiv, { dstOf(reg(OtInput, 0), inputRead[0]) }, { 1 }, 4u << 11);   // linear noperspective
          for (uint32_t r = 1; r <= info.varyingCount; r++)
            if (inputRead[r])
              add(DxDclInputPs, { dstOf(reg(OtInput, r), inputRead[r]) }, {}, 2u << 11);   // linear
          if (L.usesVFace)
            add(DxDclInputPsSgv, { dstOf(reg(OtInput, frontFaceRegister), 1) }, { 9 }, 1u << 11);   // constant
          for (uint32_t i = 0; i < 4; i++)
            if (info.colorOutputs & (1u << i))
              add(DxDclOutput, { dstOf(reg(OtOutput, i), 0xf) }, {});
          if (info.writesDepth) {
            Op depth;
            depth.type = OtOutputDepth;
            depth.comps = 1;
            depth.dims = 0;
            add(DxDclOutput, { depth }, {});
          }
        }
        add(DxDclTemps, {}, { maxTemps });
        if (L.relativeInputs)
          add(DxDclIndexableTemp, {}, { kInputArray, 16, 4 });
        if (L.relativeOutputs)
          add(DxDclIndexableTemp, {}, { kOutputArray, 12, 4 });
        return d;
      }

      static void split(const std::string& semantic, std::string& name, uint32_t& index) {
        size_t end = semantic.size();
        while (end > 0 && semantic[end - 1] >= '0' && semantic[end - 1] <= '9')
          end--;
        name = semantic.substr(0, end);
        index = end < semantic.size() ? uint32_t(std::stoul(semantic.substr(end))) : 0;
      }

      void signatures(d3d12::dxbc::Signature& in, d3d12::dxbc::Signature& out) {
        using d3d12::dxbc::SignatureElement;
        in.chunk = d3d12::dxbc::kISGN;
        out.chunk = d3d12::dxbc::kOSGN;
        auto element = [](const std::string& semantic, uint32_t sv, uint32_t type, uint32_t r, uint8_t mask, uint8_t rw) {
          SignatureElement e;
          split(semantic, e.name, e.semanticIndex);
          e.systemValue = sv;
          e.componentType = type;
          e.reg = r;
          e.mask = mask;
          e.rwMask = rw;
          return e;
        };
        if (vertex) {
          for (size_t i = 0; i < info.inputs.size(); i++)
            in.elements.push_back(element(semantic9(info.inputs[i].usage, info.inputs[i].usageIndex), 0, 1, uint32_t(i), 0xf, 0xf));
          out.elements.push_back(element("SV_Position0", 1, 3, 0, 0xf, 0));
          for (uint32_t i = 0; i < info.varyingCount; i++)
            out.elements.push_back(element(varyingSemantic9(i), 0, 3, slotRegister(i), 0xf, 0));
          if (options.clipPlanes) {
            out.elements.push_back(element("SV_ClipDistance0", 2, 3, info.varyingCount + 1, 0xf, 0));
            out.elements.push_back(element("SV_ClipDistance1", 2, 3, info.varyingCount + 2, 0x3, 0xc));
          }
        } else {
          in.elements.push_back(element("SV_Position0", 1, 3, 0, 0xf, uint8_t(inputRead[0])));
          for (uint32_t i = 0; i < info.varyingCount; i++)
            in.elements.push_back(element(varyingSemantic9(i), 0, 3, slotRegister(i), 0xf, uint8_t(inputRead[slotRegister(i)])));
          if (L.usesVFace)
            in.elements.push_back(element("SV_IsFrontFace0", 9, 1, frontFaceRegister, 0x1, 0x1));
          for (uint32_t i = 0; i < 4; i++)
            if (info.colorOutputs & (1u << i))
              out.elements.push_back(element("SV_Target" + std::to_string(i), 0, 3, i, 0xf, 0));
          if (info.writesDepth)
            out.elements.push_back(element("SV_Depth0", 0, 3, 0xffffffffu, 0x1, 0xe));
        }
      }

      std::vector<uint8_t> container() {
        std::vector<uint32_t> shex;
        const std::vector<uint32_t> decls = declarations();
        shex.push_back(vertex ? 0x00010050u : 0x00000050u);
        shex.push_back(0);
        shex.insert(shex.end(), decls.begin(), decls.end());
        shex.insert(shex.end(), code.begin(), code.end());
        shex[1] = uint32_t(shex.size());
        d3d12::dxbc::Signature in, out;
        signatures(in, out);
        const std::vector<uint8_t> isgn = d3d12::dxbc::writeSignature(in);
        const std::vector<uint8_t> osgn = d3d12::dxbc::writeSignature(out);
        struct Chunk { uint32_t type; const uint8_t* data; size_t size; };
        const Chunk chunks[3] = {
          { d3d12::dxbc::kISGN, isgn.data(), isgn.size() },
          { d3d12::dxbc::kOSGN, osgn.data(), osgn.size() },
          { d3d12::dxbc::fourcc('S', 'H', 'E', 'X'), reinterpret_cast<const uint8_t*>(shex.data()), shex.size() * 4 },
        };
        std::vector<uint8_t> blob(32 + 3 * 4, 0);
        d3d12::dxbc::writeU32(blob.data(), d3d12::dxbc::fourcc('D', 'X', 'B', 'C'));
        d3d12::dxbc::writeU32(blob.data() + 20, 1);
        d3d12::dxbc::writeU32(blob.data() + 28, 3);
        for (uint32_t i = 0; i < 3; i++) {
          d3d12::dxbc::writeU32(blob.data() + 32 + i * 4, uint32_t(blob.size()));
          uint8_t header[8];
          d3d12::dxbc::writeU32(header, chunks[i].type);
          d3d12::dxbc::writeU32(header + 4, uint32_t(chunks[i].size));
          blob.insert(blob.end(), header, header + 8);
          blob.insert(blob.end(), chunks[i].data, chunks[i].data + chunks[i].size);
        }
        d3d12::dxbc::writeU32(blob.data() + 24, uint32_t(blob.size()));
        uint32_t digest[4];
        d3d12::dxbc::computeChecksum(blob.data() + 20, uint32_t(blob.size() - 20), digest);
        std::memcpy(blob.data() + 4, digest, 16);
        return blob;
      }
    };

  }

  bool compileShader9(const uint32_t* tokens, size_t tokenCount, const TranslateOptions9& options,
                      std::vector<uint8_t>& dxbc, ShaderInfo9& info, std::string& error) {
    sm9::Layout9 layout;
    std::string hlsl;
    if (!sm9::translateShader9(tokens, tokenCount, options, hlsl, info, error, &layout))
      return false;
    Emitter e(layout, info, options);
    if (!e.run()) {
      error = e.error;
      return false;
    }
    dxbc = e.container();
    return true;
  }

}
