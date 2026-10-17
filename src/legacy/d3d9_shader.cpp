#include "d3d9_shader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>

namespace dxup::legacy {

  namespace {

    // ---- Tokens ----

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

    uint32_t regType(uint32_t token) {
      return ((token >> 28) & 0x7) | ((token >> 8) & 0x18);
    }

    struct Parser {
      const uint32_t* tokens;
      size_t count;
      uint32_t major = 0;
      size_t pos = 1;
      std::string error;

      void readRelative(Operand& op) {
        const uint32_t r = tokens[pos++];
        op.relative = true;
        op.relType = regType(r);
        op.relIndex = r & 0x7ff;
        op.relComponent = (r >> 16) & 3;
      }

      Operand readDst(size_t end) {
        Operand op;
        const uint32_t t = tokens[pos++];
        op.type = regType(t);
        op.index = t & 0x7ff;
        op.mask = (t >> 16) & 0xf;
        op.resultModifier = (t >> 20) & 0xf;
        const int32_t shift = int32_t((t >> 24) & 0xf);
        op.shift = shift >= 8 ? shift - 16 : shift;
        if ((t & (1u << 13)) && major >= 3 && pos < end)
          readRelative(op);
        return op;
      }

      Operand readSrc(size_t end) {
        Operand op;
        const uint32_t t = tokens[pos++];
        op.type = regType(t);
        op.index = t & 0x7ff;
        op.swizzle = (t >> 16) & 0xff;
        op.modifier = (t >> 24) & 0xf;
        if (t & (1u << 13)) {
          // Shader model 1 addresses with a0.x and no extra token.
          op.relative = true;
          if (major >= 2 && pos < end)
            readRelative(op);
        }
        return op;
      }

      bool next(Instruction& ins) {
        while (pos < count) {
          const uint32_t token = tokens[pos];
          const uint32_t opcode = token & 0xffff;
          if (opcode == OpComment) {
            pos += 1 + ((token >> 16) & 0x7fff);
            continue;
          }
          if (opcode == OpEnd)
            return false;
          ins = Instruction();
          ins.opcode = opcode;
          ins.controls = (token >> 16) & 0xff;
          ins.predicated = (token & (1u << 28)) != 0;
          pos++;
          size_t length = 0;
          if (major >= 2)
            length = (token >> 24) & 0xf;
          else if (opcode == OpDef)
            length = 5;
          else
            while (pos + length < count && (tokens[pos + length] & 0x80000000u))
              length++;
          const size_t end = pos + length;
          if (end > count) {
            error = "truncated instruction";
            return false;
          }
          decode(ins, end);
          pos = end;
          return true;
        }
        return false;
      }

      void decode(Instruction& ins, size_t end) {
        switch (ins.opcode) {
          case OpDcl:
            ins.declToken = tokens[pos++];
            if (pos < end) ins.dst.push_back(readDst(end));
            return;
          case OpDef: case OpDefI: case OpDefB:
            ins.dst.push_back(readDst(end));
            for (int i = 0; i < (ins.opcode == OpDefB ? 1 : 4) && pos < end; i++)
              ins.literal[i] = tokens[pos++];
            return;
          case OpCall: case OpLabel: case OpRep: case OpIf: case OpLoop: case OpCallNz: case OpIfC: case OpBreakC:
          case OpBreakP: case OpNop: case OpRet: case OpEndLoop: case OpEndRep: case OpElse: case OpEndIf: case OpBreak:
          case OpPhase:
            while (pos < end)
              ins.src.push_back(readSrc(end));
            return;
          default:
            break;
        }
        if (pos < end)
          ins.dst.push_back(readDst(end));
        if (ins.predicated && pos < end)
          ins.predicate = readSrc(end);
        while (pos < end)
          ins.src.push_back(readSrc(end));
      }
    };

    // ---- Varying slots ----

    struct SlotUsage { uint8_t usage, index; };

    const SlotUsage kSlots[kVaryingSlots9] = {
      { UsageFog, 0 }, { UsageColor, 0 }, { UsageColor, 1 },
      { UsageTexCoord, 0 }, { UsageTexCoord, 1 }, { UsageTexCoord, 2 }, { UsageTexCoord, 3 },
      { UsageTexCoord, 4 }, { UsageTexCoord, 5 }, { UsageTexCoord, 6 }, { UsageTexCoord, 7 },
      { UsageTexCoord, 8 }, { UsageTexCoord, 9 }, { UsageTexCoord, 10 }, { UsageTexCoord, 11 },
      { UsageTexCoord, 12 }, { UsageTexCoord, 13 }, { UsageTexCoord, 14 }, { UsageTexCoord, 15 },
      { UsageNormal, 0 }, { UsageNormal, 1 }, { UsageTangent, 0 }, { UsageTangent, 1 },
      { UsageBinormal, 0 }, { UsageBinormal, 1 }, { UsagePosition, 1 }, { UsagePSize, 0 },
      { UsageBlendWeight, 0 }, { UsageBlendIndices, 0 }, { UsageDepth, 0 },
    };

    constexpr int kSlotFog = 0, kSlotColor = 1, kSlotTexCoord = 3;

    // ---- HLSL writing ----

    const char* kComponents = "xyzw";

    std::string swizzleString(uint32_t swizzle) {
      std::string s = ".";
      for (int i = 0; i < 4; i++)
        s += kComponents[(swizzle >> (2 * i)) & 3];
      return s;
    }

    std::string maskString(uint32_t mask) {
      std::string s = ".";
      for (int i = 0; i < 4; i++)
        if (mask & (1u << i)) s += kComponents[i];
      return s.size() > 1 ? s : ".xyzw";
    }

    std::string floatLiteral(uint32_t bits) {
      if ((bits & 0x7f800000u) == 0x7f800000u)
        return (bits & 0x007fffffu) ? "0.0f" : (bits >> 31) ? "-3.402823466e38f" : "3.402823466e38f";
      float f;
      std::memcpy(&f, &bits, 4);
      char text[64];
      snprintf(text, sizeof(text), "%.9g", f);
      std::string s = text;
      if (s.find_first_of(".eE") == std::string::npos) s += ".0";
      return s + "f";
    }

    std::string str(uint32_t value) { return std::to_string(value); }

