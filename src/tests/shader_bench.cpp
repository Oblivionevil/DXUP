// GPU time of an application's shaders on their own: every pixel shader of a
// dxup_dump_shaders.txt dump draws a full-screen triangle into 1920x1080
// targets, with generated textures, samplers and zeroed constant buffers.
// Run once with the system's d3d11.dll and once with DXUP's next to the
// executable; compare the two outputs (shader_bench_cmp.py) to find code the
// DXBC -> DXIL path makes slower.
//
// shader_bench.exe <dump dir> <out.csv> [ps|cs]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

template<typename T>
class ComPtr {
public:
  ComPtr() = default;
  ComPtr(std::nullptr_t) { }
  ComPtr(const ComPtr& other) : m_ptr(other.m_ptr) { if (m_ptr) m_ptr->AddRef(); }
  ComPtr& operator=(const ComPtr& other) {
    if (other.m_ptr) other.m_ptr->AddRef();
    reset();
    m_ptr = other.m_ptr;
    return *this;
  }
  ~ComPtr() { reset(); }
  void reset() { if (m_ptr) m_ptr->Release(); m_ptr = nullptr; }
  T* Get() const { return m_ptr; }
  T** GetAddressOf() { reset(); return &m_ptr; }
  T* operator->() const { return m_ptr; }
  explicit operator bool() const { return m_ptr != nullptr; }
private:
  T* m_ptr = nullptr;
};

namespace {

  struct SignatureElement {
    std::string name;
    uint32_t index = 0, systemValue = 0, componentType = 0, reg = 0, mask = 0;
  };

  struct Resource {
    uint32_t slot = 0;
    // D3D10_SB_RESOURCE_DIMENSION: 1 buffer, 2 1D, 3 2D, 4 2DMS, 5 3D,
    // 6 cube, 7 1D array, 8 2D array, 9 2DMS array, 10 cube array.
    uint32_t dimension = 0;
    uint32_t returnType = 0;  // 1 unorm 2 snorm 3 sint 4 uint 5 float
    uint32_t stride = 0;      // structured
    bool     raw = false;
  };

  struct ShaderInfo {
    std::vector<SignatureElement> inputs, outputs;
    std::vector<Resource> srvs, uavs;
    std::vector<uint32_t> samplers, comparisonSamplers;
    std::vector<std::pair<uint32_t, uint32_t>> cbs;  // slot, vectors
    uint32_t threadGroup[3] = { 1, 1, 1 };
    bool unsupported = false;
  };

  uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
  }

  const uint8_t* findChunk(const std::vector<uint8_t>& code, uint32_t type, uint32_t& size) {
    if (code.size() < 32)
      return nullptr;
    uint32_t count = 0;
    std::memcpy(&count, code.data() + 28, 4);
    for (uint32_t i = 0; i < count && 32 + 4 * i + 4 <= code.size(); i++) {
      uint32_t offset = 0;
      std::memcpy(&offset, code.data() + 32 + 4 * i, 4);
      if (offset + 8 > code.size())
        continue;
      uint32_t chunkType = 0, chunkSize = 0;
      std::memcpy(&chunkType, code.data() + offset, 4);
      std::memcpy(&chunkSize, code.data() + offset + 4, 4);
      if (chunkType == type && offset + 8 + chunkSize <= code.size()) {
        size = chunkSize;
        return code.data() + offset + 8;
      }
    }
    return nullptr;
  }

  bool readSignature(const std::vector<uint8_t>& code, bool output, std::vector<SignatureElement>& elements) {
    uint32_t size = 0;
    const uint8_t* data = findChunk(code, output ? fourcc('O', 'S', 'G', 'N') : fourcc('I', 'S', 'G', 'N'), size);
    uint32_t stride = 24;
    if (!data) {
      data = findChunk(code, output ? fourcc('O', 'S', 'G', '1') : fourcc('I', 'S', 'G', '1'), size);
      stride = 32;
    }
    if (!data || size < 8)
      return false;
    uint32_t count = 0;
    std::memcpy(&count, data, 4);
    for (uint32_t i = 0; i < count; i++) {
      const uint8_t* e = data + 8 + i * stride;
      if (e + stride > data + size)
        return false;
      const uint8_t* fields = stride == 32 ? e + 4 : e;
      uint32_t nameOffset, index, systemValue, componentType, reg, mask;
      std::memcpy(&nameOffset, fields, 4);
      std::memcpy(&index, fields + 4, 4);
      std::memcpy(&systemValue, fields + 8, 4);
      std::memcpy(&componentType, fields + 12, 4);
      std::memcpy(&reg, fields + 16, 4);
      mask = fields[20];
      SignatureElement element;
      if (nameOffset < size)
        element.name = reinterpret_cast<const char*>(data + nameOffset);
      element.index = index;
      element.systemValue = systemValue;
      element.componentType = componentType;
      element.reg = reg;
      element.mask = mask;
      elements.push_back(element);
    }
    return true;
  }

  // Declarations of the shader's resources from its token stream.
  bool readDeclarations(const std::vector<uint8_t>& code, ShaderInfo& info) {
    uint32_t size = 0;
    const uint8_t* data = findChunk(code, fourcc('S', 'H', 'E', 'X'), size);
    if (!data)
      data = findChunk(code, fourcc('S', 'H', 'D', 'R'), size);
    if (!data || size < 8)
      return false;
    std::vector<uint32_t> t(size / 4);
    std::memcpy(t.data(), data, t.size() * 4);
    if ((t[0] & 0xf0) > 0x50 || (((t[0] >> 4) & 0xf) == 5 && (t[0] & 0xf) > 0)) {
      info.unsupported = true;  // shader model 5.1 register spaces
      return true;
    }
    size_t at = 2;
    while (at < t.size()) {
      const uint32_t opcode = t[at] & 0x7ff;
      uint32_t length = (t[at] >> 24) & 0x7f;
      if (opcode == 0x35)
        length = at + 1 < t.size() ? t[at + 1] : 0;
      if (!length || at + length > t.size())
        break;
      auto reg = [&](size_t operandAt) { return t[operandAt + 1]; };
      switch (opcode) {
        case 0x58: {  // dcl_resource
          Resource r;
          r.dimension = (t[at] >> 11) & 0x1f;
          r.slot = reg(at + 1);
          r.returnType = t[at + 3] & 0xf;
          info.srvs.push_back(r);
          break;
        }
        case 0xa1: { Resource r; r.slot = reg(at + 1); r.raw = true; r.dimension = 1; info.srvs.push_back(r); break; }
        case 0xa2: { Resource r; r.slot = reg(at + 1); r.stride = t[at + 3]; r.dimension = 1; info.srvs.push_back(r); break; }
        case 0x9c: {  // dcl_uav_typed
          Resource r;
          r.dimension = (t[at] >> 11) & 0x1f;
          r.slot = reg(at + 1);
          r.returnType = t[at + 3] & 0xf;
          info.uavs.push_back(r);
          break;
        }
        case 0x9d: { Resource r; r.slot = reg(at + 1); r.raw = true; r.dimension = 1; info.uavs.push_back(r); break; }
        case 0x9e: { Resource r; r.slot = reg(at + 1); r.stride = t[at + 3]; r.dimension = 1; info.uavs.push_back(r); break; }
        case 0x5a: {  // dcl_sampler
          const uint32_t mode = (t[at] >> 11) & 0xf;
          (mode == 1 ? info.comparisonSamplers : info.samplers).push_back(reg(at + 1));
          break;
        }
        case 0x59: info.cbs.push_back({ t[at + 2], t[at + 3] }); break;
        case 0x9b: info.threadGroup[0] = t[at + 1]; info.threadGroup[1] = t[at + 2]; info.threadGroup[2] = t[at + 3]; break;
        default: break;
      }
      at += length;
    }
    return true;
  }

  struct Bench {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Query> disjoint, begin, end;
    std::vector<ComPtr<ID3D11ShaderResourceView>> keep;
    std::vector<ComPtr<ID3D11UnorderedAccessView>> keepUav;

    ComPtr<ID3D11ShaderResourceView> texture(uint32_t dimension, uint32_t returnType) {
      DXGI_FORMAT format = returnType == 4 ? DXGI_FORMAT_R32G32B32A32_UINT
                         : returnType == 3 ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R8G8B8A8_UNORM;
      const bool wide = format != DXGI_FORMAT_R8G8B8A8_UNORM;
      const UINT texel = wide ? 16 : 4;
      const UINT edge = dimension == 5 ? 32 : 256;
      std::vector<uint8_t> pixels(size_t(edge) * edge * texel * (dimension == 5 ? edge : 1));
      uint32_t seed = 12345;
      for (auto& b : pixels) {
        seed = seed * 1103515245u + 12345u;
        b = uint8_t(seed >> 16);
      }
      if (wide)
        for (size_t i = 0; i < pixels.size(); i += 4)
          pixels[i + 1] = pixels[i + 2] = pixels[i + 3] = 0;  // small integers
      ComPtr<ID3D11ShaderResourceView> view;
      std::vector<D3D11_SUBRESOURCE_DATA> init;
      if (dimension == 5) {
        D3D11_TEXTURE3D_DESC desc = { edge, edge, edge, 1, format, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        D3D11_SUBRESOURCE_DATA data = { pixels.data(), edge * texel, edge * edge * texel };
        ComPtr<ID3D11Texture3D> tex;
        if (SUCCEEDED(device->CreateTexture3D(&desc, &data, tex.GetAddressOf())))
          device->CreateShaderResourceView(tex.Get(), nullptr, view.GetAddressOf());
        return view;
      }
      if (dimension == 2 || dimension == 7)
        return view;  // 1D: not built
      const bool cube = dimension == 6 || dimension == 10;
      const UINT layers = cube ? (dimension == 10 ? 12 : 6) : (dimension == 8 || dimension == 9) ? 4 : 1;
      const UINT samples = (dimension == 4 || dimension == 9) ? 4 : 1;
      UINT mips = samples > 1 ? 1 : 9;
      D3D11_TEXTURE2D_DESC desc = { edge, edge, mips, layers, format, { samples, 0 }, D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | (samples > 1 ? D3D11_BIND_RENDER_TARGET : 0), 0, cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u };
      ComPtr<ID3D11Texture2D> tex;
      if (FAILED(device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf())))
        return view;
      if (samples == 1) {
        for (UINT layer = 0; layer < layers; layer++)
          for (UINT mip = 0; mip < mips; mip++) {
            const UINT size = (std::max)(1u, edge >> mip);
            ctx->UpdateSubresource(tex.Get(), D3D11CalcSubresource(mip, layer, mips), nullptr, pixels.data(), size * texel, 0);
          }
      }
      D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
      srv.Format = format;
      switch (dimension) {
        case 3: srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = mips; break;
        case 8: srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; srv.Texture2DArray.MipLevels = mips; srv.Texture2DArray.ArraySize = layers; break;
        case 4: srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS; break;
        case 9: srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY; srv.Texture2DMSArray.ArraySize = layers; break;
        case 6: srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; srv.TextureCube.MipLevels = mips; break;
        case 10: srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY; srv.TextureCubeArray.MipLevels = mips; srv.TextureCubeArray.NumCubes = 2; break;
        default: return view;
      }
      device->CreateShaderResourceView(tex.Get(), &srv, view.GetAddressOf());
      return view;
    }

    // Typed, raw or structured buffer of 64K elements, zero.
    ComPtr<ID3D11Buffer> buffer(const Resource& r, UINT bind, D3D11_BUFFER_DESC& desc) {
      const UINT elements = 65536;
      const UINT stride = r.stride ? r.stride : 16;
      desc = { elements * stride, D3D11_USAGE_DEFAULT, bind, 0,
        r.raw ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : r.stride ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED : 0u,
        r.stride };
      std::vector<uint8_t> zero(desc.ByteWidth, 0);
      D3D11_SUBRESOURCE_DATA init = { zero.data(), 0, 0 };
      ComPtr<ID3D11Buffer> b;
      device->CreateBuffer(&desc, &init, b.GetAddressOf());
      return b;
    }

    ComPtr<ID3D11ShaderResourceView> bufferSrv(const Resource& r) {
      D3D11_BUFFER_DESC desc;
      auto b = buffer(r, D3D11_BIND_SHADER_RESOURCE, desc);
      ComPtr<ID3D11ShaderResourceView> view;
      if (!b)
        return view;
      D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
      if (r.raw) {
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        srv.BufferEx.NumElements = desc.ByteWidth / 4;
        srv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
      } else {
        srv.Format = r.stride ? DXGI_FORMAT_UNKNOWN : r.returnType == 4 ? DXGI_FORMAT_R32G32B32A32_UINT
                   : r.returnType == 3 ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = 65536;
      }
      device->CreateShaderResourceView(b.Get(), &srv, view.GetAddressOf());
      return view;
    }

    ComPtr<ID3D11UnorderedAccessView> uav(const Resource& r) {
      ComPtr<ID3D11UnorderedAccessView> view;
      const DXGI_FORMAT format = r.returnType == 4 ? DXGI_FORMAT_R32_UINT : r.returnType == 3 ? DXGI_FORMAT_R32_SINT
                               : DXGI_FORMAT_R32_FLOAT;
      if (r.dimension == 1 || r.raw || r.stride) {
        D3D11_BUFFER_DESC desc;
        auto b = buffer(r, D3D11_BIND_UNORDERED_ACCESS, desc);
        if (!b)
          return view;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        if (r.raw) {
          u.Format = DXGI_FORMAT_R32_TYPELESS;
          u.Buffer.NumElements = desc.ByteWidth / 4;
          u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        } else {
          u.Format = r.stride ? DXGI_FORMAT_UNKNOWN : format;
          u.Buffer.NumElements = 65536;
        }
        device->CreateUnorderedAccessView(b.Get(), &u, view.GetAddressOf());
        return view;
      }
      const bool three = r.dimension == 5;
      if (three) {
        D3D11_TEXTURE3D_DESC desc = { 128, 128, 32, 1, format, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
        ComPtr<ID3D11Texture3D> tex;
        if (SUCCEEDED(device->CreateTexture3D(&desc, nullptr, tex.GetAddressOf())))
          device->CreateUnorderedAccessView(tex.Get(), nullptr, view.GetAddressOf());
        return view;
      }
      if (r.dimension != 3 && r.dimension != 8)
        return view;
      const UINT layers = r.dimension == 8 ? 4 : 1;
      D3D11_TEXTURE2D_DESC desc = { 1920, 1080, 1, layers, format, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
      ComPtr<ID3D11Texture2D> tex;
      if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf())))
        device->CreateUnorderedAccessView(tex.Get(), nullptr, view.GetAddressOf());
      return view;
    }

    bool bindCommon(const ShaderInfo& info, bool compute) {
      std::vector<ID3D11ShaderResourceView*> srvs(128, nullptr);
      for (const auto& r : info.srvs) {
        if (r.slot >= 128)
          return false;
        auto view = (r.dimension == 1 || r.raw || r.stride) ? bufferSrv(r) : texture(r.dimension, r.returnType);
        if (!view)
          return false;
        srvs[r.slot] = view.Get();
        keep.push_back(view);
      }
      static ComPtr<ID3D11SamplerState> linear, comparison;
      if (!linear) {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&desc, linear.GetAddressOf());
        desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        device->CreateSamplerState(&desc, comparison.GetAddressOf());
      }
      std::vector<ID3D11SamplerState*> samplers(16, nullptr);
      for (uint32_t s : info.samplers)
        if (s < 16) samplers[s] = linear.Get();
      for (uint32_t s : info.comparisonSamplers)
        if (s < 16) samplers[s] = comparison.Get();
      std::vector<ID3D11Buffer*> cbs(14, nullptr);
      std::vector<ComPtr<ID3D11Buffer>> cbKeep;
      for (const auto& cb : info.cbs) {
        if (cb.first >= 14)
          return false;
        std::vector<float> zero(size_t((std::max)(cb.second, 1u)) * 4, 0.0f);
        D3D11_BUFFER_DESC desc = { UINT(zero.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA init = { zero.data(), 0, 0 };
        ComPtr<ID3D11Buffer> b;
        if (FAILED(device->CreateBuffer(&desc, &init, b.GetAddressOf())))
          return false;
        cbs[cb.first] = b.Get();
        cbKeep.push_back(b);
      }
      if (compute) {
        ctx->CSSetShaderResources(0, 128, srvs.data());
        ctx->CSSetSamplers(0, 16, samplers.data());
        ctx->CSSetConstantBuffers(0, 14, cbs.data());
      } else {
        ctx->PSSetShaderResources(0, 128, srvs.data());
        ctx->PSSetSamplers(0, 16, samplers.data());
        ctx->PSSetConstantBuffers(0, 14, cbs.data());
      }
      return true;
    }

    // GPU microseconds per run of `work`, the median of five measurements
    // of `reps` runs each.
    template<typename Work>
    double time(UINT reps, Work&& work) {
      std::vector<double> samples;
      for (int m = 0; m < 6; m++) {
        ctx->Begin(disjoint.Get());
        ctx->End(begin.Get());
        for (UINT i = 0; i < reps; i++)
          work();
        ctx->End(end.Get());
        ctx->End(disjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        UINT64 t0 = 0, t1 = 0;
        while (ctx->GetData(disjoint.Get(), &dj, sizeof(dj), 0) != S_OK) {}
        while (ctx->GetData(begin.Get(), &t0, sizeof(t0), 0) != S_OK) {}
        while (ctx->GetData(end.Get(), &t1, sizeof(t1), 0) != S_OK) {}
        if (m == 0)
          continue;  // warm-up: pipeline creation
        samples.push_back(dj.Disjoint || t1 <= t0 ? 0.0 : double(t1 - t0) * 1e6 / double(dj.Frequency) / reps);
      }
      std::sort(samples.begin(), samples.end());
      return samples[samples.size() / 2];
    }
  };

  std::string partnerVs(const ShaderInfo& info) {
    std::string source = "struct O {\n";
    bool position = false;
    uint32_t index = 0;
    std::string body;
    for (const auto& e : info.inputs) {
      if (e.systemValue == 1) { position = true; source += "  float4 p : SV_Position;\n"; continue; }
      if (e.systemValue != 0)
        continue;  // front face, primitive id, sample index, coverage: from the rasterizer
      uint32_t components = 0;
      for (uint32_t bit = 0; bit < 4; bit++)
        components += (e.mask >> bit) & 1;
      if (!components)
        components = 4;
      const char* type = e.componentType == 1 ? "uint" : e.componentType == 2 ? "int" : "float";
      char line[160];
      snprintf(line, sizeof(line), "  %s%u e%u : %s%u;\n", type, components, index, e.name.c_str(), e.index);
      source += line;
      if (e.componentType == 3) {
        const char* swizzle[] = { "", ".x", ".xy", ".xyz", "" };
        snprintf(line, sizeof(line), "  o.e%u = float4(uv, 0.5f, 1.0f)%s;\n", index, swizzle[components]);
        body += line;
      }
      index++;
    }
    if (!position)
      source += "  float4 p : SV_Position;\n";
    source += "};\nO main(uint id : SV_VertexID) {\n  O o = (O)0;\n  float2 uv = float2((id << 1) & 2, id & 2);\n"
              "  o.p = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.5f, 1.0f);\n" + body + "  return o;\n}\n";
    return source;
  }

  std::vector<uint8_t> readFile(const std::string& path) {
    std::vector<uint8_t> data;
    if (FILE* f = fopen(path.c_str(), "rb")) {
      fseek(f, 0, SEEK_END);
      data.resize(size_t(ftell(f)));
      fseek(f, 0, SEEK_SET);
      if (fread(data.data(), 1, data.size(), f) != data.size())
        data.clear();
      fclose(f);
    }
    return data;
  }

}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("shader_bench <dump dir> <out.csv> [ps|cs]\n");
    return 1;
  }
  const std::string dir = argv[1];
  const std::string stage = argc > 3 ? argv[3] : "ps";
  Bench bench;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
        bench.device.GetAddressOf(), nullptr, bench.ctx.GetAddressOf()))) {
    std::printf("no device\n");
    return 1;
  }
  D3D11_QUERY_DESC q = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
  bench.device->CreateQuery(&q, bench.disjoint.GetAddressOf());
  q.Query = D3D11_QUERY_TIMESTAMP;
  bench.device->CreateQuery(&q, bench.begin.GetAddressOf());
  bench.device->CreateQuery(&q, bench.end.GetAddressOf());

  FILE* out = fopen(argv[2], "w");
  if (!out)
    return 1;
  fprintf(out, "shader,us\n");
  WIN32_FIND_DATAA find;
  HANDLE handle = FindFirstFileA((dir + "\\" + stage + "_*.dxbc").c_str(), &find);
  if (handle == INVALID_HANDLE_VALUE)
    return 1;
  uint32_t measured = 0, skipped = 0;
  do {
    const std::string name = find.cFileName;
    const auto code = readFile(dir + "\\" + name);
    ShaderInfo info;
    if (code.empty() || !readDeclarations(code, info) || info.unsupported) {
      skipped++;
      continue;
    }
    bench.ctx->ClearState();
    bench.keep.clear();
    bench.keepUav.clear();
    double us = 0.0;
    if (stage == "cs") {
      ComPtr<ID3D11ComputeShader> cs;
      if (FAILED(bench.device->CreateComputeShader(code.data(), code.size(), nullptr, cs.GetAddressOf()))
       || !bench.bindCommon(info, true)) {
        skipped++;
        continue;
      }
      std::vector<ID3D11UnorderedAccessView*> uavs(8, nullptr);
      bool ok = true;
      for (const auto& r : info.uavs) {
        auto view = r.slot < 8 ? bench.uav(r) : nullptr;
        if (!view) { ok = false; break; }
        uavs[r.slot] = view.Get();
        bench.keepUav.push_back(view);
      }
      if (!ok) { skipped++; continue; }
      bench.ctx->CSSetUnorderedAccessViews(0, 8, uavs.data(), nullptr);
      bench.ctx->CSSetShader(cs.Get(), nullptr, 0);
      // Groups covering 1920x1080 threads (one dimension: 2M threads).
      const UINT gx = info.threadGroup[1] > 1 ? (1920 + info.threadGroup[0] - 1) / info.threadGroup[0]
                                              : (2073600 + info.threadGroup[0] - 1) / info.threadGroup[0];
      const UINT gy = info.threadGroup[1] > 1 ? (1080 + info.threadGroup[1] - 1) / info.threadGroup[1] : 1;
      us = bench.time(4, [&] { bench.ctx->Dispatch((std::min)(gx, 65535u), gy, 1); });
    } else {
      if (!readSignature(code, false, info.inputs) || !readSignature(code, true, info.outputs) || !info.uavs.empty()) {
        skipped++;
        continue;
      }
      const std::string vsSource = partnerVs(info);
      ComPtr<ID3DBlob> vsBlob, errors;
      ComPtr<ID3D11VertexShader> vs;
      ComPtr<ID3D11PixelShader> ps;
      if (FAILED(D3DCompile(vsSource.data(), vsSource.size(), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0,
            vsBlob.GetAddressOf(), errors.GetAddressOf()))
       || FAILED(bench.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vs.GetAddressOf()))
       || FAILED(bench.device->CreatePixelShader(code.data(), code.size(), nullptr, ps.GetAddressOf()))
       || !bench.bindCommon(info, false)) {
        skipped++;
        continue;
      }
      // Targets of the outputs' types; depth when the shader writes it.
      std::vector<ComPtr<ID3D11RenderTargetView>> rtvKeep;
      ID3D11RenderTargetView* rtvs[8] = {};
      UINT targetCount = 0;
      bool depthOut = false;
      for (const auto& o : info.outputs) {
        if (o.name == "SV_Depth" || o.name == "SV_DepthGreaterEqual" || o.name == "SV_DepthLessEqual") { depthOut = true; continue; }
        if (o.name != "SV_Target" || o.index >= 8)
          continue;
        const DXGI_FORMAT format = o.componentType == 1 ? DXGI_FORMAT_R32G32B32A32_UINT
                                 : o.componentType == 2 ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        D3D11_TEXTURE2D_DESC desc = { 1920, 1080, 1, 1, format, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        bench.device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
        if (tex)
          bench.device->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf());
        rtvs[o.index] = rtv.Get();
        rtvKeep.push_back(rtv);
        targetCount = (std::max)(targetCount, o.index + 1);
      }
      ComPtr<ID3D11DepthStencilView> dsv;
      ComPtr<ID3D11DepthStencilState> depthState;
      if (depthOut) {
        D3D11_TEXTURE2D_DESC desc = { 1920, 1080, 1, 1, DXGI_FORMAT_D32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 0 };
        ComPtr<ID3D11Texture2D> tex;
        bench.device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
        if (tex)
          bench.device->CreateDepthStencilView(tex.Get(), nullptr, dsv.GetAddressOf());
        D3D11_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable = TRUE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        bench.device->CreateDepthStencilState(&ds, depthState.GetAddressOf());
      }
      bench.ctx->OMSetRenderTargets(targetCount, rtvs, dsv.Get());
      bench.ctx->OMSetDepthStencilState(depthState.Get(), 0);
      D3D11_VIEWPORT vp = { 0, 0, 1920, 1080, 0, 1 };
      bench.ctx->RSSetViewports(1, &vp);
      bench.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      bench.ctx->VSSetShader(vs.Get(), nullptr, 0);
      bench.ctx->PSSetShader(ps.Get(), nullptr, 0);
      us = bench.time(4, [&] { bench.ctx->Draw(3, 0); });
    }
    fprintf(out, "%s,%.2f\n", name.c_str(), us);
    fflush(out);
    measured++;
  } while (FindNextFileA(handle, &find));
  FindClose(handle);
  fclose(out);
  std::printf("%u measured, %u skipped\n", measured, skipped);
  return 0;
}