    struct Writer {
      ShaderInfo9& info;
      const TranslateOptions9& options;
      bool vertex;
      uint32_t major, minor;
      std::string body;
      std::deque<std::string> functions;   // subroutines by label
      std::string* out = &body;
      int indent = 1;
      std::map<uint32_t, std::string> floatDefs, intDefs, boolDefs;
      uint32_t maxTemp = 0, maxConst = 0;
      uint32_t colorInputs = 0;       // pixel shaders before 3.0: v0/v1 read
      uint32_t texCoordInputs = 0;    // pixel shaders before 3.0: texture coordinates read
      uint32_t colorOutputMask = 0;   // vertex shaders before 3.0: oD written
      uint32_t texCoordOutputs = 0;   // vertex shaders before 3.0: oT written
      bool writesFog = false;
      bool writesPointSize = false;
      bool usesVFace = false;
      // Registers are separate variables (the compiler handles those much
      // faster than arrays); inputs and outputs indexed by aL stay arrays.
      bool relativeInputs = false, relativeOutputs = false;
      uint32_t usedInputs = 0, usedOutputs = 0, usedT = 0, usedTc = 0, usedTm = 0, usedColors = 0;
      uint32_t loopDepth = 0;
      std::string error;

      // Pixel shader model 1.x samples into texture registers t#.
      bool ps1() const { return !vertex && major == 1; }
      // Pixel shaders whose t# registers are texture coordinates.
      bool texCoordRegisters() const { return !vertex && (major == 2 || (major == 1 && minor == 4)); }

      void line(const std::string& text) {
        out->append(size_t(indent) * 2, ' ');
        *out += text;
        *out += '\n';
      }

      std::string tc(uint32_t reg) {
        texCoordInputs |= 1u << (reg & 7);
        usedTc |= 1u << (reg & 7);
        return "tc" + str(reg & 7);
      }
      std::string temp(uint32_t index) {
        maxTemp = (std::max)(maxTemp, index + 1);
        return "r" + str(index);
      }
      std::string input(uint32_t index) {
        usedInputs |= 1u << (index & 15);
        return relativeInputs ? "v[" + str(index) + "]" : "v" + str(index);
      }
      std::string output(uint32_t index) {
        usedOutputs |= 1u << (index & 15);
        return relativeOutputs ? "o[" + str(index) + "]" : "o" + str(index);
      }
      std::string texture(uint32_t index) {
        usedT |= 1u << (index & 7);
        return "t" + str(index & 7);
      }
      std::string matrixRow(uint32_t index) {
        usedTm |= 1u << (index & 7);
        return "tm" + str(index & 7);
      }
      std::string color(uint32_t index) {
        usedColors |= 1u << (index & 3);
        return "oC" + str(index & 3);
      }

      std::string relativeOffset(const Operand& op) {
        if (op.relType == RegLoop)
          return "aL + ";
        return std::string("a0.") + kComponents[op.relComponent] + " + ";
      }

      std::string registerName(const Operand& op, bool forWrite) {
        switch (op.type) {
          case RegTemp: case RegTempFloat16:
            return temp(op.index);
          case RegInput:
            if (!vertex && major < 3)
              colorInputs |= 1u << (op.index & 1);
            if (op.relative)
              return "v[" + relativeOffset(op) + str(op.index) + "]";
            return input(op.index);
          case RegConst: case RegConst2: case RegConst3: case RegConst4: {
            const uint32_t index = op.index + (op.type == RegConst2 ? 2048 : op.type == RegConst3 ? 4096
              : op.type == RegConst4 ? 6144 : 0);
            if (op.relative) {
              info.relativeConstants = true;
              // The last index from the buffer: a bound the compiler can
              // prove makes it much slower; negative indices wrap to it.
              return "c[min(uint(" + relativeOffset(op) + str(index) + "), dxup_lastConstant)]";
            }
            auto def = floatDefs.find(index);
            if (def != floatDefs.end())
              return def->second;
            maxConst = (std::max)(maxConst, index + 1);
            const std::string name = "c[" + str(index) + "]";
            // Pixel shader 1.x constants are clamped to [-1, 1].
            if (ps1() && !forWrite)
              return "clamp(" + name + ", -1.0f, 1.0f)";
            return name;
          }
          case RegAddr:
            if (vertex)
              return "float4(a0)";
            if (texCoordRegisters() && !forWrite)
              texCoordInputs |= 1u << (op.index & 7);
            return texture(op.index);
          case RegRastOut:
            if (op.index == 1) writesFog = true;
            if (op.index == 2) writesPointSize = true;
            return op.index == 0 ? "oPos" : op.index == 1 ? "oFog" : "oPts";
          case RegAttrOut:
            colorOutputMask |= 1u << (op.index & 1);
            return "oD" + str(op.index & 1);
          case RegOutput:
            if (vertex && major < 3)
              texCoordOutputs |= 1u << (op.index & 7);
            if (op.relative)
              return "o[" + relativeOffset(op) + str(op.index) + "]";
            return output(op.index);
          case RegConstInt: {
            auto def = intDefs.find(op.index);
            if (def != intDefs.end())
              return "float4(" + def->second + ")";
            return "float4(ci[" + str(op.index) + "])";
          }
          case RegColorOut:
            info.colorOutputs |= 1u << (op.index & 3);
            return color(op.index);
          case RegDepthOut:
            info.writesDepth = true;
            return "oDepth4";
          case RegConstBool: {
            auto def = boolDefs.find(op.index);
            if (def != boolDefs.end())
              return def->second;
            return "(cb[" + str(op.index / 4) + "][" + str(op.index % 4) + "] != 0u)";
          }
          case RegLoop:
            return "float4(aL, aL, aL, aL)";
          case RegMisc:
            if (op.index == 0)
              return "vPos";
            usesVFace = true;
            return "vFace";
          case RegPredicate:
            return "float4(p0)";
          default:
            error = "unsupported register type " + str(op.type);
            return "float4(0, 0, 0, 0)";
        }
      }

      // A source operand as a float4 expression.
      std::string source(const Operand& op) {
        std::string value = registerName(op, false);
        if (op.type == RegConstBool)
          return "(" + value + " ? 1.0f.xxxx : 0.0f.xxxx)";
        if (op.swizzle != 0xe4)
          value = "(" + value + ")" + swizzleString(op.swizzle);
        switch (op.modifier) {
          case 0:  return value;
          case 1:  return "(-" + value + ")";
          case 2:  return "(" + value + " - 0.5f)";
          case 3:  return "(-(" + value + " - 0.5f))";
          case 4:  return "(2.0f * " + value + " - 1.0f)";
          case 5:  return "(-(2.0f * " + value + " - 1.0f))";
          case 6:  return "(1.0f - " + value + ")";
          case 7:  return "(2.0f * " + value + ")";
          case 8:  return "(-2.0f * " + value + ")";
          case 9:  return "(" + value + " / (" + value + ").zzzz)";
          case 10: return "(" + value + " / (" + value + ").wwww)";
          case 11: return "abs(" + value + ")";
          case 12: return "(-abs(" + value + "))";
          default: return value;
        }
      }

      std::string scalar(const Operand& op) {
        return "(" + source(op) + ").x";
      }

      // A boolean condition: a predicate component or a nonzero value.
      std::string condition(const Operand& op) {
        if (op.type == RegPredicate)
          return std::string(op.modifier == kModifierNot ? "!" : "") + "p0." + kComponents[op.swizzle & 3];
        return "(" + scalar(op) + " != 0.0f)";
      }

      Operand offset(const Operand& op, uint32_t add) {
        Operand o = op;
        o.index += add;
        return o;
      }

      // dst = value (float4 expression), with modifiers, mask and predicate.
      void write(const Instruction& ins, const std::string& value) {
        if (ins.dst.empty())
          return;
        const Operand& d = ins.dst[0];
        std::string v = "(" + value + ")";
        if (d.shift > 0)
          v = "(" + v + " * " + str(1u << d.shift) + ".0f)";
        else if (d.shift < 0)
          v = "(" + v + " / " + str(1u << (-d.shift)) + ".0f)";
        if (d.resultModifier & 1)
          v = "saturate(" + v + ")";
        const std::string mask = maskString(d.mask);
        if (d.type == RegAddr && vertex) {
          // a0 holds integers: vs_1_1 truncates down, later models round.
          const std::string rounded = major >= 2 ? "round(" + v + ")" : "floor(" + v + ")";
          line("a0" + mask + " = int4(" + rounded + ")" + mask + ";");
          return;
        }
        if (d.type == RegPredicate) {
          line("p0" + mask + " = ((" + v + ")" + mask + " != 0.0f);");
          return;
        }
        const std::string target = registerName(d, true);
        if (ins.predicated) {
          const std::string predicate = "p0" + swizzleString(ins.predicate.swizzle);
          v = "(" + std::string(ins.predicate.modifier == kModifierNot ? "!" : "") + predicate + " ? " + v + " : " + target + ")";
        }
        line(target + mask + " = " + v + mask + ";");
      }

      std::string compare(uint32_t controls, const std::string& a, const std::string& b) {
        static const char* ops[] = { "!=", ">", "==", ">=", "<", "!=", "<=", "!=" };
        return "(" + a + " " + ops[controls & 7] + " " + b + ")";
      }

      std::string sampleFor(uint32_t sampler, const std::string& coord, const char* mode, const std::string& extra) {
        sampler &= 15;
        uint8_t type = info.samplerTypes[sampler];
        if (!type && options.boundSamplerTypes)
          type = options.boundSamplerTypes[sampler];
        if (!type)
          type = Sampler2D;
        info.samplerTypes[sampler] = type;
        const std::string tex = (vertex ? "vt" : "t") + str(sampler) + "_";
        const std::string smp = (vertex ? "vs" : "s") + str(sampler);
        if (!vertex && type == Sampler2D && ((options.shadowSamplers >> sampler) & 1)) {
          // Depth compared with z: 1 where the reference is not farther.
          const std::string p = "(" + coord + ")";
          return "(" + tex + ".SampleCmpLevelZero(" + smp + ", " + p + ".xy, saturate(" + p + ".z))).xxxx";
        }
        const std::string c = "(" + coord + ")" + (type == Sampler2D ? ".xy" : ".xyz");
        if (std::strcmp(mode, "level") == 0 || vertex)
          return tex + ".SampleLevel(" + smp + ", " + c + ", " + (extra.empty() ? "0.0f" : extra) + ")";
        if (std::strcmp(mode, "bias") == 0)
          return tex + ".SampleBias(" + smp + ", " + c + ", clamp(" + extra + ", -15.99f, 15.99f))";
        if (std::strcmp(mode, "grad") == 0)
          return tex + ".SampleGrad(" + smp + ", " + c + ", " + extra + ")";
        return tex + ".Sample(" + smp + ", " + c + ")";
      }

      void open(const std::string& text) {
        line(text + " {");
        indent++;
      }

      void close() {
        indent--;
        line("}");
      }

      bool emit(const Instruction& ins) {
        const auto& s = ins.src;
        auto src = [&](size_t i) { return i < s.size() ? source(s[i]) : std::string("float4(0, 0, 0, 0)"); };
        // Products as Direct3D 9 forms them (TranslateOptions9::floatEmulation
        // and dotEmulation).
        const bool legacy = options.dotEmulation;
        auto mul = [&](const std::string& a, const std::string& b) {
          return options.floatEmulation ? "dxup_mul(" + a + ", " + b + ")" : a + " * " + b;
        };
        auto dot4 = [&](const std::string& a, const std::string& b) {
          return legacy ? "dxup_dot4(" + a + ", " + b + ")" : "dot(" + a + ", " + b + ")";
        };
        auto dot3 = [&](const std::string& a, const std::string& b) {
          return legacy ? "dxup_dot4(float4((" + a + ").xyz, 0.0f), " + b + ")" : "dot((" + a + ").xyz, (" + b + ").xyz)";
        };
        auto dot2 = [&](const std::string& a, const std::string& b) {
          return legacy ? "dxup_dot4(float4((" + a + ").xy, 0.0f, 0.0f), " + b + ")" : "dot((" + a + ").xy, (" + b + ").xy)";
        };
        auto one = [&](size_t i) { return i < s.size() ? scalar(s[i]) : std::string("0.0f"); };
        const uint32_t reg = ins.dst.empty() ? 0 : ins.dst[0].index;
        switch (ins.opcode) {
          case OpNop: case OpPhase: case OpDcl: case OpDef: case OpDefI: case OpDefB:
            return true;
          case OpMov: case OpMova: write(ins, src(0)); return true;
          case OpAdd:  write(ins, src(0) + " + " + src(1)); return true;
          case OpSub:  write(ins, src(0) + " - " + src(1)); return true;
          case OpMul:  write(ins, mul(src(0), src(1))); return true;
          case OpMad:  write(ins, mul(src(0), src(1)) + " + " + src(2)); return true;
          case OpMin:  write(ins, "min(" + src(0) + ", " + src(1) + ")"); return true;
          case OpMax:  write(ins, "max(" + src(0) + ", " + src(1) + ")"); return true;
          case OpSlt:  write(ins, "float4(" + src(0) + " < " + src(1) + ")"); return true;
          case OpSge:  write(ins, "float4(" + src(0) + " >= " + src(1) + ")"); return true;
          case OpRcp:  write(ins, "dxup_rcp(" + one(0) + ").xxxx"); return true;
          case OpRsq:  write(ins, "dxup_rcp(sqrt(abs(" + one(0) + "))).xxxx"); return true;
          case OpExpP:
            // vs_1_1: 2^floor(x), the fraction, 2^x, 1.
            if (vertex && major == 1) {
              write(ins, "dxup_expp(" + one(0) + ")");
              return true;
            }
            write(ins, "exp2(" + one(0) + ").xxxx");
            return true;
          case OpLogP:
            // vs_1_1: exponent, mantissa, log2 |x|, 1.
            if (vertex && major == 1) {
              write(ins, "dxup_logp(" + one(0) + ")");
              return true;
            }
            write(ins, "dxup_log(" + one(0) + ").xxxx");
            return true;
          case OpExp: write(ins, "exp2(" + one(0) + ").xxxx"); return true;
          case OpLog: write(ins, "dxup_log(" + one(0) + ").xxxx"); return true;
          case OpPow:  write(ins, "dxup_pow(" + one(0) + ", " + one(1) + ").xxxx"); return true;
          case OpDp3:  write(ins, dot3(src(0), src(1)) + ".xxxx"); return true;
          case OpDp4:  write(ins, dot4(src(0), src(1)) + ".xxxx"); return true;
          case OpDp2Add: write(ins, "(" + dot2(src(0), src(1)) + " + " + one(2) + ").xxxx"); return true;
          case OpFrc:  write(ins, "frac(" + src(0) + ")"); return true;
          case OpAbs:  write(ins, "abs(" + src(0) + ")"); return true;
          case OpSgn:  write(ins, "sign(" + src(0) + ")"); return true;
          case OpNrm:  write(ins, "dxup_nrm(" + src(0) + ")"); return true;
          case OpCrs:  write(ins, "float4(cross((" + src(0) + ").xyz, (" + src(1) + ").xyz), 0.0f)"); return true;
          case OpLrp:  write(ins, "lerp(" + src(2) + ", " + src(1) + ", " + src(0) + ")"); return true;
          case OpCmp:  write(ins, "dxup_cmp(" + src(0) + ", " + src(1) + ", " + src(2) + ")"); return true;
          case OpCnd:
            // ps_1_1 to ps_1_3 compare r0.a only.
            if (ps1() && minor < 4)
              write(ins, "((" + src(0) + ").w > 0.5f ? " + src(1) + " : " + src(2) + ")");
            else
              write(ins, "dxup_cnd(" + src(0) + ", " + src(1) + ", " + src(2) + ")");
            return true;
          case OpLit:  write(ins, "dxup_lit(" + src(0) + ")"); return true;
          case OpDst:
            write(ins, "float4(1.0f, (" + src(0) + ").y * (" + src(1) + ").y, (" + src(0) + ").z, (" + src(1) + ").w)");
            return true;
          case OpSinCos: {
            const std::string a = one(0);
            write(ins, "float4(cos(" + a + "), sin(" + a + "), 0.0f, 0.0f)");
            return true;
          }
          case OpM4x4: case OpM4x3: case OpM3x4: case OpM3x3: case OpM3x2: {
            const bool four = ins.opcode == OpM4x4 || ins.opcode == OpM4x3;
            const uint32_t rows = ins.opcode == OpM4x4 || ins.opcode == OpM3x4 ? 4 : ins.opcode == OpM3x2 ? 2 : 3;
            std::string parts[4] = { "0.0f", "0.0f", "0.0f", "0.0f" };
            const std::string v = src(0);
            for (uint32_t i = 0; i < rows && s.size() > 1; i++)
              parts[i] = four ? dot4(v, source(offset(s[1], i))) : dot3(v, source(offset(s[1], i)));
            write(ins, "float4(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ", " + parts[3] + ")");
            return true;
          }
          case OpDsx:  write(ins, "ddx(" + src(0) + ")"); return true;
          case OpDsy:  write(ins, "ddy(" + src(0) + ")"); return true;
          case OpSetP: write(ins, "float4(" + compare(ins.controls, src(0), src(1)) + ")"); return true;

          // ---- Textures ----
          case OpTex:
            if (ps1() && minor < 4) {
              // tex t#: sample with texture coordinate #.
              write(ins, sampleFor(reg, tc(reg), "", ""));
            } else if (ps1()) {
              // texld r#, t#/r#: the sampler is the destination's number.
              write(ins, sampleFor(reg, src(0), "", ""));
            } else {
              const uint32_t sampler = s.size() > 1 ? s[1].index : 0;
              const std::string coord = src(0);
              if (ins.controls == 1)
                write(ins, sampleFor(sampler, "(" + coord + ") / (" + coord + ").wwww", "", ""));
              else if (ins.controls == 2)
                write(ins, sampleFor(sampler, coord, "bias", "(" + coord + ").w"));
              else
                write(ins, sampleFor(sampler, coord, "", ""));
            }
            return true;
          case OpTexLdl:
            write(ins, sampleFor(s.size() > 1 ? s[1].index : 0, src(0), "level", "(" + src(0) + ").w"));
            return true;
          case OpTexLdd:
          {
            const uint32_t sampler = s.size() > 1 ? s[1].index & 15 : 0;
            const bool flat = (info.samplerTypes[sampler] ? info.samplerTypes[sampler]
              : options.boundSamplerTypes && options.boundSamplerTypes[sampler] ? options.boundSamplerTypes[sampler] : Sampler2D) == Sampler2D;
            const char* comps = flat ? ").xy" : ").xyz";
            write(ins, sampleFor(sampler, src(0), "grad", "(" + src(2) + comps + ", (" + src(3) + comps));
          }
            return true;
          case OpTexCoord:
            if (ps1() && minor < 4)
              write(ins, "float4(saturate(" + tc(reg) + ".xyz), 1.0f)");
            else
              write(ins, "float4((" + src(0) + ").xyz, 1.0f)");
            return true;
          case OpTexKill: {
            // ps_1_1 to ps_1_3 test the texture coordinates of t#.
            const std::string value = ps1() && minor < 4 && ins.dst[0].type == RegAddr ? tc(reg)
              : registerName(ins.dst[0], false);
            line("clip((" + value + ")" + (major >= 2 ? maskString(ins.dst[0].mask) : std::string(".xyz")) + ");");
            return true;
          }
          case OpTexBem: case OpTexBemL: {
            const std::string t = src(0);
            const std::string coord = "float4(" + tc(reg) + ".xy + float2(dot(bumpMatrix[" + str(reg) + "].xz, (" + t
              + ").xy), dot(bumpMatrix[" + str(reg) + "].yw, (" + t + ").xy)), 0.0f, 0.0f)";
            std::string sample = sampleFor(reg, coord, "", "");
            if (ins.opcode == OpTexBemL)
              sample = "dxup_luminance(" + sample + ", (" + t + ").z, bumpLuminance[" + str(reg) + "].xy)";
            write(ins, sample);
            return true;
          }
          case OpTexReg2Ar: write(ins, sampleFor(reg, "(" + src(0) + ").wxww", "", "")); return true;
          case OpTexReg2Gb: write(ins, sampleFor(reg, "(" + src(0) + ").yzzz", "", "")); return true;
          case OpTexReg2Rgb: write(ins, sampleFor(reg, "(" + src(0) + ").xyzz", "", "")); return true;
          case OpTexM3x2Pad: case OpTexM3x3Pad:
            line(matrixRow(reg) + " = dot(" + tc(reg) + ".xyz, (" + src(0) + ").xyz);");
            return true;
          case OpTexM3x2Tex: case OpTexM3x2Depth: {
            const std::string u = matrixRow(reg - 1);
            const std::string v = "dot(" + tc(reg) + ".xyz, (" + src(0) + ").xyz)";
            if (ins.opcode == OpTexM3x2Depth) {
              line("oDepth4 = (" + v + " != 0.0f ? " + u + " / " + v + " : 1.0f).xxxx;");
              info.writesDepth = true;
            } else {
              write(ins, sampleFor(reg, "float4(" + u + ", " + v + ", 0.0f, 0.0f)", "", ""));
            }
            return true;
          }
          case OpTexM3x3Tex: case OpTexM3x3: case OpTexM3x3Spec: case OpTexM3x3VSpec: {
            const std::string n = "float3(" + matrixRow(reg - 2) + ", " + matrixRow(reg - 1) + ", dot(" + tc(reg)
              + ".xyz, (" + src(0) + ").xyz))";
            if (ins.opcode == OpTexM3x3) {
              write(ins, "float4(" + n + ", 1.0f)");
            } else if (ins.opcode == OpTexM3x3Tex) {
              write(ins, sampleFor(reg, "float4(" + n + ", 0.0f)", "", ""));
            } else {
              const std::string eye = ins.opcode == OpTexM3x3Spec ? "(" + src(1) + ").xyz"
                : "float3(" + tc(reg - 2) + ".w, " + tc(reg - 1) + ".w, " + tc(reg) + ".w)";
              write(ins, sampleFor(reg, "float4(dxup_reflect(" + n + ", " + eye + "), 0.0f)", "", ""));
            }
            return true;
          }
          case OpTexDp3Tex:
            write(ins, sampleFor(reg, "float4(dot(" + tc(reg) + ".xyz, (" + src(0) + ").xyz), 0.0f, 0.0f, 0.0f)", "", ""));
            return true;
          case OpTexDp3:
            write(ins, "dot(" + tc(reg) + ".xyz, (" + src(0) + ").xyz).xxxx");
            return true;
          case OpTexDepth: {
            const std::string r5 = registerName(ins.dst[0], false);
            line("oDepth4 = (" + r5 + ".y != 0.0f ? " + r5 + ".x / " + r5 + ".y : 1.0f).xxxx;");
            info.writesDepth = true;
            return true;
          }
          case OpBem:
            write(ins, "float4((" + src(0) + ").xy + float2(dot(bumpMatrix[" + str(reg) + "].xz, (" + src(1)
              + ").xy), dot(bumpMatrix[" + str(reg) + "].yw, (" + src(1) + ").xy)), 0.0f, 0.0f)");
            return true;

          // ---- Flow control ----
          case OpIf:
            open("if (" + condition(s[0]) + ")");
            return true;
          case OpIfC:
            open("if " + compare(ins.controls, one(0), one(1)));
            return true;
          case OpElse:
            indent--;
            line("} else {");
            indent++;
            return true;
          case OpEndIf:
            close();
            return true;
          case OpRep: {
            const std::string counter = "dxup_rep" + str(loopDepth++);
            open("[loop] for (int " + counter + " = 0; " + counter + " < int(" + one(0) + "); " + counter + "++)");
            return true;
          }
          case OpEndRep:
            loopDepth--;
            close();
            return true;
          case OpLoop: {
            // loop aL, i#: count, start, step; aL restored after the loop.
            const std::string d = str(loopDepth++);
            open("");
            line("int4 dxup_li" + d + " = int4(" + src(1) + ");");
            line("int dxup_saved" + d + " = aL;");
            line("aL = dxup_li" + d + ".y;");
            open("[loop] for (int dxup_lc" + d + " = 0; dxup_lc" + d + " < dxup_li" + d + ".x; dxup_lc" + d + "++, aL += dxup_li" + d + ".z)");
            return true;
          }
          case OpEndLoop: {
            const std::string d = str(--loopDepth);
            close();
            line("aL = dxup_saved" + d + ";");
            close();
            return true;
          }
          case OpBreak:
            line("break;");
            return true;
          case OpBreakC:
            line("if " + compare(ins.controls, one(0), one(1)) + " break;");
            return true;
          case OpBreakP:
            line("if (" + condition(s[0]) + ") break;");
            return true;
          case OpCall:
            line("L" + str(s[0].index) + "();");
            return true;
          case OpCallNz:
            line("if (" + condition(s[1]) + ") L" + str(s[0].index) + "();");
            return true;
          case OpRet:
            if (out != &body) {
              close();
              out = &body;
              indent = 1;
            } else {
              line("return;");
            }
            return true;
          case OpLabel:
            functions.emplace_back();
            out = &functions.back();
            indent = 0;
            open("void L" + str(s[0].index) + "()");
            return true;
          default:
            error = "unsupported instruction " + str(ins.opcode);
            return false;
        }
      }
    };

    const char* usageSemantic(uint8_t usage) {
      static const char* names[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
        "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };
      return usage < 14 ? names[usage] : "TEXCOORD";
    }

    std::string slotSemantic(uint32_t slot) {
      return semantic9(kSlots[slot].usage, kSlots[slot].index);
    }

    std::string componentsOf(uint8_t mask) {
      std::string comps;
      for (int c = 0; c < 4; c++)
        if (mask & (1 << c)) comps += kComponents[c];
      return comps.empty() ? "xyzw" : comps;
    }

  }

  int varyingSlot9(uint8_t usage, uint8_t usageIndex) {
    for (uint32_t i = 0; i < kVaryingSlots9; i++)
      if (kSlots[i].usage == usage && kSlots[i].index == usageIndex)
        return int(i);
    return -1;
  }

  const char* semanticName9(uint8_t usage) {
    return usageSemantic(usage);
  }

  std::string varyingSemantic9(uint32_t slot) {
    return slot < kVaryingSlots9 ? slotSemantic(slot) : std::string("TEXCOORD15");
  }

  std::string semantic9(uint8_t usage, uint8_t usageIndex) {
    return std::string(usageSemantic(usage)) + std::to_string(usageIndex);
  }

  bool translateShader9(const uint32_t* tokens, size_t tokenCount, const TranslateOptions9& options,
                        std::string& hlsl, ShaderInfo9& info, std::string& error) {
    if (!tokens || tokenCount < 2) {
      error = "empty shader";
      return false;
    }
    const uint32_t version = tokens[0];
    info = ShaderInfo9();
    const uint32_t kind = version >> 16;
    if (kind != 0xfffe && kind != 0xffff) {
      error = "not a shader";
      return false;
    }
    const bool vertex = kind == 0xfffe;
    info.stage = vertex ? ShaderStage9::Vertex : ShaderStage9::Pixel;
    info.major = (version >> 8) & 0xff;
    info.minor = version & 0xff;
    if (info.major < 1 || info.major > 3) {
      error = "unsupported shader model";
      return false;
    }

    Parser parser{ tokens, tokenCount, info.major };
    std::vector<Instruction> instructions;
    Instruction ins;
    while (parser.next(ins))
      instructions.push_back(ins);
    if (!parser.error.empty()) {
      error = parser.error;
      return false;
    }

    Writer w{ info, options, vertex, info.major, info.minor };
    for (const auto& i : instructions) {
      for (const auto* ops : { &i.dst, &i.src })
        for (const Operand& op : *ops) {
          if (op.relative && op.type == RegInput) w.relativeInputs = true;
          if (op.relative && op.type == RegOutput) w.relativeOutputs = true;
        }
    }
    for (const auto& i : instructions) {
      if (i.dst.empty())
        continue;
      const Operand& d = i.dst[0];
      if (i.opcode == OpDef) {
        const uint32_t index = d.index + (d.type == RegConst2 ? 2048 : d.type == RegConst3 ? 4096 : d.type == RegConst4 ? 6144 : 0);
        w.floatDefs[index] = "float4(" + floatLiteral(i.literal[0]) + ", " + floatLiteral(i.literal[1]) + ", "
          + floatLiteral(i.literal[2]) + ", " + floatLiteral(i.literal[3]) + ")";
      } else if (i.opcode == OpDefI) {
        char text[96];
        snprintf(text, sizeof(text), "int4(%d, %d, %d, %d)", int32_t(i.literal[0]), int32_t(i.literal[1]),
          int32_t(i.literal[2]), int32_t(i.literal[3]));
        w.intDefs[d.index] = text;
      } else if (i.opcode == OpDefB) {
        w.boolDefs[d.index] = i.literal[0] ? "true" : "false";
      } else if (i.opcode == OpDcl) {
        const uint8_t usage = uint8_t(i.declToken & 0x1f);
        const uint8_t usageIndex = uint8_t((i.declToken >> 16) & 0xf);
        if (d.type == RegSampler) {
          const uint32_t type = (i.declToken >> 27) & 0xf;
          info.samplerTypes[d.index & 15] = uint8_t(type ? type : Sampler2D);
        } else if (d.type == RegInput && (vertex || info.major >= 3)) {
          info.inputs.push_back({ d.index, usage, usageIndex, uint8_t(d.mask) });
        } else if (d.type == RegOutput && vertex && info.major >= 3) {
          info.outputs.push_back({ d.index, usage, usageIndex, uint8_t(d.mask) });
        }
      }
    }

    for (const auto& i : instructions) {
      if (!w.emit(i) || !w.error.empty()) {
        error = w.error;
        return false;
      }
    }
    if (w.indent != 1 || w.out != &w.body) {
      error = "unbalanced flow control";
      return false;
    }

    // ---- Declarations ----
    std::string s;
    s += "// Translated by DXUP from ";
    s += vertex ? "vs_" : "ps_";
    s += std::to_string(info.major) + "_" + std::to_string(info.minor) + "\n";
    // Floats bound as a window of what the shader reads; integers and
    // booleans apart (they change rarely).
    s += "cbuffer Constants : register(b0) {\n  float4 c[256];\n};\n";
    s += "cbuffer IntConstants : register(b2) {\n  int4 ci[16];\n  uint4 cb[4];\n  uint dxup_lastConstant;   // 255\n};\n";
    if (vertex && options.clipPlanes)
      s += "cbuffer ClipPlanes : register(b3) {\n  float4 dxup_clipPlanes[6];\n};\n";
    if (!vertex) {
      s += "cbuffer Fixed : register(b1) {\n"
           "  float4 dxup_alphaTest;   // reference, function, enabled\n"
           "  float4 dxup_fogColor;\n"
           "  float4 dxup_fogParams;   // start, end, density, table mode (0 vertex fog, 1 exp, 2 exp2, 3 linear)\n"
           "  uint4  dxup_flags;       // fog enabled\n"
           "  float4 bumpMatrix[8];\n"
           "  float4 bumpLuminance[8];\n"
           "};\n";
    }
    // Direct3D 9's arithmetic.
    s += "float dxup_rcp(float x) { return x == 0.0f ? 3.402823466e38f : 1.0f / x; }\n";
    s += "float dxup_log(float x) { return x == 0.0f ? -3.402823466e38f : log2(abs(x)); }\n";
    s += "float dxup_pow(float x, float y) { return y == 0.0f ? 1.0f : exp2(y * dxup_log(x)); }\n";
    s += "float4 dxup_nrm(float4 x) { float l = dot(x.xyz, x.xyz); return l > 0.0f ? x * rsqrt(l) : 0.0f.xxxx; }\n";
    if (options.floatEmulation || options.dotEmulation) {
      // 0 times anything is 0 (Direct3D 9), not NaN for infinity or NaN.
      s += "float4 dxup_mul(float4 a, float4 b) { return a == 0.0f || b == 0.0f ? 0.0f.xxxx : a * b; }\n";
      s += "float dxup_dot4(float4 a, float4 b) { float4 m = dxup_mul(a, b); return (m.x + m.y) + (m.z + m.w); }\n";
    }
    // Per-component selects: faster to compile than four scalar ones.
    s += "float4 dxup_cmp(float4 a, float4 b, float4 c) { return a >= 0.0f ? b : c; }\n";
    s += "float4 dxup_cnd(float4 a, float4 b, float4 c) { return a > 0.5f ? b : c; }\n";
    s += "float4 dxup_lit(float4 s) { float p = clamp(s.w, -127.9961f, 127.9961f); "
         "return float4(1.0f, max(s.x, 0.0f), s.x > 0.0f && s.y > 0.0f ? dxup_pow(s.y, p) : 0.0f, 1.0f); }\n";
    s += "float3 dxup_reflect(float3 n, float3 e) { return 2.0f * dot(n, e) / dot(n, n) * n - e; }\n";
    if (vertex && info.major == 1) {
      s += "float4 dxup_expp(float x) { float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1.0f); }\n";
      s += "float4 dxup_logp(float x) { float a = abs(x); if (a == 0.0f) return float4(-3.402823466e38f, 1.0f, -3.402823466e38f, 1.0f);\n"
           "  uint b = asuint(a); return float4(float(int(b >> 23) - 127), asfloat((b & 0x7fffffu) | 0x3f800000u), log2(a), 1.0f); }\n";
    }
    if (!vertex)
      s += "float4 dxup_luminance(float4 c, float l, float2 p) { return float4(c.rgb * saturate(l * p.x + p.y), c.a); }\n";

    for (uint32_t i = 0; i < 16; i++) {
      const uint8_t type = info.samplerTypes[i];
      if (!type)
        continue;
      const char* texType = type == SamplerCube ? "TextureCube" : type == SamplerVolume ? "Texture3D" : "Texture2D";
      const std::string n = std::to_string(i);
      const bool compare = !vertex && type == Sampler2D && ((options.shadowSamplers >> i) & 1);
      s += std::string(texType) + (vertex ? " vt" : " t") + n + "_ : register(t" + n + ");\n";
      s += std::string(compare ? "SamplerComparisonState " : "SamplerState ") + (vertex ? "vs" : "s") + n
        + " : register(s" + n + ");\n";
    }

    // ---- Inputs and outputs ----
    std::string inputStruct = "struct ShaderInput {\n";
    std::string loadInputs;
    std::string outputStruct = "struct ShaderOutput {\n";
    std::string storeOutputs;
    if (vertex) {
      for (const auto& in : info.inputs) {
        const std::string reg = std::to_string(in.reg);
        const bool isUint = (options.uintInputs >> in.reg) & 1;
        const bool isSint = (options.sintInputs >> in.reg) & 1;
        inputStruct += std::string("  ") + (isUint ? "uint4" : isSint ? "int4" : "float4") + " v" + reg + " : "
          + semantic9(in.usage, in.usageIndex) + ";\n";
        loadInputs += "  " + w.input(in.reg) + " = " + (isUint || isSint ? "float4(input.v" + reg + ")" : "input.v" + reg) + ";\n";
      }
      // Slots: what the shader writes, then up to minVaryings.
      std::string slotValue[kVaryingSlots9];
      uint32_t count = 0;
      if (info.major >= 3) {
        bool position = false;
        std::string pointSize;
        for (const auto& o : info.outputs) {
          const std::string reg = w.output(o.reg);
          if (o.usage == UsagePosition && o.usageIndex == 0) {
            if (!position)
              storeOutputs += "  output.position = " + reg + ";\n";
            position = true;
            continue;
          }
          // The point size rides in the fog slot (z, w = 1) for point sprites.
          if (o.usage == UsagePSize) {
            pointSize = reg + "." + kComponents[o.mask & 1 ? 0 : o.mask & 2 ? 1 : o.mask & 4 ? 2 : 3];
            count = (std::max)(count, 1u);
            info.writesPointSize = true;
            continue;
          }
          const int slot = varyingSlot9(o.usage, o.usageIndex);
          if (slot < 0)
            continue;
          // Packed registers keep their components where they are.
          if (slotValue[slot].empty())
            slotValue[slot] = "0.0f.xxxx";
          const std::string comps = componentsOf(o.mask);
          storeOutputs += "  output.s" + std::to_string(slot) + "." + comps + " = " + reg + "." + comps + ";\n";
          count = (std::max)(count, uint32_t(slot) + 1);
        }
        if (!position)
          storeOutputs += "  output.position = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
        if (!pointSize.empty())
          storeOutputs += "  output.s0.zw = float2(" + pointSize + ", 1.0f);\n";
      } else {
        // Before 3.0: position, fog, colors (clamped), texture coordinates.
        storeOutputs += "  output.position = oPos;\n";
        // Fog, the depth of table fog, the point size (w = 1 when written).
        slotValue[kSlotFog] = w.writesPointSize ? "float4(oFog.x, oPos.w, oPts.x, 1.0f)" : "float4(oFog.x, oPos.w, 0.0f, 0.0f)";
        info.writesPointSize = w.writesPointSize;
        count = 1;
        for (uint32_t i = 0; i < 2; i++) {
          if (w.colorOutputMask & (1u << i)) {
            slotValue[kSlotColor + i] = "saturate(oD" + std::to_string(i) + ")";
            count = (std::max)(count, kSlotColor + i + 1);
          }
        }
        for (uint32_t i = 0; i < 8; i++) {
          if (w.texCoordOutputs & (1u << i)) {
            slotValue[kSlotTexCoord + i] = w.output(i);
            count = (std::max)(count, kSlotTexCoord + i + 1);
          }
        }
        info.outputs.push_back({ 0, UsagePosition, 0, 0xf });
        info.outputs.push_back({ 0, UsageFog, 0, 0x1 });
        for (uint8_t i = 0; i < 2; i++)
          if (w.colorOutputMask & (1u << i)) info.outputs.push_back({ i, UsageColor, i, 0xf });
        for (uint8_t i = 0; i < 8; i++)
          if (w.texCoordOutputs & (1u << i)) info.outputs.push_back({ i, UsageTexCoord, i, 0xf });
      }
      // Direct3D 9 draws a vertex whose depth is NaN at depth 0 (Intel's
      // driver), Direct3D 11 discards its primitive: Mirror's Edge has NaN
      // in its view-projection's z column in some frames.
      if (options.nanDepthZero)
        storeOutputs += "  if ((asuint(output.position.z) & 0x7fffffffu) > 0x7f800000u) output.position.z = 0.0f;\n";
      count = (std::min)((std::max)(count, options.minVaryings), kVaryingSlots9);
      outputStruct += "  float4 position : SV_Position;\n";
      for (uint32_t i = 0; i < count; i++) {
        outputStruct += "  float4 s" + std::to_string(i) + " : " + slotSemantic(i) + ";\n";
        if (info.major < 3)
          storeOutputs += "  output.s" + std::to_string(i) + " = " + (slotValue[i].empty()
            ? (i == kSlotColor || i == kSlotColor + 1 ? std::string("1.0f.xxxx") : std::string("float4(0.0f, 0.0f, 0.0f, 1.0f)"))
            : slotValue[i]) + ";\n";
      }
      info.varyingCount = count;
      if (options.clipPlanes) {
        // User clip planes in clip space, after the varyings (the pixel
        // stage's inputs stay a prefix of these outputs).
        outputStruct += "  float4 clip0 : SV_ClipDistance0;\n  float2 clip1 : SV_ClipDistance1;\n";
        static const char* targets[6] = { "clip0.x", "clip0.y", "clip0.z", "clip0.w", "clip1.x", "clip1.y" };
        for (int plane = 0; plane < 6; plane++)
          storeOutputs += std::string("  output.") + targets[plane] + " = "
            + ((options.clipPlanes >> plane) & 1 ? "dot(output.position, dxup_clipPlanes[" + std::to_string(plane) + "])"
                                                 : std::string("1.0f")) + ";\n";
      }
    } else {
      inputStruct += "  float4 position : SV_Position;\n";
      uint32_t count = 0;
      if (info.major >= 3) {
        for (const auto& in : info.inputs) {
          const std::string comps = componentsOf(in.mask);
          const std::string target = "  " + w.input(in.reg) + "." + comps;
          if (in.usage == UsagePosition && in.usageIndex == 0) {
            loadInputs += target + " = input.position." + comps + ";\n";
            continue;
          }
          const int slot = varyingSlot9(in.usage, in.usageIndex);
          if (slot < 0) {
            loadInputs += target + " = 0.0f;\n";
            continue;
          }
          loadInputs += target + " = input.s" + std::to_string(slot) + "." + comps + ";\n";
          count = (std::max)(count, uint32_t(slot) + 1);
        }
      } else {
        // Fog (fixed function), colors, texture coordinates.
        count = 1;
        for (uint32_t i = 0; i < 2; i++) {
          if (w.colorInputs & (1u << i)) {
            loadInputs += "  " + w.input(i) + " = input.s" + std::to_string(kSlotColor + i) + ";\n";
            count = (std::max)(count, kSlotColor + i + 1);
          }
        }
        for (uint32_t i = 0; i < 8; i++) {
          if (w.texCoordInputs & (1u << i)) {
            const std::string slot = "input.s" + std::to_string(kSlotTexCoord + i);
            loadInputs += "  " + w.tc(i) + " = " + slot + ";\n";
            if (w.texCoordRegisters())
              loadInputs += "  " + w.texture(i) + " = " + slot + ";\n";
            count = (std::max)(count, kSlotTexCoord + i + 1);
          }
        }
      }
      for (uint32_t i = 0; i < count; i++)
        inputStruct += "  float4 s" + std::to_string(i) + " : " + slotSemantic(i) + ";\n";
      if (w.usesVFace)
        inputStruct += "  bool frontFace : SV_IsFrontFace;\n";
      info.varyingCount = count;
      loadInputs += "  vPos = float4(input.position.xy - 0.5f, 0.0f, 0.0f);\n";
      if (w.usesVFace)
        loadInputs += "  vFace = input.frontFace ? 1.0f.xxxx : -1.0f.xxxx;\n";

      // Pixel shader 1.x writes its color to r0.
      if (info.major == 1)
        info.colorOutputs |= 1;
      for (int i = 0; i < 4; i++)
        if (info.colorOutputs & (1u << i))
          outputStruct += "  float4 color" + std::to_string(i) + " : SV_Target" + std::to_string(i) + ";\n";
      if (info.writesDepth)
        outputStruct += "  float depth : SV_Depth;\n";
      // Pixel shader 1.x colors are fixed point: clamped to [0, 1].
      storeOutputs += "  float4 color = " + (info.major == 1 ? "saturate(" + w.temp(0) + ")" : w.color(0)) + ";\n";
      if (info.major < 3) {
        // Fixed-function fog: the vertex's or by depth.
        storeOutputs += "  [branch] if (dxup_flags.x != 0u) {\n"
                        "    float depth = input.s0.y, x = depth * dxup_fogParams.z;\n"
                        "    uint mode = dxup_flags.y;\n"
                        "    float f = mode == 0u ? input.s0.x : mode == 3u ? (dxup_fogParams.y - depth) * dxup_fogParams.w\n"
                        "      : exp(-(mode == 2u ? x * x : x));\n"
                        "    color.rgb = lerp(dxup_fogColor.rgb, color.rgb, saturate(f));\n"
                        "  }\n";
      }
      // The comparisons that pass: less, equal, greater, unordered bits.
      storeOutputs += "  [branch] if (dxup_flags.z != 0u) {\n"
                      "    float a = color.a, ref = dxup_alphaTest.x;\n"
                      "    if ((dxup_flags.z & (a < ref ? 1u : a == ref ? 2u : a > ref ? 4u : 8u)) == 0u) discard;\n"
                      "  }\n";
      for (int i = 0; i < 4; i++)
        if (info.colorOutputs & (1u << i))
          storeOutputs += "  output.color" + std::to_string(i) + " = " + (i == 0 ? std::string("color") : w.color(i)) + ";\n";
      if (info.writesDepth)
        storeOutputs += "  output.depth = oDepth4.x;\n";
    }
    // ---- Registers ----
    auto declare = [&](const char* type, const char* prefix, uint32_t mask, const char* init, std::string& inits) {
      for (uint32_t i = 0; i < 32; i++) {
        if (!((mask >> i) & 1))
          continue;
        const std::string name = prefix + std::to_string(i);
        s += std::string("static ") + type + " " + name + ";\n";
        inits += "  " + name + " = " + init + ";\n";
      }
    };
    std::string inits;
    const uint32_t temps = (std::max)(w.maxTemp, 1u);
    declare("float4", "r", temps >= 32 ? ~0u : (1u << temps) - 1, "0.0f", inits);
    if (w.relativeInputs) {
      s += "static float4 v[16];\n";
      inits += "  [unroll] for (int j = 0; j < 16; j++) v[j] = 0.0f;\n";
    } else {
      declare("float4", "v", w.usedInputs, "0.0f", inits);
    }
    s += "static int4 a0;\nstatic int aL;\nstatic bool4 p0;\n";
    inits += "  a0 = 0; aL = 0; p0 = false;\n";
    if (vertex) {
      s += "static float4 oPos;\nstatic float4 oFog;\nstatic float4 oPts;\nstatic float4 oD0;\nstatic float4 oD1;\n";
      inits += "  oPos = float4(0.0f, 0.0f, 0.0f, 1.0f); oFog = 1.0f; oPts = 0.0f; oD0 = 1.0f; oD1 = 1.0f;\n";
      if (w.relativeOutputs) {
        s += "static float4 o[12];\n";
        inits += "  [unroll] for (int k = 0; k < 12; k++) o[k] = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
      } else {
        declare("float4", "o", w.usedOutputs, "float4(0.0f, 0.0f, 0.0f, 1.0f)", inits);
      }
    } else {
      declare("float4", "oC", w.usedColors, "0.0f", inits);
      declare("float4", "t", w.usedT, "0.0f", inits);
      declare("float4", "tc", w.usedTc, "0.0f", inits);
      declare("float", "tm", w.usedTm, "0.0f", inits);
      s += "static float4 oDepth4;\nstatic float4 vPos;\nstatic float4 vFace;\n";
      inits += "  oDepth4 = input.position.zzzz; vFace = 1.0f;\n";
    }
    s += inputStruct + "};\n" + outputStruct + "};\n";

    // Subroutines: prototypes, the program, the bodies.
    for (const auto& f : w.functions) {
      const size_t brace = f.find(" {");
      if (brace != std::string::npos)
        s += f.substr(0, brace) + ";\n";
    }
    s += "void dxup_body() {\n" + w.body + "}\n";
    for (const auto& f : w.functions)
      s += f;

    s += "ShaderOutput main(ShaderInput input) {\n";
    s += "  ShaderOutput output = (ShaderOutput)0;\n";
    s += inits;
    s += loadInputs;
    s += "  dxup_body();\n";
    s += storeOutputs;
    s += "  return output;\n}\n";

    info.floatConstantCount = info.relativeConstants ? 256 : (std::min)(w.maxConst, 256u);
    hlsl = std::move(s);
    return true;
  }

}
