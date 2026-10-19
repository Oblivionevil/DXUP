#include "legacy_device.h"
#include "legacy_formats.h"

#include "../util/util_types.h"
#include "../util/util_config.h"
#include "../util/util_thread.h"

#include "ffp_vs.h"
#include "ffp_ps.h"
#include "quad_vs.h"
#include "clear_ps.h"
#include "blit_ps.h"
#include "gamma_ps.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <xmmintrin.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>

#include <d3dcompiler.h>
#include <dxgi1_5.h>

namespace dxup::legacy {

  // ============================================================
  // Flexible vertex formats
  // ============================================================

  namespace {
    uint32_t lowestBit(uint32_t bits) {
#if defined(_MSC_VER)
      unsigned long index = 0;
      _BitScanForward(&index, bits);
      return uint32_t(index);
#else
      return uint32_t(__builtin_ctz(bits));
#endif
    }

    void writeLines(uint8_t* destination, const void* data, uint32_t size) {
      // Write-combined memory (mapped upload buffers) in whole 64-byte
      // lines, the tail filled with zeros: partly written lines wait in
      // the CPU until the next locked instruction, which then stalls.
      std::memcpy(destination, data, size);
      const uint32_t tail = ((size + 63) & ~63u) - size;
      if (tail)
        std::memset(destination + size, 0, tail);
    }

    uint32_t blendWeightCount(uint32_t fvf) {
      switch (fvf & FvfPositionMask) {
        case 0x006: return 1;
        case 0x008: return 2;
        case 0x00a: return 3;
        case 0x00c: return 4;
        case 0x00e: return 5;
        default:    return 0;
      }
    }

    uint32_t texCoordSize(uint32_t fvf, uint32_t set) {
      switch ((fvf >> (16 + set * 2)) & 3) {
        case 1:  return 3;
        case 2:  return 4;
        case 3:  return 1;
        default: return 2;
      }
    }
  }

  uint32_t fvfStride(uint32_t fvf) {
    uint32_t size = 0;
    switch (fvf & FvfPositionMask) {
      case FvfXyz:    size += 12; break;
      case FvfXyzRhw: size += 16; break;
      case FvfXyzW:   size += 16; break;
      default:
        if (blendWeightCount(fvf))
          size += 12 + 4 * blendWeightCount(fvf);
        break;
    }
    if (fvf & FvfNormal)   size += 12;
    if (fvf & FvfPSize)    size += 4;
    if (fvf & FvfDiffuse)  size += 4;
    if (fvf & FvfSpecular) size += 4;
    const uint32_t texCount = (std::min)((fvf & FvfTexCountMask) >> FvfTexCountShift, 8u);
    for (uint32_t i = 0; i < texCount; i++)
      size += 4 * texCoordSize(fvf, i);
    return size;
  }

  namespace {
    float asFloat(uint32_t value) {
      float result;
      std::memcpy(&result, &value, sizeof(result));
      return result;
    }

    uint32_t asUint(float value) {
      uint32_t result;
      std::memcpy(&result, &value, sizeof(result));
      return result;
    }

    void colorToFloats(uint32_t argb, float* out) {
      out[0] = float((argb >> 16) & 255) / 255.0f;
      out[1] = float((argb >> 8) & 255) / 255.0f;
      out[2] = float(argb & 255) / 255.0f;
      out[3] = float(argb >> 24) / 255.0f;
    }

    void identity(float* m) {
      std::memset(m, 0, 16 * sizeof(float));
      m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    // Rows at a time: results stored element by element and read back as
    // vectors stalled on store forwarding. Same sums in the same order.
    void multiply(const float* a, const float* b, float* out) {
      const __m128 b0 = _mm_loadu_ps(b), b1 = _mm_loadu_ps(b + 4), b2 = _mm_loadu_ps(b + 8), b3 = _mm_loadu_ps(b + 12);
      __m128 rows[4];
      for (int r = 0; r < 4; r++) {
        __m128 sum = _mm_mul_ps(_mm_set1_ps(a[r * 4]), b0);
        sum = _mm_add_ps(sum, _mm_mul_ps(_mm_set1_ps(a[r * 4 + 1]), b1));
        sum = _mm_add_ps(sum, _mm_mul_ps(_mm_set1_ps(a[r * 4 + 2]), b2));
        rows[r] = _mm_add_ps(sum, _mm_mul_ps(_mm_set1_ps(a[r * 4 + 3]), b3));
      }
      for (int r = 0; r < 4; r++)
        _mm_storeu_ps(out + r * 4, rows[r]);
    }

    // Inverse transpose of the upper 3x3, for normals (row vectors).
    void normalMatrixOf(const float* m, float* out) {
      const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
      const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
      const float inv = std::fabs(det) > 1e-20f ? 1.0f / det : 0.0f;
      // transpose(inverse(M)) = cofactor matrix / det
      identity(out);
      out[0] = (e * i - f * h) * inv; out[1] = -(d * i - f * g) * inv; out[2] = (d * h - e * g) * inv;
      out[4] = -(b * i - c * h) * inv; out[5] = (a * i - c * g) * inv; out[6] = -(a * h - b * g) * inv;
      out[8] = (b * f - c * e) * inv; out[9] = -(a * f - c * d) * inv; out[10] = (a * e - b * d) * inv;
    }

    // Inverse of a 4x4 matrix (row-major); false when singular.
    bool invert(const float* m, float* out) {
      float inv[16];
      inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
      inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
      inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
      inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
      inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
      inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
      inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
      inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
      inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
      inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
      inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
      inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
      inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
      inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
      inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
      inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
      const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
      if (det == 0.0f)
        return false;
      for (int i = 0; i < 16; i++)
        out[i] = inv[i] / det;
      return true;
    }

    void transformPoint(const float* m, const Vector& v, float* out) {
      for (int c = 0; c < 3; c++)
        out[c] = v.x * m[c] + v.y * m[4 + c] + v.z * m[8 + c] + m[12 + c];
    }

    void transformDirection(const float* m, const Vector& v, float* out) {
      for (int c = 0; c < 3; c++)
        out[c] = v.x * m[c] + v.y * m[4 + c] + v.z * m[8 + c];
      const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
      if (length > 0.0f)
        for (int c = 0; c < 3; c++) out[c] /= length;
    }

    HMODULE loadD3D11() {
      // DXUP's d3d11.dll next to this module, else the system's.
      HMODULE self = nullptr;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&loadD3D11), &self);
      wchar_t path[MAX_PATH] = {};
      if (self && GetModuleFileNameW(self, path, MAX_PATH)) {
        if (wchar_t* name = wcsrchr(path, L'\\')) {
          wcscpy_s(name + 1, MAX_PATH - size_t(name + 1 - path), L"d3d11.dll");
          if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            if (HMODULE module = LoadLibraryW(path))
              return module;
        }
      }
      return LoadLibraryW(L"d3d11.dll");
    }
  }

  // ============================================================
  // Constant buffers (layouts of ffp.hlsl)
  // ============================================================

  struct Device::TransformConstants {
    float    worldView[4][16];
    float    normalMatrix[4][16];
  };

  struct Device::VertexConstants {
    float    projection[16];
    float    textureMatrix[8][16];
    float    rhwScale[4];
    float    rhwOffset[4];
    float    materialDiffuse[4];
    float    materialAmbient[4];
    float    materialSpecular[4];
    float    materialEmissive[4];
    float    globalAmbient[4];
    float    fogVertex[4];
    uint32_t vertexFlags[4];
    uint32_t colorSources[4];
    uint32_t vertexFlags2[4];
    uint32_t texCoordSetup[8][4];
    struct {
      float diffuse[4], specular[4], ambient[4], position[4], direction[4], params0[4], params1[4];
    } lights[8];
    float    clipPlanes[6][4];
    uint32_t clipFlags[4];
  };

  struct Device::PixelConstants {
    uint32_t stageOps[8][4];
    uint32_t stageArgs[8][4];
    float    stageConstant[8][4];
    float    bumpMatrix[8][4];
    float    bumpLuminance[8][4];
    float    textureFactor[4];
    float    fogColor[4];
    float    fogTable[4];
    float    alphaTest[4];
    uint32_t pixelFlags[4];
  };

  struct QuadConstants {
    float rect[4];
    float texRect[4];
    float color[4];
    float depth[4];
  };

  // ============================================================
  // Texture
  // ============================================================

  Texture::Texture(Device* device, const TextureDesc& desc)
  : m_device(device), m_desc(desc) {
    if (!m_desc.width || !m_desc.height)
      return;
    if (!m_desc.levels)
      m_desc.levels = 1;
    if (!m_desc.depth || !m_desc.volume)
      m_desc.depth = 1;
    if (!m_desc.samples)
      m_desc.samples = 1;
    const FormatInfo info = formatInfo(m_desc.format);
    m_levels.resize(subresourceCount());
    // Render targets without memory: nothing to create.
    if (m_desc.format == FormatNULL) {
      m_valid = true;
      return;
    }
    if (m_desc.systemMemory) {
      m_valid = true;
      return;
    }

    DXGI_FORMAT format = directDxgiFormat(m_desc.format);
    UINT support = m_desc.volume ? D3D11_FORMAT_SUPPORT_TEXTURE3D : D3D11_FORMAT_SUPPORT_TEXTURE2D;
    if (m_desc.renderTarget)
      support |= D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    if (info.depth)
      support = D3D11_FORMAT_SUPPORT_DEPTH_STENCIL;
    if (format == DXGI_FORMAT_UNKNOWN || !m_device->supports(format, support)) {
      if (info.depth || info.block)
        return;
      format = DXGI_FORMAT_B8G8R8A8_UNORM;
      m_convert = true;
      static std::atomic<uint64_t> s_logged{0};
      const uint64_t bit = 1ull << (uint32_t(m_desc.format) & 63);
      if (!(s_logged.fetch_or(bit) & bit))
        logMessage(LogLevel::Info, "D3D9: textures of format %u converted to B8G8R8A8", uint32_t(m_desc.format));
    }
    m_dxgiFormat = format;
    DXGI_FORMAT resourceFormat = format;
    DXGI_FORMAT dsvFormat = format;
    SrgbFormats srgb = {};
    DepthFormats depthFormat = {};
    const bool sampledDepth = info.depth && m_desc.sampledDepth && m_desc.samples == 1 && depthFormats(format, depthFormat);
    if (sampledDepth) {
      resourceFormat = depthFormat.typeless;
      dsvFormat = depthFormat.dsv;
      m_dxgiFormat = depthFormat.srv;
      m_shadow = shadowFormat(m_desc.format);
    } else if (m_desc.srgb && srgbFormats(format, srgb)) {
      resourceFormat = srgb.typeless;
      m_srgbFormat = srgb.srgb;
    }

    UINT bind = info.depth ? D3D11_BIND_DEPTH_STENCIL : 0;
    if (!info.depth || sampledDepth)
      bind |= m_desc.samples > 1 ? 0 : D3D11_BIND_SHADER_RESOURCE;
    if (m_desc.renderTarget || m_desc.autoMips)
      bind |= D3D11_BIND_RENDER_TARGET;
    const UINT misc = (m_desc.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0)
      | (m_desc.autoMips && m_desc.levels > 1 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0);

    if (m_desc.volume) {
      D3D11_TEXTURE3D_DESC td = {};
      td.Width = m_desc.width;
      td.Height = m_desc.height;
      td.Depth = m_desc.depth;
      if (info.block) {
        td.Width = (td.Width + 3) & ~3u;
        td.Height = (td.Height + 3) & ~3u;
      }
      td.MipLevels = m_desc.levels;
      td.Format = resourceFormat;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if (FAILED(m_device->d3d11()->CreateTexture3D(&td, nullptr, m_volume.put())))
        return;
    } else {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = m_desc.width;
      td.Height = m_desc.height;
      if (info.block) {
        // Block compressed textures cover whole blocks.
        td.Width = (td.Width + 3) & ~3u;
        td.Height = (td.Height + 3) & ~3u;
      }
      td.MipLevels = m_desc.levels;
      td.ArraySize = m_desc.cube ? 6 : 1;
      td.Format = resourceFormat;
      td.SampleDesc.Count = m_desc.samples;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = bind;
      td.MiscFlags = misc;
      if (m_desc.backBuffer)
        m_device->noteBackBufferSize(m_desc.width, m_desc.height);
      if (FAILED(m_device->d3d11()->CreateTexture2D(&td, nullptr, m_texture.put())))
        return;
    }
    ID3D11Resource* resource = d3dResource();
    if (info.depth) {
      D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
      dd.Format = dsvFormat;
      if (m_desc.samples > 1) {
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
      } else if (m_desc.cube) {
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dd.Texture2DArray.ArraySize = 1;
      } else {
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
      }
      if (FAILED(m_device->d3d11()->CreateDepthStencilView(resource, &dd, m_dsv.put())))
        return;
    }
    if (bind & D3D11_BIND_SHADER_RESOURCE) {
      if (!srv(false))
        return;
    }
    m_rtvs.resize(subresourceCount(), nullptr);
    m_rtvsSrgb.resize(subresourceCount(), nullptr);
    m_valid = true;
  }

  Texture::~Texture() {
    for (auto* rtv : m_rtvs)
      if (rtv) rtv->Release();
    for (auto* rtv : m_rtvsSrgb)
      if (rtv) rtv->Release();
    for (auto* staging : m_staging)
      if (staging) staging->Release();
  }

  ID3D11Resource* Texture::d3dResource() const {
    if (m_volume)
      return m_volume.get();
    return m_texture.get();
  }

  ID3D11ShaderResourceView* Texture::srv(bool srgb) {
    Ref<ID3D11ShaderResourceView>& view = srgb && m_srgbFormat != DXGI_FORMAT_UNKNOWN ? m_srvSrgb : m_srv;
    if (view || !d3dResource() || m_desc.samples > 1)
      return view.get();
    if (formatInfo(m_desc.format).depth && !m_desc.sampledDepth)
      return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = &view == &m_srvSrgb ? m_srgbFormat : m_dxgiFormat;
    if (m_desc.volume) {
      sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
      sd.Texture3D.MipLevels = m_desc.levels;
    } else if (m_desc.cube) {
      sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
      sd.TextureCube.MipLevels = m_desc.levels;
    } else {
      sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MipLevels = m_desc.levels;
    }
    m_device->createShaderResourceView(d3dResource(), &sd, shaderComponentMapping(m_desc.format), view.put());
    return view.get();
  }

  ID3D11Texture2D* Texture::staging(uint32_t subresource) {
    const uint32_t mip = subresource % m_desc.levels;
    if (m_staging.size() < m_desc.levels)
      m_staging.resize(m_desc.levels, nullptr);
    if (!m_staging[mip] && m_texture) {
      D3D11_TEXTURE2D_DESC td;
      m_texture->GetDesc(&td);
      td.Width = (std::max)(1u, td.Width >> mip);
      td.Height = (std::max)(1u, td.Height >> mip);
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_STAGING;
      td.BindFlags = 0;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
      td.MiscFlags = 0;
      m_device->d3d11()->CreateTexture2D(&td, nullptr, &m_staging[mip]);
    }
    return m_staging[mip];
  }

  void Texture::write(uint32_t subresource, const D3D11_BOX& box, const void* data, uint32_t pitch, uint32_t depthPitch) {
    const FormatInfo info = formatInfo(m_dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM && m_convert ? FormatA8R8G8B8 : m_desc.format);
    const size_t bytes = size_t(pitch) * (box.bottom - box.top);
    ID3D11Texture2D* copy = !info.block && !m_volume && m_desc.samples == 1 && bytes >= (1u << 20) ? staging(subresource) : nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (!copy || FAILED(m_device->context()->Map(copy, 0, D3D11_MAP_WRITE, 0, &mapped))) {
      m_device->context()->UpdateSubresource(d3dResource(), subresource, &box, data, pitch, depthPitch);
      return;
    }
    const uint32_t rowBytes = (box.right - box.left) * info.bytesPerBlock;
    for (uint32_t y = box.top; y < box.bottom; y++)
      std::memcpy(static_cast<uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch + size_t(box.left) * info.bytesPerBlock,
        static_cast<const uint8_t*>(data) + size_t(y - box.top) * pitch, rowBytes);
    m_device->context()->Unmap(copy, 0);
    m_device->context()->CopySubresourceRegion(m_texture.get(), subresource, box.left, box.top, 0, copy, 0, &box);
  }

  uint32_t Texture::width(uint32_t subresource) const {
    return (std::max)(1u, m_desc.width >> (subresource % m_desc.levels));
  }

  uint32_t Texture::height(uint32_t subresource) const {
    return (std::max)(1u, m_desc.height >> (subresource % m_desc.levels));
  }

  uint32_t Texture::depth(uint32_t subresource) const {
    return (std::max)(1u, m_desc.depth >> (subresource % m_desc.levels));
  }

  uint32_t Texture::pitch(uint32_t subresource) const {
    return legacyPitch(m_desc.format, width(subresource));
  }

  uint32_t Texture::slicePitch(uint32_t subresource) const {
    return pitch(subresource) * legacyRows(m_desc.format, height(subresource));
  }

  size_t Texture::levelSize(uint32_t subresource) const {
    // At least what a lock lets the application write (ATI1: height rows of
    // the pitch it reports, twice its blocks).
    const size_t locked = size_t(legacyLockPitch(m_desc.format, width(subresource))) * height(subresource);
    return (std::max)(size_t(slicePitch(subresource)), locked) * depth(subresource);
  }

  ID3D11RenderTargetView* Texture::rtv(uint32_t subresource, bool srgb) {
    if (!m_texture || !m_desc.renderTarget || subresource >= m_rtvs.size())
      return nullptr;
    srgb = srgb && m_srgbFormat != DXGI_FORMAT_UNKNOWN;
    ID3D11RenderTargetView*& view = srgb ? m_rtvsSrgb[subresource] : m_rtvs[subresource];
    if (!view) {
      D3D11_RENDER_TARGET_VIEW_DESC rd = {};
      rd.Format = srgb ? m_srgbFormat : m_dxgiFormat;
      if (m_desc.samples > 1) {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
      } else if (m_desc.cube) {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.MipSlice = subresource % m_desc.levels;
        rd.Texture2DArray.FirstArraySlice = subresource / m_desc.levels;
        rd.Texture2DArray.ArraySize = 1;
      } else {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = subresource;
      }
      m_device->d3d11()->CreateRenderTargetView(m_texture.get(), &rd, &view);
    }
    return view;
  }

  bool Texture::lock(uint32_t subresource, const Rect* rect, bool readOnly, void** data, uint32_t* pitch, bool discard) {
    if (!rect)
      return lockBox(subresource, nullptr, readOnly, data, pitch, nullptr, discard);
    const Box box = { uint32_t(rect->left), uint32_t(rect->top), 0, uint32_t(rect->right), uint32_t(rect->bottom), 1 };
    return lockBox(subresource, &box, readOnly, data, pitch, nullptr, discard);
  }

  bool Texture::lockBox(uint32_t subresource, const Box* box, bool readOnly, void** data, uint32_t* rowPitch,
                        uint32_t* slicePitchOut, bool discard) {
    if (subresource >= m_levels.size() || !data || m_desc.format == FormatNULL)
      return false;
    Level& level = m_levels[subresource];
    if (level.data.empty())
      level.data.resize(levelSize(subresource));
    if (m_gpuWritten && d3dResource() && !discard) {
      // What the application wrote before goes to the GPU first.
      flush();
      readBack(subresource);
    }
    level.locked = true;
    level.lockReadOnly = readOnly;
    const uint32_t rowBytes = pitch(subresource);
    const uint32_t sliceBytes = slicePitch(subresource);
    size_t offset = 0;
    if (box) {
      const FormatInfo info = formatInfo(m_desc.format);
      if (info.block)
        offset = size_t(box->top / 4) * rowBytes + size_t(box->left / 4) * info.bytesPerBlock;
      else
        offset = size_t(box->top) * rowBytes + size_t(box->left) * info.bytesPerBlock;
      offset += size_t(box->front) * sliceBytes;
    }
    *data = level.data.data() + offset;
    if (rowPitch)
      *rowPitch = box ? rowBytes : legacyLockPitch(m_desc.format, width(subresource));
    if (slicePitchOut)
      *slicePitchOut = sliceBytes;
    return true;
  }

  void Texture::unlock(uint32_t subresource) {
    if (subresource >= m_levels.size())
      return;
    Level& level = m_levels[subresource];
    if (!level.locked)
      return;
    level.locked = false;
    if (!level.lockReadOnly && d3dResource()) {
      level.dirty = true;
      markPending();
    }
  }

  void Texture::markPending() {
    m_anyDirty = true;
    m_device->noteTexturePending();
  }

  void Texture::markGpuWritten() {
    m_gpuWritten = true;
    if (m_desc.autoMips) {
      m_mipsDirty = true;
      m_device->noteTexturePending();
    }
  }

  void Texture::flush() {
    if (m_anyDirty) {
      m_anyDirty = false;
      for (uint32_t i = 0; i < m_levels.size(); i++) {
        if (m_levels[i].dirty) {
          m_levels[i].dirty = false;
          upload(i);
          if (m_desc.autoMips && i % m_desc.levels == 0)
            m_mipsDirty = true;
        }
      }
    }
    if (m_mipsDirty) {
      m_mipsDirty = false;
      if (m_srv && m_desc.levels > 1)
        m_device->context()->GenerateMips(m_srv.get());
    }
  }

  void Texture::upload(uint32_t subresource) {
    const Level& level = m_levels[subresource];
    if (!d3dResource() || level.data.empty())
      return;
    const uint32_t w = width(subresource), h = height(subresource), d = depth(subresource);
    const uint32_t srcPitch = pitch(subresource);
    const uint32_t srcSlice = slicePitch(subresource);
    const FormatInfo info = formatInfo(m_desc.format);
    D3D11_BOX box = { 0, 0, 0, w, h, d };
    if (info.block) {
      box.right = (w + 3) & ~3u;
      box.bottom = (h + 3) & ~3u;
      // The D3D11 subresource may be smaller than whole blocks at small mips.
      uint32_t fullWidth = m_desc.width, fullHeight = m_desc.height;
      if (m_texture) {
        D3D11_TEXTURE2D_DESC td;
        m_texture->GetDesc(&td);
        fullWidth = td.Width;
        fullHeight = td.Height;
      } else {
        D3D11_TEXTURE3D_DESC td;
        m_volume->GetDesc(&td);
        fullWidth = td.Width;
        fullHeight = td.Height;
      }
      const uint32_t mip = subresource % m_desc.levels;
      box.right = (std::min)(box.right, (std::max)(1u, fullWidth >> mip));
      box.bottom = (std::min)(box.bottom, (std::max)(1u, fullHeight >> mip));
      if ((box.right & 3) || (box.bottom & 3)) {
        box.right = (std::max)(1u, fullWidth >> mip);
        box.bottom = (std::max)(1u, fullHeight >> mip);
      }
    }
    const bool keyed = m_colorKeyEnabled;
    if (m_desc.format == FormatA2R10G10B10) {
      std::vector<uint8_t> swapped(size_t(w) * h * d * 4);
      for (uint32_t z = 0; z < d; z++)
        for (uint32_t y = 0; y < h; y++)
          std::memcpy(swapped.data() + (size_t(z) * h + y) * w * 4,
                      level.data.data() + size_t(z) * srcSlice + size_t(y) * srcPitch, size_t(w) * 4);
      swapRedBlue1010102(swapped.data(), size_t(w) * h * d);
      write(subresource, box, swapped.data(), w * 4, w * h * 4);
      return;
    }
    if (!m_convert && !keyed && m_desc.format != FormatX1R5G5B5) {
      write(subresource, box, level.data.data(), srcPitch, srcSlice);
      return;
    }
    std::vector<uint32_t> converted(size_t(w) * h * d);
    const uint32_t bpp = info.bytesPerBlock;
    for (uint32_t z = 0; z < d; z++) {
      for (uint32_t y = 0; y < h; y++) {
        const uint8_t* row = level.data.data() + size_t(z) * srcSlice + size_t(y) * srcPitch;
        for (uint32_t x = 0; x < w; x++) {
          uint32_t argb = toArgb(m_desc.format, row + size_t(x) * bpp, m_palette.data());
          if (keyed && (argb & 0x00ffffffu) == (m_colorKey & 0x00ffffffu))
            argb = 0;
          converted[(size_t(z) * h + y) * w + x] = argb;
        }
      }
    }
    if (m_dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM || m_dxgiFormat == DXGI_FORMAT_B8G8R8X8_UNORM) {
      write(subresource, box, converted.data(), w * 4, w * h * 4);
      return;
    }
    // A direct format with a color key or alpha bit to fix: back to the format.
    std::vector<uint8_t> packed(converted.size() * bpp);
    for (size_t i = 0; i < converted.size(); i++)
      fromArgb(m_desc.format == FormatX1R5G5B5 ? FormatA1R5G5B5 : m_desc.format, converted[i], packed.data() + i * bpp);
    m_device->context()->UpdateSubresource(d3dResource(), subresource, &box, packed.data(), w * bpp, w * h * bpp);
  }

  void Texture::readBack(uint32_t subresource) {
    // Render targets: what the GPU drew, into the application's format.
    m_gpuWritten = false;
    if (!m_texture || formatInfo(m_desc.format).depth)
      return;
    ID3D11Texture2D* copy = staging(subresource);
    if (!copy)
      return;
    ID3D11Texture2D* source = m_texture.get();
    uint32_t sourceSubresource = subresource;
    if (m_desc.samples > 1) {
      // Multisampled: resolved first.
      if (!m_resolved) {
        D3D11_TEXTURE2D_DESC td;
        m_texture->GetDesc(&td);
        td.SampleDesc.Count = 1;
        td.BindFlags = 0;
        td.MiscFlags = 0;
        td.MipLevels = 1;
        td.ArraySize = 1;
        if (FAILED(m_device->d3d11()->CreateTexture2D(&td, nullptr, m_resolved.put())))
          return;
      }
      m_device->context()->ResolveSubresource(m_resolved.get(), 0, m_texture.get(), 0, m_dxgiFormat);
      source = m_resolved.get();
      sourceSubresource = 0;
    }
    for (uint32_t i = 0; i < m_levels.size(); i++) {
      Level& level = m_levels[i];
      if (level.data.empty())
        level.data.resize(levelSize(i));
      if (i != subresource)
        continue;
      m_device->context()->CopySubresourceRegion(copy, 0, 0, 0, 0, source, sourceSubresource, nullptr);
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      if (FAILED(m_device->context()->Map(copy, 0, D3D11_MAP_READ, 0, &mapped)))
        return;
      const uint32_t w = width(i), h = height(i), dstPitch = pitch(i);
      const uint32_t bpp = formatInfo(m_desc.format).bytesPerBlock;
      for (uint32_t y = 0; y < h; y++) {
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
        uint8_t* dst = level.data.data() + size_t(y) * dstPitch;
        if (!m_convert) {
          std::memcpy(dst, src, size_t(w) * bpp);
          if (m_desc.format == FormatA2R10G10B10)
            swapRedBlue1010102(dst, w);
        } else {
          for (uint32_t x = 0; x < w; x++) {
            uint32_t argb;
            std::memcpy(&argb, src + x * 4, 4);
            fromArgb(m_desc.format, argb, dst + size_t(x) * bpp);
          }
        }
      }
      m_device->context()->Unmap(copy, 0);
    }
    // Other subresources stay as they were; they are read back when locked.
    m_gpuWritten = m_levels.size() > 1;
  }

  void Texture::setPalette(const uint32_t* entries) {
    if (entries)
      std::memcpy(m_palette.data(), entries, sizeof(uint32_t) * 256);
    if (m_desc.format == FormatP8) {
      for (auto& level : m_levels)
        if (!level.data.empty()) level.dirty = true;
      markPending();
    }
  }

  void Texture::setColorKey(bool enabled, uint32_t key) {
    if (m_colorKeyEnabled == enabled && m_colorKey == key)
      return;
    m_colorKeyEnabled = enabled;
    m_colorKey = key;
    for (auto& level : m_levels)
      if (!level.data.empty()) level.dirty = true;
    markPending();
  }

  // ============================================================
  // Buffer
  // ============================================================

  Buffer::Buffer(Device* device, uint32_t size, bool index, bool dynamic)
  : m_device(device), m_data(size), m_dynamic(dynamic), m_declaredDynamic(dynamic) {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (size + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = index ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
    m_device->d3d11()->CreateBuffer(&bd, nullptr, m_buffer.put());
  }

  Buffer::~Buffer() = default;

  bool Buffer::indexRange(uint32_t start, uint32_t count, bool index32, uint32_t& lo, uint32_t& hi) {
    const uint32_t size = index32 ? 4 : 2;
    if (!count || (uint64_t(start) + count) * size > m_data.size())
      return false;
    const uint64_t key = (uint64_t(start) << 33) ^ (uint64_t(count) << 1) ^ (index32 ? 1u : 0u);
    auto it = m_indexRanges.find(key);
    if (it != m_indexRanges.end() && it->second.version == m_version) {
      lo = it->second.lo;
      hi = it->second.hi;
      return true;
    }
    lo = UINT32_MAX;
    hi = 0;
    if (index32) {
      const auto* indices = reinterpret_cast<const uint32_t*>(m_data.data()) + start;
      for (uint32_t i = 0; i < count; i++) {
        lo = (std::min)(lo, indices[i]);
        hi = (std::max)(hi, indices[i]);
      }
    } else {
      const auto* indices = reinterpret_cast<const uint16_t*>(m_data.data()) + start;
      for (uint32_t i = 0; i < count; i++) {
        lo = (std::min)(lo, uint32_t(indices[i]));
        hi = (std::max)(hi, uint32_t(indices[i]));
      }
    }
    if (m_indexRanges.size() >= 4096)
      m_indexRanges.clear();
    m_indexRanges[key] = { m_version, lo, hi };
    return true;
  }

  void Buffer::invalidate(uint32_t offset, uint32_t size) {
    m_version++;
    // Written again and again (vertices of a frame, without a dynamic flag):
    // streamed like a dynamic buffer from now on.
    m_drawsSinceWrite = 0;
    if (!m_dynamic && !m_settled && ++m_writes >= 4)
      m_dynamic = true;
    if (!size) {
      m_clean.clear();
      return;
    }
    const uint32_t end = offset + size;
    auto& remaining = m_scratch;
    remaining.clear();
    for (const auto& range : m_clean) {
      if (range.second <= offset || range.first >= end) {
        remaining.push_back(range);
        continue;
      }
      if (range.first < offset) remaining.push_back({ range.first, offset });
      if (range.second > end) remaining.push_back({ end, range.second });
    }
    m_clean.swap(remaining);
  }

  void Buffer::prepare(uint32_t offset, uint32_t size) {
    if (!m_buffer)
      return;
    const uint32_t total = uint32_t(m_data.size());
    uint32_t begin = (std::min)(offset, total);
    const uint32_t end = (std::min)(size ? offset + size : total, total);
    if (begin >= end)
      return;
    // Already clean (the usual draw): nothing to upload or merge.
    for (const auto& range : m_clean) {
      if (range.first > begin)
        break;
      if (range.second >= end)
        return;
    }
    auto upload = [&](uint32_t from, uint32_t to) {
      D3D11_BOX box = { from, 0, 0, to, 1, 1 };
      m_device->context()->UpdateSubresource(m_buffer.get(), 0, &box, m_data.data() + from, 0, 0);
    };
    // Upload the gaps between clean ranges inside [begin, end).
    for (const auto& range : m_clean) {
      if (range.second <= begin)
        continue;
      if (range.first >= end)
        break;
      if (range.first > begin)
        upload(begin, range.first);
      begin = (std::max)(begin, range.second);
      if (begin >= end)
        break;
    }
    if (begin < end)
      upload(begin, end);
    // [offset, end) is clean now: merge it in.
    const uint32_t from = (std::min)(offset, total);
    auto& merged = m_scratch;
    merged.clear();
    std::pair<uint32_t, uint32_t> added = { from, end };
    bool placed = false;
    for (const auto& range : m_clean) {
      if (range.second < added.first) {
        merged.push_back(range);
      } else if (range.first > added.second) {
        if (!placed) { merged.push_back(added); placed = true; }
        merged.push_back(range);
      } else {
        added.first = (std::min)(added.first, range.first);
        added.second = (std::max)(added.second, range.second);
      }
    }
    if (!placed)
      merged.push_back(added);
    m_clean.swap(merged);
  }

  // ============================================================
  // Vertex declarations
  // ============================================================

  namespace {
    uint32_t declComponents(uint8_t type) {
      switch (type) {
        case DeclFloat1:   return 1;
        case DeclFloat2: case DeclShort2: case DeclShort2N: case DeclUShort2N: case DeclFloat16x2: return 2;
        case DeclFloat3: case DeclUDec3: case DeclDec3N: return 3;
        default:           return 4;
      }
    }

    DXGI_FORMAT declFormat(uint8_t type) {
      switch (type) {
        case DeclFloat1:    return DXGI_FORMAT_R32_FLOAT;
        case DeclFloat2:    return DXGI_FORMAT_R32G32_FLOAT;
        case DeclFloat3:    return DXGI_FORMAT_R32G32B32_FLOAT;
        case DeclFloat4:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DeclD3DColor:  return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DeclUByte4:    return DXGI_FORMAT_R8G8B8A8_UINT;
        case DeclShort2:    return DXGI_FORMAT_R16G16_SINT;
        case DeclShort4:    return DXGI_FORMAT_R16G16B16A16_SINT;
        case DeclUByte4N:   return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DeclShort2N:   return DXGI_FORMAT_R16G16_SNORM;
        case DeclShort4N:   return DXGI_FORMAT_R16G16B16A16_SNORM;
        case DeclUShort2N:  return DXGI_FORMAT_R16G16_UNORM;
        case DeclUShort4N:  return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DeclUDec3:
        case DeclDec3N:     return DXGI_FORMAT_R10G10B10A2_UINT;   // w = 1, DEC3N signed: in the shader
        case DeclFloat16x2: return DXGI_FORMAT_R16G16_FLOAT;
        case DeclFloat16x4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:            return DXGI_FORMAT_UNKNOWN;
      }
    }

    bool declUint(uint8_t type) { return type == DeclUByte4 || type == DeclUDec3; }
    bool declSint(uint8_t type) { return type == DeclShort2 || type == DeclShort4; }
    // Read as w = 1 (Direct3D 9's (x, y, z, 1)).
    bool declWOne(uint8_t type) { return type == DeclUDec3 || type == DeclDec3N; }

    // D3DDECLUSAGE values the fixed function reads.
    enum : uint8_t { UsePosition = 0, UseBlendWeight = 1, UseBlendIndices = 2, UseNormal = 3, UsePSize = 4,
                     UseTexCoord = 5, UsePositionT = 9, UseColor = 10 };

    // Stream frequency flags (D3DSTREAMSOURCE_*).
    constexpr uint32_t kIndexedData = 1u << 30;
    constexpr uint32_t kInstanceData = 2u << 30;
  }

  std::vector<VertexElement> VertexDecl::elementsOf(uint32_t fvf) {
    std::vector<VertexElement> elements;
    uint16_t offset = 0;
    auto add = [&](uint8_t type, uint8_t usage, uint8_t index, uint16_t size) {
      elements.push_back({ 0, offset, type, 0, usage, index });
      offset = uint16_t(offset + size);
    };
    const uint32_t position = fvf & FvfPositionMask;
    const uint32_t weights = blendWeightCount(fvf);
    if (position == FvfXyzRhw)
      add(DeclFloat4, UsePositionT, 0, 16);
    else if (position == FvfXyzW)
      add(DeclFloat4, UsePosition, 0, 16);
    else if (position)
      add(DeclFloat3, UsePosition, 0, 12);
    if (weights) {
      const bool lastIsIndices = (fvf & (FvfLastBetaUByte4 | FvfLastBetaD3DColor)) != 0;
      const uint32_t floatCount = lastIsIndices ? weights - 1 : weights;
      if (floatCount)
        add(uint8_t(DeclFloat1 + (std::min)(floatCount, 4u) - 1), UseBlendWeight, 0, uint16_t(4 * floatCount));
      if (lastIsIndices)
        add((fvf & FvfLastBetaUByte4) ? DeclUByte4 : DeclD3DColor, UseBlendIndices, 0, 4);
    }
    if (fvf & FvfNormal)   add(DeclFloat3, UseNormal, 0, 12);
    if (fvf & FvfPSize)    add(DeclFloat1, UsePSize, 0, 4);
    if (fvf & FvfDiffuse)  add(DeclD3DColor, UseColor, 0, 4);
    if (fvf & FvfSpecular) add(DeclD3DColor, UseColor, 1, 4);
    const uint32_t texCount = (std::min)((fvf & FvfTexCountMask) >> FvfTexCountShift, 8u);
    for (uint32_t i = 0; i < texCount; i++) {
      const uint32_t size = texCoordSize(fvf, i);
      add(uint8_t(DeclFloat1 + size - 1), UseTexCoord, uint8_t(i), uint16_t(4 * size));
    }
    return elements;
  }

  VertexDecl::VertexDecl(const VertexElement* elements, uint32_t count, uint32_t fvf)
  : m_fvf(fvf) {
    for (uint32_t i = 0; i < count; i++) {
      if (elements[i].stream == 0xff || elements[i].type == DeclUnused)
        break;
      m_elements.push_back(elements[i]);
    }
    uint64_t hash = 14695981039346656037ull;
    for (const auto& e : m_elements) {
      const uint64_t word = uint64_t(e.stream) | (uint64_t(e.offset) << 8) | (uint64_t(e.type) << 24)
        | (uint64_t(e.usage) << 32) | (uint64_t(e.usageIndex) << 40);
      hash = fnv64Multiply(hash ^ word);
      m_streamMask |= 1u << (e.stream & 15);
    }
    m_hash = hash;

    // The fixed function's view: an equivalent flexible vertex format.
    if (fvf) {
      m_ffpFvf = fvf;
      return;
    }
    uint32_t ffp = 0;
    uint32_t weightCount = 0;
    uint32_t texCount = 0;
    uint32_t texSizes = 0;
    for (const auto& e : m_elements) {
      switch (e.usage) {
        case UsePositionT: if (e.usageIndex == 0) ffp = (ffp & ~FvfPositionMask) | FvfXyzRhw; break;
        case UsePosition:
          if (e.usageIndex == 0 && (ffp & FvfPositionMask) != FvfXyzRhw)
            ffp = (ffp & ~FvfPositionMask) | (e.type == DeclFloat4 ? FvfXyzW : FvfXyz);
          break;
        case UseBlendWeight: if (e.usageIndex == 0) weightCount = declComponents(e.type); break;
        case UseNormal: if (e.usageIndex == 0) ffp |= FvfNormal; break;
        case UseColor: if (e.usageIndex == 0) ffp |= FvfDiffuse; else if (e.usageIndex == 1) ffp |= FvfSpecular; break;
        case UseTexCoord:
          if (e.usageIndex < 8) {
            texCount = (std::max)(texCount, uint32_t(e.usageIndex) + 1);
            // D3DFVF_TEXCOORDSIZE: 1 -> 3, 2 -> 0, 3 -> 1, 4 -> 2.
            static const uint32_t codes[5] = { 0, 3, 0, 1, 2 };
            texSizes |= codes[declComponents(e.type)] << (16 + e.usageIndex * 2);
          }
          break;
        default: break;
      }
    }
    if (weightCount && (ffp & FvfPositionMask) == FvfXyz)
      ffp = (ffp & ~FvfPositionMask) | (0x006u + 2u * ((std::min)(weightCount, 4u) - 1u));
    m_ffpFvf = ffp | (texCount << FvfTexCountShift) | texSizes;
  }

  // ============================================================
  // Shaders
  // ============================================================

  namespace {
    using CompileFunction = HRESULT (WINAPI*)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*,
      const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);

    CompileFunction compiler() {
      static CompileFunction function = [] {
        HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
        return module ? reinterpret_cast<CompileFunction>(GetProcAddress(module, "D3DCompile")) : nullptr;
      }();
      return function;
    }

    // dxup_pso_cache next to the DLL (created), empty when unknown.
    std::wstring cacheDirectory() {
      HMODULE self = nullptr;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&compiler), &self);
      wchar_t path[MAX_PATH] = {};
      if (!self || !GetModuleFileNameW(self, path, MAX_PATH))
        return std::wstring();
      wchar_t* name = wcsrchr(path, L'\\');
      if (!name)
        return std::wstring();
      name[1] = 0;
      std::wstring dir = std::wstring(path) + L"dxup_pso_cache";
      CreateDirectoryW(dir.c_str(), nullptr);
      return dir;
    }

    // Translated and compiled shaders of this and earlier runs, by the hash
    // of bytecode and variant: dxup_pso_cache\d3d9_shaders.bin next to the
    // DLL, records of hash, size and DXBC appended as they compile.
    class ShaderDiskCache {
    public:
      static ShaderDiskCache& instance() {
        static ShaderDiskCache cache;
        return cache;
      }

      bool find(uint64_t key, std::vector<uint8_t>& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        if (it == m_entries.end())
          return false;
        out = it->second;
        return true;
      }

      void store(uint64_t key, const void* data, size_t size) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_entries.count(key))
          return;
        m_entries[key].assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        if (m_path.empty())
          return;
        FILE* file = nullptr;
        if (_wfopen_s(&file, m_path.c_str(), L"ab") != 0)
          file = nullptr;
        if (!file)
          return;
        const uint64_t header[2] = { key, uint64_t(size) };
        fwrite(header, sizeof(header), 1, file);
        fwrite(data, 1, size, file);
        fclose(file);
      }

    private:
      ShaderDiskCache() {
        const std::wstring dir = cacheDirectory();
        if (dir.empty())
          return;
        m_path = dir + L"\\d3d9_shaders.bin";
        FILE* file = nullptr;
        if (_wfopen_s(&file, m_path.c_str(), L"rb") != 0)
          file = nullptr;
        if (!file)
          return;
        uint64_t header[2];
        while (fread(header, sizeof(header), 1, file) == 1) {
          if (header[1] > (16u << 20))
            break;
          std::vector<uint8_t> data(static_cast<size_t>(header[1]));
          if (fread(data.data(), 1, data.size(), file) != data.size())
            break;
          m_entries[header[0]] = std::move(data);
        }
        fclose(file);
      }

      std::mutex m_mutex;
      std::wstring m_path;
      std::unordered_map<uint64_t, std::vector<uint8_t>> m_entries;
    };

    // Changes whenever the translator's output changes (the HLSL path's;
    // DXUP's compiler is not cached, it is faster than reading the cache).
    constexpr uint64_t kTranslatorVersion = 8;

    // The shaders draws used in this and earlier runs, their code and the
    // variants they were drawn with: compiled at the start of a later run,
    // before the game creates them (Device::startShaderWarmup), and compiled
    // anew from their code when the translator changed with an update. A
    // variant a draw needs first (a shadow map, a clip plane, integer inputs,
    // more varyings) stalls the draw while it compiles, and so does the
    // Direct3D 12 pipeline of a shader new to the driver.
    // dxup_pso_cache\d3d9_variants.bin: entries of type, size and data
    // appended (1: hash and tokens of a shader, 2: a variant).
    class VariantRecords {
    public:
      struct Record {
        uint64_t shader;
        uint32_t minVaryings;
        uint16_t reserved0, reserved1, shadowSamplers, reserved2;
        uint8_t  clipPlanes, typed, reserved[2];
        uint8_t  types[16];
      };
      static_assert(sizeof(Record) == 40, "record layout");

      static VariantRecords& instance() {
        static VariantRecords records;
        return records;
      }

      // A variant a draw used (options as Shader::variant keys them).
      void note(uint64_t shader, const std::vector<uint32_t>& tokens, const TranslateOptions9& options) {
        Record record = {};
        record.shader = shader;
        record.minVaryings = options.minVaryings;
        record.shadowSamplers = options.shadowSamplers;
        record.clipPlanes = options.clipPlanes;
        if (options.boundSamplerTypes) {
          record.typed = 1;
          std::memcpy(record.types, options.boundSamplerTypes, sizeof(record.types));
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_known.insert(digest(record)).second)
          return;
        m_records.emplace(shader, record);
        if (!m_code.count(shader)) {
          m_code.emplace(shader, std::make_shared<std::vector<uint32_t>>(tokens));
          m_order.push_back(shader);
          std::vector<uint8_t> entry(sizeof(uint64_t) + tokens.size() * sizeof(uint32_t));
          std::memcpy(entry.data(), &shader, sizeof(shader));
          std::memcpy(entry.data() + sizeof(shader), tokens.data(), tokens.size() * sizeof(uint32_t));
          append(kCode, entry.data(), entry.size());
        }
        append(kVariant, &record, sizeof(record));
      }

      // The variants earlier draws used of a shader.
      std::vector<Record> find(uint64_t shader) {
        std::vector<Record> found;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto range = m_records.equal_range(shader);
        for (auto it = range.first; it != range.second; ++it)
          found.push_back(it->second);
        return found;
      }

      // The shaders draws used, in the order they were first drawn.
      std::vector<std::pair<uint64_t, std::shared_ptr<const std::vector<uint32_t>>>> shaders() {
        std::vector<std::pair<uint64_t, std::shared_ptr<const std::vector<uint32_t>>>> list;
        std::lock_guard<std::mutex> lock(m_mutex);
        list.reserve(m_order.size());
        for (uint64_t shader : m_order)
          list.emplace_back(shader, m_code[shader]);
        return list;
      }

      // Options of a record; the sampler types point into it.
      static void apply(const Record& record, TranslateOptions9& options) {
        options.minVaryings = record.minVaryings;
        options.shadowSamplers = record.shadowSamplers;
        options.clipPlanes = record.clipPlanes;
        options.boundSamplerTypes = record.typed ? record.types : nullptr;
      }

    private:
      static constexpr uint32_t kHeader[2] = { 0x39565844u /* "DXV9" */, 2 };
      static constexpr uint32_t kCode = 1, kVariant = 2;
      static constexpr size_t kMaxBytes = 64u << 20;
      static constexpr size_t kMaxEntry = 4u << 20;

      static uint64_t digest(const Record& record) {
        uint64_t hash = 14695981039346656037ull;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
        for (size_t i = 0; i < sizeof(record); i++)
          hash = (hash ^ bytes[i]) * 1099511628211ull;
        return hash;
      }

      // One write per entry (other processes of the game may append too).
      void append(uint32_t type, const void* data, size_t size) {
        if (m_path.empty() || m_size + size > kMaxBytes)
          return;
        std::vector<uint8_t> entry;
        if (m_size == 0)
          entry.insert(entry.end(), reinterpret_cast<const uint8_t*>(kHeader), reinterpret_cast<const uint8_t*>(kHeader) + sizeof(kHeader));
        const uint32_t head[2] = { type, uint32_t(size) };
        entry.insert(entry.end(), reinterpret_cast<const uint8_t*>(head), reinterpret_cast<const uint8_t*>(head) + sizeof(head));
        entry.insert(entry.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        FILE* file = nullptr;
        if (_wfopen_s(&file, m_path.c_str(), L"ab") != 0 || !file)
          return;
        fwrite(entry.data(), 1, entry.size(), file);
        fclose(file);
        m_size += entry.size();
      }

      VariantRecords() {
        const std::wstring dir = cacheDirectory();
        if (dir.empty())
          return;
        m_path = dir + L"\\d3d9_variants.bin";
        FILE* file = nullptr;
        if (_wfopen_s(&file, m_path.c_str(), L"rb") != 0)
          file = nullptr;
        if (!file)
          return;
        uint32_t header[2] = {};
        const bool valid = fread(header, sizeof(header), 1, file) == 1 && header[0] == kHeader[0] && header[1] == kHeader[1];
        uint32_t head[2];
        std::vector<uint8_t> data;
        while (valid && fread(head, sizeof(head), 1, file) == 1 && head[1] <= kMaxEntry) {
          data.resize(head[1]);
          if (fread(data.data(), 1, data.size(), file) != data.size())
            break;
          if (head[0] == kCode && data.size() > sizeof(uint64_t) && (data.size() - sizeof(uint64_t)) % sizeof(uint32_t) == 0) {
            uint64_t shader;
            std::memcpy(&shader, data.data(), sizeof(shader));
            if (!m_code.count(shader)) {
              auto tokens = std::make_shared<std::vector<uint32_t>>((data.size() - sizeof(uint64_t)) / sizeof(uint32_t));
              std::memcpy(tokens->data(), data.data() + sizeof(uint64_t), tokens->size() * sizeof(uint32_t));
              m_code.emplace(shader, std::move(tokens));
              m_order.push_back(shader);
            }
          } else if (head[0] == kVariant && data.size() == sizeof(Record)) {
            Record record;
            std::memcpy(&record, data.data(), sizeof(record));
            if (m_known.insert(digest(record)).second)
              m_records.emplace(record.shader, record);
          }
        }
        m_size = valid ? size_t(_ftelli64(file)) : 0;
        fclose(file);
        // Another layout: started anew.
        if (!valid)
          DeleteFileW(m_path.c_str());
      }

      std::mutex m_mutex;
      std::wstring m_path;
      size_t m_size = 0;
      std::unordered_set<uint64_t> m_known;
      std::unordered_multimap<uint64_t, Record> m_records;
      std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint32_t>>> m_code;
      std::vector<uint64_t> m_order;
    };
  }

  // The PCI vendor of the GPU DXUP renders on (DXUPGetVendorId), 0 unknown.
  std::atomic<uint32_t> g_vendorId{0};

  // Where Direct3D 9 drivers differ: as dxup.conf says, or as the driver of
  // the GPU's maker (UserOverrides::d3d9FloatEmulation, d3d9NanDepth).
  void driverBehavior(TranslateOptions9& options) {
    const UserOverrides& overrides = UserOverrides::get();
    const bool intel = g_vendorId.load(std::memory_order_relaxed) == 0x8086;
    options.floatEmulation = overrides.d3d9FloatEmulation == 1;
    options.dotEmulation = overrides.d3d9FloatEmulation != 0;
    options.nanDepthZero = overrides.d3d9NanDepth == 1 || (overrides.d3d9NanDepth < 0 && intel);
  }

  namespace {
    // Workers compiling shader variants while a game loads.
    class CompilePool {
    public:
      static CompilePool& instance() {
        static CompilePool* pool = new CompilePool();   // lives until the process ends
        return *pool;
      }
      // First: ahead of the queue (a shader the game just created, likely
      // drawn next; loading screens queue hundreds before it).
      void post(std::function<void()> task, bool first = false) {
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          if (first)
            m_tasks.push_front(std::move(task));
          else
            m_tasks.push_back(std::move(task));
        }
        m_wake.notify_one();
      }
    private:
      CompilePool() {
        const unsigned threads = (std::max)(1u, (std::min)(8u, std::thread::hardware_concurrency() / 2));
        for (unsigned i = 0; i < threads; i++)
          Thread([this] { run(); }).detach();
      }
      void run() {
        SetThreadDescription(GetCurrentThread(), L"dxup-d3d9-compile");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
          std::function<void()> task;
          {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return !m_tasks.empty(); });
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
          }
          task();
        }
      }
      std::mutex m_mutex;
      std::condition_variable m_wake;
      std::deque<std::function<void()>> m_tasks;
    };
  }

  std::shared_ptr<Shader> Shader::create(Device* device, const uint32_t* tokens, size_t tokenCount, std::string& error) {
    if (!tokens || tokenCount < 2) {
      error = "empty shader";
      return nullptr;
    }
    // The token stream ends with the end token.
    size_t count = 1;
    while (count < tokenCount && tokens[count] != 0x0000ffffu) {
      const uint32_t token = tokens[count];
      if ((token & 0xffff) == 0xfffe)
        count += (token >> 16) & 0x7fff;
      count++;
    }
    count = (std::min)(count + 1, tokenCount);
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < count; i++)
      hash = fnv64Multiply(hash ^ tokens[i]);
    TranslateOptions9 options;
    driverBehavior(options);
    // Drawn in an earlier run: compiled since the device was made.
    std::shared_ptr<Shader> shader = device->takeWarmShader(hash, tokens, count);
    if (!shader)
      shader = make(device->d3d11(), tokens, count, hash, error);
    if (!shader)
      return nullptr;
    // The likely variants now: compiling at the first draw would stall it.
    // DXUP's compiler takes a fraction of a millisecond, as Direct3D 9
    // drivers compile when the shader is created; fxc runs on workers
    // (loading screens create shaders by the hundred).
    const bool now = UserOverrides::get().d3d9DxupCompiler;
    auto ahead = [&](const TranslateOptions9& o, bool first) {
      if (now)
        shader->variant(o, false);
      else
        shader->prepare(o, first);
    };
    ahead(options, true);
    // Vertex shaders 1.x, and any once the game drew them so, also for the
    // fixed-function pixel stage.
    if (shader->vertex() && shader->m_info.varyingCount < Device::kFixedPixelVaryings
     && (shader->m_info.major == 1 || device->fixedPixelsWithShaders())) {
      TranslateOptions9 fixed = options;
      fixed.minVaryings = Device::kFixedPixelVaryings;
      ahead(fixed, false);
    }
    // And every variant draws used in earlier runs.
    for (const VariantRecords::Record& record : VariantRecords::instance().find(hash)) {
      TranslateOptions9 recorded = options;
      VariantRecords::apply(record, recorded);
      ahead(recorded, false);
    }
    return shader;
  }

  std::shared_ptr<Shader> Shader::make(ID3D11Device* d3d11, const uint32_t* tokens, size_t count, uint64_t hash,
                                       std::string& error) {
    auto shader = std::make_shared<Shader>();
    d3d11->AddRef();
    *shader->m_d3d11.put() = d3d11;
    shader->m_tokens.assign(tokens, tokens + count);
    shader->m_hash = hash;
    std::string hlsl;
    TranslateOptions9 options;
    driverBehavior(options);
    if (!translateShader9(shader->m_tokens.data(), shader->m_tokens.size(), options, hlsl, shader->m_info, error))
      return nullptr;
    return shader;
  }

  // ---- Shaders compiled ahead ----

  struct Device::ShaderWarmup {
    ID3D11Device*           d3d11 = nullptr;   // the device's, used while active
    std::mutex              mutex;
    std::condition_variable changed;
    std::unordered_set<uint64_t> queued;      // not yet started
    std::unordered_set<uint64_t> building;    // being made
    std::unordered_map<uint64_t, std::shared_ptr<Shader>> ready;   // not yet created by the game
    uint32_t                active = 0;
    std::atomic<bool>       cancelled{false};
  };

  void Device::startShaderWarmup() {
    auto shaders = VariantRecords::instance().shaders();
    if (shaders.empty())
      return;
    // At most what the address space of a 32-bit game affords.
    const size_t limit = sizeof(void*) == 4 ? 2048 : 16384;
    if (shaders.size() > limit)
      shaders.resize(limit);
    m_shaderWarmup = std::make_shared<ShaderWarmup>();
    m_shaderWarmup->d3d11 = m_device.get();
    for (const auto& entry : shaders)
      m_shaderWarmup->queued.insert(entry.first);
    logMessage(LogLevel::Info, "D3D9 shaders: compiling %zu shaders of earlier runs ahead", shaders.size());
    std::weak_ptr<ShaderWarmup> weak = m_shaderWarmup;
    for (const auto& entry : shaders) {
      const uint64_t hash = entry.first;
      std::shared_ptr<const std::vector<uint32_t>> tokens = entry.second;
      CompilePool::instance().post([weak, hash, tokens] {
        auto warmup = weak.lock();
        if (!warmup)
          return;
        {
          std::lock_guard<std::mutex> lock(warmup->mutex);
          if (warmup->cancelled || !warmup->queued.erase(hash))
            return;
          warmup->building.insert(hash);
          warmup->active++;
        }
        std::string error;
        std::shared_ptr<Shader> shader = Shader::make(warmup->d3d11, tokens->data(), tokens->size(), hash, error);
        {
          std::lock_guard<std::mutex> lock(warmup->mutex);
          warmup->building.erase(hash);
          if (shader && !warmup->cancelled)
            warmup->ready.emplace(hash, shader);
        }
        warmup->changed.notify_all();
        // Its variants: what the game's creation compiles ahead, and what
        // draws used.
        if (shader) {
          TranslateOptions9 options;
          driverBehavior(options);
          shader->variant(options, false);
          for (const VariantRecords::Record& record : VariantRecords::instance().find(hash)) {
            if (warmup->cancelled)
              break;
            TranslateOptions9 recorded = options;
            VariantRecords::apply(record, recorded);
            shader->variant(recorded, false);
          }
          shader.reset();
        }
        {
          std::lock_guard<std::mutex> lock(warmup->mutex);
          warmup->active--;
        }
        warmup->changed.notify_all();
      });
    }
  }

  std::shared_ptr<Shader> Device::takeWarmShader(uint64_t hash, const uint32_t* tokens, size_t count) {
    if (!m_shaderWarmup)
      return nullptr;
    ShaderWarmup& warmup = *m_shaderWarmup;
    std::unique_lock<std::mutex> lock(warmup.mutex);
    const bool queued = warmup.queued.erase(hash) != 0;
    warmup.changed.wait(lock, [&] { return !warmup.building.count(hash); });
    auto it = warmup.ready.find(hash);
    if (it == warmup.ready.end()) {
      if (queued)
        logMessage(LogLevel::Info, "D3D9 shaders: shader %016llx created before its compile ahead started", (unsigned long long)hash);
      return nullptr;
    }
    std::shared_ptr<Shader> shader = std::move(it->second);
    warmup.ready.erase(it);
    lock.unlock();
    if (shader->tokens().size() != count || std::memcmp(shader->tokens().data(), tokens, count * sizeof(uint32_t)) != 0)
      return nullptr;
    shader->m_ahead = true;
    return shader;
  }

  void Device::stopShaderWarmup() {
    if (!m_shaderWarmup)
      return;
    std::unordered_map<uint64_t, std::shared_ptr<Shader>> ready;
    {
      std::unique_lock<std::mutex> lock(m_shaderWarmup->mutex);
      m_shaderWarmup->cancelled = true;
      m_shaderWarmup->queued.clear();
      ready.swap(m_shaderWarmup->ready);
      // Compiles in progress use the device; a process that exits has
      // ended its workers.
      if (!processExiting())
        m_shaderWarmup->changed.wait(lock, [&] { return m_shaderWarmup->active == 0; });
    }
    ready.clear();
    m_shaderWarmup.reset();
  }

  void Shader::prepare(const TranslateOptions9& options, bool first) {
    TranslateOptions9 copy = options;
    std::array<uint8_t, 16> types = {};
    const bool typed = options.boundSamplerTypes != nullptr;
    if (typed)
      std::memcpy(types.data(), options.boundSamplerTypes, types.size());
    copy.boundSamplerTypes = nullptr;
    std::weak_ptr<Shader> self = shared_from_this();
    CompilePool::instance().post([self, copy, types, typed]() mutable {
      if (typed)
        copy.boundSamplerTypes = types.data();
      if (auto shader = self.lock())
        shader->variant(copy, false);
    }, first);
  }

  const Shader::Variant* Shader::variant(const TranslateOptions9& in, bool draw) {
    // A vertex shader writes its own varyings at least: the same variant.
    TranslateOptions9 options = in;
    driverBehavior(options);
    if (vertex())
      options.minVaryings = (std::max)(options.minVaryings, m_info.varyingCount);
    else
      options.minVaryings = 0;
    // Only the samplers the shader reads tell variants apart; 2D textures
    // are what it assumes without bound types (the variant compiled ahead).
    uint16_t usedSamplers = 0;
    for (uint32_t i = 0; i < 16; i++)
      if (m_info.samplerTypes[i]) usedSamplers |= uint16_t(1u << i);
    options.shadowSamplers &= usedSamplers;
    uint8_t usedTypes[16] = {};
    if (options.boundSamplerTypes) {
      bool other = false;
      for (uint32_t i = 0; i < 16; i++) {
        usedTypes[i] = (usedSamplers >> i) & 1 ? options.boundSamplerTypes[i] : 0;
        other |= usedTypes[i] && usedTypes[i] != Sampler2D;
      }
      options.boundSamplerTypes = other ? usedTypes : nullptr;
    }
    uint64_t key = kTranslatorVersion;
    key = fnv64Multiply(key ^ options.minVaryings);
    key = fnv64Multiply(key ^ options.shadowSamplers);
    key = fnv64Multiply(key ^ (uint32_t(options.clipPlanes) << 16));
    key = fnv64Multiply(key ^ ((options.floatEmulation ? 0x1000000u : 0u) | (options.dotEmulation ? 0x2000000u : 0u)
      | (options.nanDepthZero ? 0x4000000u : 0u)));
    if (options.boundSamplerTypes)
      for (int i = 0; i < 16; i++)
        key = fnv64Multiply(key ^ (options.boundSamplerTypes[i] + 0x100u * uint32_t(i)));
    std::shared_ptr<Slot> slot;
    bool firstDraw = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      auto& entry = m_variants[key];
      if (!entry)
        entry = std::make_shared<Slot>();
      slot = entry;
      if (draw && !slot->drawn)
        slot->drawn = firstDraw = true;
    }
    if (firstDraw)
      if (!g_selfTesting.load(std::memory_order_relaxed))
        VariantRecords::instance().note(m_hash, m_tokens, options);
    // Draws that compile or wait log it (info log): the stutter of shaders.
    const bool timed = firstDraw && logLevelEnabled(LogLevel::Info);
    const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    bool compiledHere = false;
    std::call_once(slot->once, [&] { compiledHere = true; slot->ok = compile(options, key, slot->variant); });
    if (timed) {
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      if (compiledHere) {
        size_t variants = 0;
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          variants = m_variants.size() - 1;
        }
        logMessage(LogLevel::Info, "D3D9 shaders: a draw compiled %s shader %016llx's variant %016llx (%.1f ms; shader %s, %zu other variants;"
          " varyings %u/%u, shadow %x, clip %x, types %d)",
          vertex() ? "vertex" : "pixel", (unsigned long long)m_hash, (unsigned long long)key, ms,
          m_ahead ? "compiled ahead" : "new", variants, options.minVaryings, m_info.varyingCount,
          options.shadowSamplers, options.clipPlanes, options.boundSamplerTypes ? 1 : 0);
      }
      else if (ms >= 1.0)
        logMessage(LogLevel::Info, "D3D9 shaders: a draw waited %.1f ms for %s shader %016llx's variant %016llx",
          ms, vertex() ? "vertex" : "pixel", (unsigned long long)m_hash, (unsigned long long)key);
    }
    return slot->ok ? &slot->variant : nullptr;
  }

  bool Shader::compile(const TranslateOptions9& options, uint64_t key, Variant& v) {
    static std::atomic<uint32_t> s_nextId{1};
    v.id = s_nextId++;
    std::string hlsl, error;
    // DXUP's compiler: DXBC straight from the tokens (d3d9_dxbc.cpp).
    if (UserOverrides::get().d3d9DxupCompiler) {
      if (compileShader9(m_tokens.data(), m_tokens.size(), options, v.bytecode, v.info, error))
        return createVariant(options, v);
      logMessage(LogLevel::Info, "D3D9 shaders: %s shader %016llx through fxc: %s", vertex() ? "vertex" : "pixel",
        (unsigned long long)m_hash, error.c_str());
      error.clear();
      v.bytecode.clear();
    }
    if (!translateShader9(m_tokens.data(), m_tokens.size(), options, hlsl, v.info, error)) {
      logMessage(LogLevel::Warn, "D3D9 shader translation failed: %s", error.c_str());
      return false;
    }
    const uint64_t cacheKey = fnv64Multiply(m_hash ^ key);
    if (!ShaderDiskCache::instance().find(cacheKey, v.bytecode)) {
      CompileFunction compileHlsl = compiler();
      if (!compileHlsl) {
        logMessage(LogLevel::Warn, "D3D9 shaders: d3dcompiler_47.dll missing");
        return false;
      }
      ID3DBlob* code = nullptr;
      ID3DBlob* errors = nullptr;
      const HRESULT hr = compileHlsl(hlsl.data(), hlsl.size(), "d3d9", nullptr, nullptr, "main", vertex() ? "vs_5_0" : "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
      if (FAILED(hr)) {
        logMessage(LogLevel::Warn, "D3D9 shader compile failed: %s\n%s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "",
          hlsl.c_str());
        if (errors) errors->Release();
        return false;
      }
      v.bytecode.assign(static_cast<const uint8_t*>(code->GetBufferPointer()),
        static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
      code->Release();
      if (errors) errors->Release();
      ShaderDiskCache::instance().store(cacheKey, v.bytecode.data(), v.bytecode.size());
    }
    return createVariant(options, v);
  }

  bool Shader::createVariant(const TranslateOptions9& options, Variant& v) {
    HRESULT hr;
    if (vertex())
      hr = m_d3d11->CreateVertexShader(v.bytecode.data(), v.bytecode.size(), nullptr, v.vs.put());
    else
      hr = m_d3d11->CreatePixelShader(v.bytecode.data(), v.bytecode.size(), nullptr, v.ps.put());
    if (FAILED(hr))
      return false;
    v.clipPlanes = options.clipPlanes;
    if (!vertex())
      v.bytecode.clear();
    return true;
  }

  // ============================================================
  // Device
  // ============================================================

  struct Device::ShaderFixedConstants {
    float    alphaTest[4];   // reference, function, enabled
    float    fogColor[4];
    float    fogParams[4];   // start, end, density, 1 / (end - start)
    uint32_t flags[4];       // fog enabled, fog table mode, alpha test (comparisons passing | 0x10)
    float    bumpMatrix[8][4];
    float    bumpLuminance[8][4];
  };

  struct Device::DrawArgs {
    PrimitiveType type = PrimitiveTriangleList;
    bool        indexed = false;
    uint32_t    startVertex = 0;     // not indexed
    uint32_t    vertexCount = 0;     // not indexed: drawn; indexed: the range indices reach
    int32_t     baseVertex = 0;      // indexed
    uint32_t    minIndex = 0;        // indexed
    uint32_t    startIndex = 0;
    uint32_t    indexCount = 0;
    const void* userVertices = nullptr;   // stream 0 from application memory
    uint32_t    userStride = 0;
    const void* userIndices = nullptr;    // indices from application memory
    bool        userIndex32 = false;
  };

  namespace {
    // The default stream: elements a declaration leaves out.
    constexpr UINT kDefaultStream = kMaxStreams;
    constexpr UINT kDefaultZeroOffset = 64;
  }

  std::unique_ptr<Device> Device::create(HWND window, std::string& error) {
    std::unique_ptr<Device> device(new Device());
    if (!device->init(window, error))
      return nullptr;
    return device;
  }

  Device::~Device() {
    stopShaderWarmup();
    if (m_context)
      m_context->ClearState();
    for (auto& entry : m_inputLayouts) if (entry.second) entry.second->Release();
    for (auto& entry : m_blendStates) if (entry.second) entry.second->Release();
    for (auto& entry : m_depthStates) if (entry.second) entry.second->Release();
    for (auto& entry : m_rasterStates) if (entry.second) entry.second->Release();
    for (auto& entry : m_samplerStates) if (entry.second) entry.second->Release();
    for (auto& entry : m_pointShaders) if (entry.second) entry.second->Release();
  }

  bool glitchReportsEnabled();
  std::atomic<bool> g_selfTesting{false};

  HRESULT Device::createShaderResourceView(ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, UINT mapping,
                                           ID3D11ShaderResourceView** view) {
    if (mapping && m_createMappedView)
      return m_createMappedView(m_device.get(), resource, desc, mapping, view);
    return m_device->CreateShaderResourceView(resource, desc, view);
  }

  void Device::noteBackBufferSize(uint32_t width, uint32_t height) {
    if (m_noteBackBufferSize && m_device)
      m_noteBackBufferSize(m_device.get(), width, height);
  }

  bool Device::init(HWND window, std::string& error) {
    (void)window;
    m_d3d11Module = loadD3D11();
    auto create = m_d3d11Module ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
      GetProcAddress(m_d3d11Module, "D3D11CreateDevice")) : nullptr;
    if (!create) {
      error = "d3d11.dll not found";
      return false;
    }
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1,
      D3D11_SDK_VERSION, m_device.put(), nullptr, m_context.put());
    if (FAILED(hr)) {
      error = "D3D11CreateDevice failed";
      return false;
    }
    m_noteBackBufferSize = reinterpret_cast<decltype(m_noteBackBufferSize)>(
      GetProcAddress(m_d3d11Module, "DXUPNoteBackBufferSize"));
    m_createMappedView = reinterpret_cast<decltype(m_createMappedView)>(
      GetProcAddress(m_d3d11Module, "DXUPCreateShaderResourceView"));
    // The GPU's maker, before the first shader is translated.
    using GetVendorId = UINT (WINAPI*)(IUnknown*);
    if (auto getVendorId = reinterpret_cast<GetVendorId>(GetProcAddress(m_d3d11Module, "DXUPGetVendorId")))
      g_vendorId.store(getVendorId(m_device.get()), std::memory_order_relaxed);
    // The shaders of earlier runs, compiled while the game loads.
    startShaderWarmup();
    // Constant buffers bound with offsets (Direct3D 11.1).
    if (FAILED(m_context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(m_context1.put())))) {
      error = "no ID3D11DeviceContext1";
      return false;
    }
    {
      Ref<IDXGIDevice> dxgiDevice;
      Ref<IDXGIAdapter> adapter;
      if (FAILED(m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.put())))
       || FAILED(dxgiDevice->GetAdapter(adapter.put()))
       || FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(m_factory.put())))) {
        error = "no DXGI factory";
        return false;
      }
    }

    hr = m_device->CreateVertexShader(g_ffp_vs, sizeof(g_ffp_vs), nullptr, m_ffpVs.put());
    if (SUCCEEDED(hr)) hr = m_device->CreatePixelShader(g_ffp_ps, sizeof(g_ffp_ps), nullptr, m_ffpPs.put());
    if (SUCCEEDED(hr)) hr = m_device->CreateVertexShader(g_quad_vs, sizeof(g_quad_vs), nullptr, m_quadVs.put());
    if (SUCCEEDED(hr)) hr = m_device->CreatePixelShader(g_clear_ps, sizeof(g_clear_ps), nullptr, m_clearPs.put());
    if (SUCCEEDED(hr)) hr = m_device->CreatePixelShader(g_blit_ps, sizeof(g_blit_ps), nullptr, m_blitPs.put());
    if (SUCCEEDED(hr)) hr = m_device->CreatePixelShader(g_gamma_ps, sizeof(g_gamma_ps), nullptr, m_gammaPs.put());
    if (FAILED(hr)) {
      error = "shader creation failed";
      return false;
    }

    auto constantBuffer = [&](UINT size, Ref<ID3D11Buffer>& buffer) {
      D3D11_BUFFER_DESC bd = {};
      bd.ByteWidth = (size + 63) & ~63u;   // written in whole lines
      bd.Usage = D3D11_USAGE_DYNAMIC;
      bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      return SUCCEEDED(m_device->CreateBuffer(&bd, nullptr, buffer.put()));
    };
    auto ringBuffer = [&](UINT size, UINT bind, Ref<ID3D11Buffer>& buffer) {
      D3D11_BUFFER_DESC bd = {};
      bd.ByteWidth = size;
      bd.Usage = D3D11_USAGE_DYNAMIC;
      bd.BindFlags = bind;
      bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      return SUCCEEDED(m_device->CreateBuffer(&bd, nullptr, buffer.put()));
    };
    m_vertexRingSize = 8u << 20;
    m_indexRingSize = 2u << 20;
    if (!constantBuffer(sizeof(VertexConstants), m_vertexConstants)
     || !constantBuffer(sizeof(TransformConstants), m_transformConstants)
     || !constantBuffer(sizeof(PixelConstants), m_pixelConstants)
     || !constantBuffer(sizeof(QuadConstants), m_quadConstants)
     || !constantBuffer(kConstantRingSize, m_constantRings[0])
     || !constantBuffer(kConstantRingSize, m_constantRings[1])
     || !constantBuffer(sizeof(ConstantStore) - offsetof(ConstantStore, i), m_shaderIntConstants[0])
     || !constantBuffer(sizeof(ConstantStore) - offsetof(ConstantStore, i), m_shaderIntConstants[1])
     || !constantBuffer(sizeof(ShaderFixedConstants), m_shaderFixedConstants)
     || !constantBuffer(sizeof(m_clipPlanes), m_clipPlaneConstants)
     || !constantBuffer(64, m_pointConstants)
     || !ringBuffer(m_vertexRingSize, D3D11_BIND_VERTEX_BUFFER, m_vertexRing)
     || !ringBuffer(m_indexRingSize, D3D11_BIND_INDEX_BUFFER, m_indexRing)) {
      error = "buffer creation failed";
      return false;
    }
    m_vertexRingOffset = m_vertexRingSize;
    m_indexRingOffset = m_indexRingSize;

    // Elements a vertex format leaves out: weights 0, indices 0, normal 0,
    // diffuse white, specular black with alpha 1 (no fog), texcoords 0;
    // shader inputs 0 from 64 on.
    {
      uint8_t defaults[128] = {};
      const uint32_t white = 0xffffffffu, specular = 0xff000000u;
      std::memcpy(defaults + 32, &white, 4);
      std::memcpy(defaults + 36, &specular, 4);
      D3D11_BUFFER_DESC bd = { sizeof(defaults), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
      D3D11_SUBRESOURCE_DATA init = { defaults, 0, 0 };
      if (FAILED(m_device->CreateBuffer(&bd, &init, m_defaultVertex.put()))) {
        error = "buffer creation failed";
        return false;
      }
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    m_device->CreateSamplerState(&sd, m_pointSampler.put());
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    m_device->CreateSamplerState(&sd, m_linearSampler.put());
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&bd, m_opaqueBlend.put());
    D3D11_DEPTH_STENCIL_DESC dd = {};
    m_device->CreateDepthStencilState(&dd, m_noDepth.put());
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    m_device->CreateRasterizerState(&rd, m_quadRaster.put());

    resetState();
    if (glitchReportsEnabled() || logLevelEnabled(LogLevel::Info)) {
      selfTestFormats();
      selfTestVertexTypes();
      resetState();
    }
    return true;
  }

  void Device::resetState() {
    m_renderStates = {};
    m_stageStates = {};
    m_samplerStatesValues = {};
    m_textures = {};
    m_lights.clear();
    m_lightEnabled.clear();
    m_material = {};
    m_clipPlanes = {};
    m_clipPlanesDirty = true;
    m_streams = {};
    m_indices = nullptr;
    m_index32 = false;
    m_decl = nullptr;
    m_vertexShader = m_pixelShader = nullptr;
    m_vsVariant = m_psVariant = nullptr;
    std::memset(m_constants, 0, sizeof(m_constants));
    m_constants[0].limits[0] = m_constants[1].limits[0] = 255;
    m_shaderConstantsDirty[0] = m_shaderConstantsDirty[1] = true;
    m_shaderIntsDirty[0] = m_shaderIntsDirty[1] = true;
    m_renderTargets = {};
    m_renderTargetSubresources = {};
    m_depthStencil = nullptr;
    m_psTextureTypes = 0;
    m_shadowMask = 0;
    m_ffpFvf = 0;
    // Direct3D 9's default states.
    auto& rs = m_renderStates;
    rs[RsZEnable] = 0;
    rs[RsFillMode] = 3;
    rs[RsShadeMode] = 2;
    rs[RsZWriteEnable] = 1;
    rs[RsLastPixel] = 1;
    rs[RsSrcBlend] = 2;
    rs[RsDestBlend] = 1;
    rs[RsCullMode] = 3;
    rs[RsZFunc] = 4;
    rs[RsAlphaFunc] = 8;
    rs[RsFogEnd] = asUint(1.0f);
    rs[RsFogDensity] = asUint(1.0f);
    rs[RsStencilFail] = rs[RsStencilZFail] = rs[RsStencilPass] = 1;
    rs[RsStencilFunc] = 8;
    rs[RsStencilMask] = rs[RsStencilWriteMask] = 0xffffffffu;
    rs[RsTextureFactor] = 0xffffffffu;
    rs[RsClipping] = 1;
    rs[RsLighting] = 1;
    rs[RsColorVertex] = 1;
    rs[RsLocalViewer] = 1;
    rs[RsDiffuseMaterialSource] = 1;
    rs[RsSpecularMaterialSource] = 2;
    rs[RsColorWriteEnable] = rs[RsColorWriteEnable1] = rs[RsColorWriteEnable2] = rs[RsColorWriteEnable3] = 0xf;
    rs[RsBlendOp] = 1;
    rs[RsCcwStencilFail] = rs[RsCcwStencilZFail] = rs[RsCcwStencilPass] = 1;
    rs[RsCcwStencilFunc] = 8;
    rs[RsBlendFactor] = 0xffffffffu;
    rs[RsSrcBlendAlpha] = 2;
    rs[RsDestBlendAlpha] = 1;
    rs[RsBlendOpAlpha] = 1;
    rs[RsPointSize] = asUint(1.0f);
    rs[RsPointSizeMin] = asUint(1.0f);
    rs[RsPointSizeMax] = asUint(64.0f);
    rs[158] = asUint(1.0f);   // D3DRS_POINTSCALE_A
    rs[RsMultisampleAntialias] = 1;
    for (uint32_t stage = 0; stage < kFixedStages; stage++) {
      auto& tss = m_stageStates[stage];
      tss[TssColorOp] = stage ? 1 : 4;
      tss[TssAlphaOp] = stage ? 1 : 2;
      tss[TssColorArg1] = tss[TssAlphaArg1] = 2;
      tss[TssColorArg2] = tss[TssAlphaArg2] = 1;
      tss[TssColorArg0] = tss[TssAlphaArg0] = tss[TssResultArg] = 1;
      tss[TssTexCoordIndex] = stage;
      identity(m_textureTransforms[stage].data());
    }
    for (uint32_t sampler = 0; sampler < kSamplerCount; sampler++) {
      auto& ss = m_samplerStatesValues[sampler];
      ss[SampAddressU] = ss[SampAddressV] = ss[SampAddressW] = 1;
      ss[SampMagFilter] = ss[SampMinFilter] = 1;
      ss[SampMaxAnisotropy] = 1;
    }
    for (auto& world : m_world)
      identity(world.data());
    identity(m_view.data());
    identity(m_projection.data());
    m_vertexConstantsDirty = m_transformConstantsDirty = m_pixelConstantsDirty = m_shaderFixedDirty = true;
    invalidateBindings();
  }

  bool Device::supports(DXGI_FORMAT format, UINT usage) const {
    UINT support = 0;
    return SUCCEEDED(m_device->CheckFormatSupport(format, &support)) && (support & usage) == usage;
  }

  // ---- State ----

  namespace {
    // What each render state feeds: D3D11 state objects, constants.
    enum StateGroup : uint8_t {
      GroupBlend = 1, GroupDepth = 2, GroupRaster = 4, GroupStencilRef = 8, GroupVertex = 16, GroupPixel = 32,
      GroupTargets = 64,
    };

    struct StateGroups {
      uint8_t groups[RsCount] = {};
      StateGroups() {
        for (uint32_t s : { 19u, 20u, 27u, 168u, 171u, 190u, 191u, 192u, 193u, 206u, 207u, 208u, 209u }) groups[s] |= GroupBlend;
        for (uint32_t s : { 7u, 14u, 23u, 52u, 53u, 54u, 55u, 56u, 58u, 59u, 185u, 186u, 187u, 188u, 189u }) groups[s] |= GroupDepth;
        groups[57] |= GroupStencilRef;
        for (uint32_t s : { 8u, 22u, 47u, 161u, 174u, 175u, 195u }) groups[s] |= GroupRaster;
        for (uint32_t s : { 28u, 29u, 35u, 36u, 37u, 38u, 48u, 137u, 139u, 140u, 141u, 142u, 143u, 145u, 146u, 147u, 148u,
                            151u, 152u, 167u })
          groups[s] |= GroupVertex;
        for (uint32_t s : { 15u, 24u, 25u, 28u, 29u, 34u, 35u, 36u, 37u, 38u, 48u, 60u, 140u }) groups[s] |= GroupPixel;
        groups[194] |= GroupTargets;
      }
    };
    const StateGroups kStateGroups;

    uint32_t textureTypeOf(Texture* texture) {
      return texture ? texture->type() : 0u;
    }
  }

  void Device::setRenderState(uint32_t state, uint32_t value) {
    if (state >= RsCount || m_renderStates[state] == value)
      return;
    m_renderStates[state] = value;
    const uint8_t groups = kStateGroups.groups[state];
    if (groups & GroupBlend) m_dirty |= DirtyBlend;
    if (groups & GroupDepth) m_dirty |= DirtyDepth;
    if (groups & GroupRaster) m_dirty |= DirtyRaster;
    if (groups & GroupStencilRef) m_dirty |= DirtyStencilRef;
    if (groups & GroupVertex) m_vertexConstantsDirty = true;
    if (state == RsVertexBlend || state == RsIndexedVertexBlendEnable || state == RsLighting)
      m_transformConstantsDirty = true;
    if (groups & GroupPixel) m_pixelConstantsDirty = m_shaderFixedDirty = true;
    if (groups & GroupTargets) m_targetsDirty = true;
    // Shader vertex stages write the clip distances of enabled planes.
    if (state == RsClipPlaneEnable && m_vertexShader)
      m_dirty |= DirtyShaders;
  }

  void Device::setTextureStageState(uint32_t stage, uint32_t type, uint32_t value) {
    if (stage >= kFixedStages || type >= TssCount || m_stageStates[stage][type] == value)
      return;
    m_stageStates[stage][type] = value;
    m_pixelConstantsDirty = m_shaderFixedDirty = true;
    if (type == TssTexCoordIndex || type == TssTextureTransformFlags)
      m_vertexConstantsDirty = true;
    if (type == TssTexCoordIndex)
      m_transformConstantsDirty = true;
  }

  void Device::invalidateBindings() {
    m_dirty = DirtyAll;
    m_targetsDirty = true;
    m_textureDirtyMask = m_samplerDirtyMask = (1u << kSamplerCount) - 1;
    std::memset(m_boundViews, 0, sizeof(m_boundViews));
    std::memset(m_boundVertexViews, 0, sizeof(m_boundVertexViews));
    std::memset(m_boundSamplers, 0, sizeof(m_boundSamplers));
    std::memset(m_boundStreams, 0, sizeof(m_boundStreams));
    m_boundLayout = nullptr;
    m_defaultStreamBound = false;
  }

  uint32_t Device::textureStageState(uint32_t stage, uint32_t type) const {
    return stage < kFixedStages && type < TssCount ? m_stageStates[stage][type] : 0;
  }

  void Device::setSamplerState(uint32_t sampler, uint32_t type, uint32_t value) {
    if (sampler >= kSamplerCount || type >= SampCount || m_samplerStatesValues[sampler][type] == value)
      return;
    m_samplerStatesValues[sampler][type] = value;
    m_samplerDirtyMask |= 1u << sampler;
    if (type == SampSrgbTexture)
      m_textureDirtyMask |= 1u << sampler;
  }

  uint32_t Device::samplerState(uint32_t sampler, uint32_t type) const {
    return sampler < kSamplerCount && type < SampCount ? m_samplerStatesValues[sampler][type] : 0;
  }

  void Device::setTexture(uint32_t stage, Texture* texture) {
    if (stage >= kSamplerCount || m_textures[stage] == texture)
      return;
    const uint32_t oldType = textureTypeOf(m_textures[stage]), newType = textureTypeOf(texture);
    if (oldType != newType) {
      if (stage < kFixedStages)
        m_pixelConstantsDirty = true;
      // Shader model 1 pixel shaders sample with the bound textures' types.
      if (stage < kPixelSamplers) {
        m_psTextureTypes = (m_psTextureTypes & ~(3u << (stage * 2))) | (newType << (stage * 2));
        if (m_pixelShader && m_pixelShader->info().major == 1)
          m_dirty |= DirtyShaders;
      }
    }
    const bool oldShadow = m_textures[stage] && m_textures[stage]->shadow();
    const bool newShadow = texture && texture->shadow();
    if (oldShadow != newShadow && stage < kPixelSamplers) {
      m_shadowMask = newShadow ? (m_shadowMask | (1u << stage)) : (m_shadowMask & ~(1u << stage));
      if (m_pixelShader)
        m_dirty |= DirtyShaders;
      m_samplerDirtyMask |= 1u << stage;
    }
    m_textures[stage] = texture;
    m_textureDirtyMask |= 1u << stage;
    if (texture && texture->pending())
      m_texturesPending = true;
  }

  void Device::setTransform(uint32_t state, const float* matrix) {
    float* target = nullptr;
    if (state == TransformView) target = m_view.data();
    else if (state == TransformProjection) target = m_projection.data();
    else if (state >= TransformTexture0 && state < TransformTexture0 + 8) target = m_textureTransforms[state - TransformTexture0].data();
    else if (state >= TransformWorld && state < TransformWorld + 4) target = m_world[state - TransformWorld].data();
    if (!target)
      return;
    std::memcpy(target, matrix, 16 * sizeof(float));
    if (state >= TransformWorld || state == TransformView)
      m_transformConstantsDirty = true;
    // Lights are given in view space; projection and texture matrices.
    if (state < TransformWorld)
      m_vertexConstantsDirty = true;
  }

  void Device::getTransform(uint32_t state, float* matrix) const {
    const float* source = nullptr;
    if (state == TransformView) source = m_view.data();
    else if (state == TransformProjection) source = m_projection.data();
    else if (state >= TransformTexture0 && state < TransformTexture0 + 8) source = m_textureTransforms[state - TransformTexture0].data();
    else if (state >= TransformWorld && state < TransformWorld + 4) source = m_world[state - TransformWorld].data();
    if (source) std::memcpy(matrix, source, 16 * sizeof(float));
    else identity(matrix);
  }

  void Device::setMaterial(const Material& material) {
    m_material = material;
    m_vertexConstantsDirty = true;
  }

  void Device::setLight(uint32_t index, const Light& light) {
    if (index >= m_lights.size()) {
      m_lights.resize(index + 1);
      m_lightEnabled.resize(index + 1, false);
    }
    m_lights[index] = light;
    m_vertexConstantsDirty = true;
  }

  bool Device::getLight(uint32_t index, Light* light) const {
    if (index >= m_lights.size())
      return false;
    *light = m_lights[index];
    return true;
  }

  void Device::lightEnable(uint32_t index, bool enable) {
    if (index >= m_lights.size()) {
      // As Direct3D: enabling an unset light sets the default one.
      Light defaults = {};
      defaults.type = 3;
      defaults.diffuse = { 1.0f, 1.0f, 1.0f, 0.0f };
      defaults.direction = { 0.0f, 0.0f, 1.0f };
      setLight(index, defaults);
    }
    m_lightEnabled[index] = enable;
    m_vertexConstantsDirty = true;
  }

  bool Device::lightEnabled(uint32_t index) const {
    return index < m_lightEnabled.size() && m_lightEnabled[index];
  }

  void Device::setViewport(const Viewport& viewport) {
    m_viewport = viewport;
    m_targetsDirty = true;
    m_vertexConstantsDirty = true;
  }

  void Device::setScissor(const Rect& rect) {
    m_scissor = rect;
    m_targetsDirty = true;
  }

  void Device::setRenderTarget(uint32_t index, Texture* target, uint32_t subresource) {
    if (index >= kMaxRenderTargets)
      return;
    if (target && index == 0) {
      // As Direct3D 9: the first target sets viewport and scissor to itself,
      // also when it is the target already bound (UE3 games set their scene
      // target again after a smaller viewport: The Vanishing of Ethan Carter
      // drew its world into a 512x512 corner).
      m_viewport = { 0, 0, target->width(subresource), target->height(subresource), 0.0f, 1.0f };
      m_scissor = { 0, 0, int32_t(m_viewport.width), int32_t(m_viewport.height) };
      m_vertexConstantsDirty = true;
      m_targetsDirty = true;
    }
    if (m_renderTargets[index] == target && m_renderTargetSubresources[index] == subresource)
      return;
    m_renderTargets[index] = target;
    m_renderTargetSubresources[index] = subresource;
    m_targetsDirty = true;
    // A texture bound as the new target is unbound from its stage.
    m_textureDirtyMask = (1u << kSamplerCount) - 1;
    m_dirty |= DirtyBlend;
  }

  void Device::setDepthStencil(Texture* depth) {
    if (m_depthStencil == depth)
      return;
    m_depthStencil = depth;
    m_targetsDirty = true;
    m_textureDirtyMask = (1u << kSamplerCount) - 1;
    m_dirty |= DirtyDepth;
  }

  // ---- Shaders and streams ----

  void Device::setVertexShader(Shader* shader) {
    if (m_vertexShader == shader)
      return;
    m_vertexShader = shader;
    m_dirty |= DirtyShaders | DirtyLayout;
    m_shaderConstantsDirty[0] = true;
  }

  void Device::setPixelShader(Shader* shader) {
    if (m_pixelShader == shader)
      return;
    // Fixed-function and shader pixel stages bind textures differently.
    if (!m_pixelShader != !shader)
      m_textureDirtyMask = m_samplerDirtyMask = (1u << kSamplerCount) - 1;
    m_pixelShader = shader;
    m_dirty |= DirtyShaders | DirtyLayout;
    m_shaderConstantsDirty[1] = true;
  }

  void Device::setShaderConstantF(bool pixel, uint32_t start, const float* data, uint32_t count) {
    if (start >= 256 || !data)
      return;
    count = (std::min)(count, 256u - start);
    std::memcpy(m_constants[pixel].f[start], data, size_t(count) * 16);
    m_shaderConstantsDirty[pixel] = true;
  }

  void Device::setShaderConstantI(bool pixel, uint32_t start, const int32_t* data, uint32_t count) {
    if (start >= 16 || !data)
      return;
    count = (std::min)(count, 16u - start);
    std::memcpy(m_constants[pixel].i[start], data, size_t(count) * 16);
    m_shaderIntsDirty[pixel] = true;
  }

  void Device::setShaderConstantB(bool pixel, uint32_t start, const uint32_t* data, uint32_t count) {
    if (start >= 16 || !data)
      return;
    count = (std::min)(count, 16u - start);
    for (uint32_t i = 0; i < count; i++)
      m_constants[pixel].b[start + i] = data[i] ? 1u : 0u;
    m_shaderIntsDirty[pixel] = true;
  }

  void Device::getShaderConstantF(bool pixel, uint32_t start, float* data, uint32_t count) const {
    if (start >= 256 || !data)
      return;
    count = (std::min)(count, 256u - start);
    std::memcpy(data, m_constants[pixel].f[start], size_t(count) * 16);
  }

  void Device::getShaderConstantI(bool pixel, uint32_t start, int32_t* data, uint32_t count) const {
    if (start >= 16 || !data)
      return;
    count = (std::min)(count, 16u - start);
    std::memcpy(data, m_constants[pixel].i[start], size_t(count) * 16);
  }

  void Device::getShaderConstantB(bool pixel, uint32_t start, uint32_t* data, uint32_t count) const {
    if (start >= 16 || !data)
      return;
    count = (std::min)(count, 16u - start);
    std::memcpy(data, &m_constants[pixel].b[start], size_t(count) * 4);
  }

  void Device::setVertexDeclaration(VertexDecl* decl) {
    if (m_decl == decl)
      return;
    const uint32_t ffpFvf = decl ? decl->ffpFvf() : 0;
    if (ffpFvf != m_ffpFvf) {
      if ((ffpFvf ^ m_ffpFvf) & FvfPositionMask)
        m_pixelConstantsDirty = true;
      m_ffpFvf = ffpFvf;
      m_vertexConstantsDirty = true;
    }
    m_decl = decl;
    m_dirty |= DirtyLayout | DirtyShaders;
  }

  VertexDecl* Device::fvfDeclaration(uint32_t fvf) {
    // Draws of Direct3D 7 name their format each time: the last one kept.
    if (m_lastFvfDecl && m_lastFvf == fvf)
      return m_lastFvfDecl;
    auto& decl = m_fvfDecls[fvf];
    if (!decl) {
      const auto elements = VertexDecl::elementsOf(fvf);
      decl = std::make_unique<VertexDecl>(elements.data(), uint32_t(elements.size()), fvf);
    }
    m_lastFvf = fvf;
    m_lastFvfDecl = decl.get();
    return m_lastFvfDecl;
  }

  void Device::setStreamSource(uint32_t stream, Buffer* buffer, uint32_t offset, uint32_t stride) {
    if (stream >= kMaxStreams)
      return;
    m_streams[stream].buffer = buffer;
    m_streams[stream].offset = offset;
    m_streams[stream].stride = stride;
  }

  void Device::setStreamFrequency(uint32_t stream, uint32_t value) {
    if (stream >= kMaxStreams || m_streams[stream].frequency == value)
      return;
    const bool wasInstance = (m_streams[stream].frequency & kInstanceData) != 0;
    m_streams[stream].frequency = value;
    if (wasInstance != ((value & kInstanceData) != 0) || (value & kInstanceData))
      m_dirty |= DirtyLayout;
  }

  void Device::setIndices(Buffer* buffer, bool index32) {
    m_indices = buffer;
    m_index32 = index32;
  }

  void Device::setClipPlane(uint32_t index, const float* plane) {
    if (index >= m_clipPlanes.size() || !plane)
      return;
    std::memcpy(m_clipPlanes[index].data(), plane, 16);
    m_vertexConstantsDirty = true;
    m_clipPlanesDirty = true;
  }

  // ---- D3D11 states from the legacy ones ----

  namespace {
    D3D11_BLEND blendFactor(uint32_t value, bool alpha) {
      switch (value) {
        case 1:  return D3D11_BLEND_ZERO;
        case 2:  return D3D11_BLEND_ONE;
        case 3:  return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case 4:  return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case 5:  return D3D11_BLEND_SRC_ALPHA;
        case 6:  return D3D11_BLEND_INV_SRC_ALPHA;
        case 7:  return D3D11_BLEND_DEST_ALPHA;
        case 8:  return D3D11_BLEND_INV_DEST_ALPHA;
        case 9:  return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case 10: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case 11: return D3D11_BLEND_SRC_ALPHA_SAT;
        case 14: return D3D11_BLEND_BLEND_FACTOR;
        case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
        case 16: return alpha ? D3D11_BLEND_SRC1_ALPHA : D3D11_BLEND_SRC1_COLOR;
        case 17: return alpha ? D3D11_BLEND_INV_SRC1_ALPHA : D3D11_BLEND_INV_SRC1_COLOR;
        default: return D3D11_BLEND_ONE;
      }
    }

    D3D11_BLEND_OP blendOp(uint32_t value) {
      return value >= 1 && value <= 5 ? D3D11_BLEND_OP(value) : D3D11_BLEND_OP_ADD;
    }

    D3D11_COMPARISON_FUNC comparison(uint32_t value) {
      return value >= 1 && value <= 8 ? D3D11_COMPARISON_FUNC(value) : D3D11_COMPARISON_ALWAYS;
    }

    D3D11_STENCIL_OP stencilOp(uint32_t value) {
      return value >= 1 && value <= 8 ? D3D11_STENCIL_OP(value) : D3D11_STENCIL_OP_KEEP;
    }

    template<typename T>
    uint64_t hashBytes(const T& value) {
      // Two 32-bit lanes over 32-bit words: 64-bit multiplies are calls on
      // x86, and these run whenever a draw changed a state.
      static_assert(sizeof(T) % 4 == 0, "state descriptions are whole words");
      uint32_t words[sizeof(T) / 4];
      std::memcpy(words, &value, sizeof(T));
      uint32_t a = 2166136261u, b = 0x9e3779b9u;
      for (uint32_t word : words) {
        a = (a ^ word) * 16777619u;
        b = (b ^ word) * 0x85ebca6bu + 0x27d4eb2fu;
      }
      return (uint64_t(a) << 32) | b;
    }
  }

  ID3D11BlendState* Device::blendState() {
    const auto& rs = m_renderStates;
    // The states the description comes from, as the key: cheaper than
    // hashing the description at every change.
    static const uint32_t masks[3] = { RsColorWriteEnable1, RsColorWriteEnable2, RsColorWriteEnable3 };
    auto field = [](uint32_t value) { return uint64_t(value < 31 ? value : 31); };
    uint64_t key = rs[RsColorWriteEnable] & 0xf;
    for (uint32_t i = 1; i < kMaxRenderTargets; i++)
      if (m_renderTargets[i])
        key |= uint64_t(0x10 | (rs[masks[i - 1]] & 0xf)) << (4 + 5 * (i - 1));
    if (rs[RsAlphaBlendEnable]) {
      key |= 1ull << 19 | field(rs[RsSrcBlend]) << 20 | field(rs[RsDestBlend]) << 25 | field(rs[RsBlendOp]) << 30;
      if (rs[RsSeparateAlphaBlendEnable])
        key |= 1ull << 35 | field(rs[RsSrcBlendAlpha]) << 36 | field(rs[RsDestBlendAlpha]) << 41
             | field(rs[RsBlendOpAlpha]) << 46;
    }
    if (key == m_lastBlendKey && m_lastBlendState)
      return m_lastBlendState;
    auto& state = m_blendStates[key];
    if (!state) {
      D3D11_BLEND_DESC desc = blendDesc();
      m_device->CreateBlendState(&desc, &state);
    }
    m_lastBlendKey = key;
    m_lastBlendState = state;
    return state;
  }

  D3D11_BLEND_DESC Device::blendDesc() const {
    const auto& rs = m_renderStates;
    D3D11_BLEND_DESC desc = {};
    auto& target = desc.RenderTarget[0];
    target.BlendEnable = rs[RsAlphaBlendEnable] != 0;
    uint32_t src = rs[RsSrcBlend], dst = rs[RsDestBlend];
    if (src == 12) { src = 5; dst = 6; }   // BOTHSRCALPHA
    if (src == 13) { src = 6; dst = 5; }   // BOTHINVSRCALPHA
    target.SrcBlend = blendFactor(src, false);
    target.DestBlend = blendFactor(dst, false);
    target.BlendOp = blendOp(rs[RsBlendOp]);
    if (rs[RsSeparateAlphaBlendEnable]) {
      target.SrcBlendAlpha = blendFactor(rs[RsSrcBlendAlpha], true);
      target.DestBlendAlpha = blendFactor(rs[RsDestBlendAlpha], true);
      target.BlendOpAlpha = blendOp(rs[RsBlendOpAlpha]);
    } else {
      target.SrcBlendAlpha = blendFactor(src, true);
      target.DestBlendAlpha = blendFactor(dst, true);
      target.BlendOpAlpha = target.BlendOp;
    }
    target.RenderTargetWriteMask = UINT8(rs[RsColorWriteEnable] & 0xf);
    if (!target.BlendEnable) {
      target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
      target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_ZERO;
      target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    // More targets: the same blending, each its write mask.
    static const uint32_t masks[3] = { RsColorWriteEnable1, RsColorWriteEnable2, RsColorWriteEnable3 };
    for (uint32_t i = 1; i < kMaxRenderTargets; i++) {
      if (!m_renderTargets[i])
        continue;
      desc.RenderTarget[i] = target;
      desc.RenderTarget[i].RenderTargetWriteMask = UINT8(rs[masks[i - 1]] & 0xf);
      if (desc.RenderTarget[i].RenderTargetWriteMask != target.RenderTargetWriteMask)
        desc.IndependentBlendEnable = TRUE;
    }
    return desc;
  }

  ID3D11DepthStencilState* Device::depthStencilState() {
    const auto& rs = m_renderStates;
    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable = rs[RsZEnable] != 0 && m_depthStencil;
    desc.DepthWriteMask = rs[RsZWriteEnable] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = comparison(rs[RsZFunc]);
    desc.StencilEnable = rs[RsStencilEnable] != 0 && m_depthStencil && formatInfo(m_depthStencil->desc().format).stencil;
    desc.StencilReadMask = UINT8(rs[RsStencilMask]);
    desc.StencilWriteMask = UINT8(rs[RsStencilWriteMask]);
    desc.FrontFace = { stencilOp(rs[RsStencilFail]), stencilOp(rs[RsStencilZFail]), stencilOp(rs[RsStencilPass]),
                       comparison(rs[RsStencilFunc]) };
    desc.BackFace = rs[RsTwoSidedStencilMode]
      ? D3D11_DEPTH_STENCILOP_DESC{ stencilOp(rs[RsCcwStencilFail]), stencilOp(rs[RsCcwStencilZFail]),
                                    stencilOp(rs[RsCcwStencilPass]), comparison(rs[RsCcwStencilFunc]) }
      : desc.FrontFace;
    if (!desc.DepthEnable) {
      desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
      desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    }
    const uint64_t key = hashBytes(desc);
    auto& state = m_depthStates[key];
    if (!state)
      m_device->CreateDepthStencilState(&desc, &state);
    return state;
  }

  ID3D11RasterizerState* Device::rasterizerState() {
    const auto& rs = m_renderStates;
    D3D11_RASTERIZER_DESC desc = {};
    desc.FillMode = rs[RsFillMode] == 2 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    // Direct3D's front faces turn clockwise: CW culls them, CCW the back.
    desc.CullMode = rs[RsCullMode] == 2 ? D3D11_CULL_FRONT : rs[RsCullMode] == 3 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    desc.FrontCounterClockwise = FALSE;
    const float bias = asFloat(rs[RsDepthBias]);
    // Direct3D 9's bias is in depth units; D3D11's in steps of the format.
    const float steps = m_depthStencil && formatInfo(m_depthStencil->desc().format).bytesPerBlock == 2 ? 65535.0f : 16777215.0f;
    desc.DepthBias = INT(bias * steps) - INT(rs[RsZBias]) * 1000;
    desc.SlopeScaledDepthBias = asFloat(rs[RsSlopeScaleDepthBias]);
    desc.DepthClipEnable = TRUE;
    desc.ScissorEnable = rs[RsScissorTestEnable] != 0;
    desc.MultisampleEnable = TRUE;
    const uint64_t key = hashBytes(desc);
    auto& state = m_rasterStates[key];
    if (!state)
      m_device->CreateRasterizerState(&desc, &state);
    return state;
  }

  ID3D11SamplerState* Device::d3dSampler(uint32_t sampler, bool compare) {
    const auto& ss = m_samplerStatesValues[sampler];
    D3D11_SAMPLER_DESC desc = {};
    const uint32_t mag = ss[SampMagFilter], min = ss[SampMinFilter], mip = ss[SampMipFilter];
    const bool anisotropic = mag == 3 || min == 3;
    const auto linear = [](uint32_t f) { return f >= 2 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT; };
    const D3D11_FILTER_REDUCTION_TYPE reduction = compare ? D3D11_FILTER_REDUCTION_TYPE_COMPARISON
                                                          : D3D11_FILTER_REDUCTION_TYPE_STANDARD;
    if (anisotropic && !compare) {
      desc.Filter = D3D11_FILTER_ANISOTROPIC;
      desc.MaxAnisotropy = (std::max)(1u, (std::min)(16u, ss[SampMaxAnisotropy]));
    } else {
      desc.Filter = D3D11_ENCODE_BASIC_FILTER(linear(min), linear(mag), linear(mip), reduction);
      desc.MaxAnisotropy = 1;
    }
    const auto address = [](uint32_t a) { return a >= 1 && a <= 5 ? D3D11_TEXTURE_ADDRESS_MODE(a) : D3D11_TEXTURE_ADDRESS_WRAP; };
    desc.AddressU = address(ss[SampAddressU]);
    desc.AddressV = address(ss[SampAddressV]);
    desc.AddressW = address(ss[SampAddressW]);
    colorToFloats(ss[SampBorderColor], desc.BorderColor);
    // Direct3D 9 drivers take any bias (UE3 games set NaN: The Vanishing of
    // Ethan Carter); D3D11 refuses samplers with NaN or one out of range.
    const float bias = asFloat(ss[SampMipMapLodBias]);
    desc.MipLODBias = bias == bias ? (std::max)(-16.0f, (std::min)(bias, 15.99f)) : 0.0f;
    desc.MinLOD = float(ss[SampMaxMipLevel]);
    desc.MaxLOD = mip == 0 ? desc.MinLOD : D3D11_FLOAT32_MAX;
    // Shadow maps: 1 where the reference is not farther than the depth.
    desc.ComparisonFunc = compare ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_NEVER;
    const uint64_t key = hashBytes(desc);
    auto& state = m_samplerStates[key];
    if (!state)
      m_device->CreateSamplerState(&desc, &state);
    return state;
  }

  ID3D11InputLayout* Device::inputLayout() {
    if (!m_decl)
      return nullptr;
    const Shader::Variant* vs = m_vsVariant;
    uint64_t key = fnv64Multiply(m_decl->hash() ^ (vs ? vs->id : 0u));
    for (uint32_t stream = 0; stream < kMaxStreams; stream++) {
      const uint32_t frequency = m_streams[stream].frequency;
      if ((m_decl->streamMask() >> stream) & 1 && (frequency & kInstanceData))
        key = fnv64Multiply(key ^ (uint64_t(stream) << 32) ^ frequency);
    }
    auto& layout = m_inputLayouts[key];
    if (layout)
      return layout;

    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    auto find = [&](uint8_t usage, uint8_t index) -> const VertexElement* {
      for (const auto& e : m_decl->elements())
        if (e.usage == usage && e.usageIndex == index)
          return &e;
      return nullptr;
    };
    auto add = [&](const char* name, UINT index, const VertexElement* e, DXGI_FORMAT fallback, UINT fallbackOffset) {
      if (e && declFormat(e->type) != DXGI_FORMAT_UNKNOWN) {
        const uint32_t frequency = m_streams[e->stream & 15].frequency;
        const bool instanced = (frequency & kInstanceData) != 0;
        elements.push_back({ name, index, declFormat(e->type), UINT(e->stream & 15), e->offset,
          instanced ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA,
          instanced ? (std::max)(1u, frequency & 0x3fffffffu) : 0u });
      } else {
        elements.push_back({ name, index, fallback, kDefaultStream, fallbackOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 });
      }
    };
    if (vs) {
      for (const auto& in : vs->info.inputs) {
        const VertexElement* e = find(in.usage, in.usageIndex);
        // Pre-transformed positions feed position inputs too.
        if (!e && in.usage == UsePosition)
          e = find(UsePositionT, in.usageIndex);
        add(semanticName9(in.usage), in.usageIndex, e, DXGI_FORMAT_R32G32B32A32_FLOAT, kDefaultZeroOffset);
      }
      if (FAILED(m_device->CreateInputLayout(elements.data(), UINT(elements.size()), vs->bytecode.data(),
            vs->bytecode.size(), &layout)))
        layout = nullptr;
      return layout;
    }
    const VertexElement* position = find(UsePositionT, 0);
    if (!position) position = find(UsePosition, 0);
    add("POSITION", 0, position, DXGI_FORMAT_R32G32B32A32_FLOAT, kDefaultZeroOffset);
    add("BLENDWEIGHT", 0, find(UseBlendWeight, 0), DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
    const VertexElement* indices = find(UseBlendIndices, 0);
    if (indices && indices->type != DeclUByte4 && indices->type != DeclD3DColor)
      indices = nullptr;
    if (indices) {
      VertexElement asUint = *indices;
      asUint.type = DeclUByte4;
      add("BLENDINDICES", 0, &asUint, DXGI_FORMAT_R8G8B8A8_UINT, 16);
    } else {
      add("BLENDINDICES", 0, nullptr, DXGI_FORMAT_R8G8B8A8_UINT, 16);
    }
    add("NORMAL", 0, find(UseNormal, 0), DXGI_FORMAT_R32G32B32_FLOAT, 20);
    add("COLOR", 0, find(UseColor, 0), DXGI_FORMAT_B8G8R8A8_UNORM, 32);
    add("COLOR", 1, find(UseColor, 1), DXGI_FORMAT_B8G8R8A8_UNORM, 36);
    for (uint8_t i = 0; i < 8; i++)
      add("TEXCOORD", i, find(UseTexCoord, i), DXGI_FORMAT_R32G32B32A32_FLOAT, 48);
    if (FAILED(m_device->CreateInputLayout(elements.data(), UINT(elements.size()), g_ffp_vs, sizeof(g_ffp_vs), &layout)))
      layout = nullptr;
    return layout;
  }

  namespace {
    // Where the parts of a flexible-format vertex are (-1: absent).
    struct FvfLayout {
      int position = 0, normal = -1, diffuse = -1, specular = -1;
      int tex[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
      uint32_t texSize[8] = {};
      uint32_t texCount = 0;
    };

    FvfLayout fvfLayout(uint32_t fvf) {
      FvfLayout l;
      int offset = 0;
      switch (fvf & FvfPositionMask) {
        case FvfXyz:    offset = 12; break;
        case FvfXyzRhw: offset = 16; break;
        case FvfXyzW:   offset = 16; break;
        default:        offset = 12 + 4 * int(blendWeightCount(fvf)); break;
      }
      if (fvf & FvfNormal) { l.normal = offset; offset += 12; }
      if (fvf & FvfPSize) offset += 4;
      if (fvf & FvfDiffuse) { l.diffuse = offset; offset += 4; }
      if (fvf & FvfSpecular) { l.specular = offset; offset += 4; }
      l.texCount = (std::min)((fvf & FvfTexCountMask) >> FvfTexCountShift, 8u);
      for (uint32_t i = 0; i < l.texCount; i++) {
        l.tex[i] = offset;
        l.texSize[i] = texCoordSize(fvf, i);
        offset += int(4 * l.texSize[i]);
      }
      return l;
    }

    uint32_t packColor(const float* rgba) {
      auto channel = [](float v) { return uint32_t((v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v) * 255.0f + 0.5f); };
      return channel(rgba[3]) << 24 | channel(rgba[0]) << 16 | channel(rgba[1]) << 8 | channel(rgba[2]);
    }
  }

  void Device::processVertices(const uint8_t* src, uint32_t srcFvf, uint32_t srcStride, uint8_t* dst, uint32_t dstFvf,
                               uint32_t dstStride, uint32_t count, bool light, bool copyData) const {
    const auto& rs = m_renderStates;
    const FvfLayout from = fvfLayout(srcFvf), to = fvfLayout(dstFvf);
    float worldView[16], normalMatrix[16];
    multiply(m_world[0].data(), m_view.data(), worldView);
    normalMatrixOf(worldView, normalMatrix);
    const bool lit = light && rs[RsLighting] && from.normal >= 0;

    // The enabled lights in view space, as the vertex shader gets them.
    struct ViewLight { const Light* light; float position[4], direction[4], range, cosTheta, cosPhi; };
    ViewLight lights[8];
    uint32_t lightCount = 0;
    if (lit) {
      for (size_t i = 0; i < m_lights.size() && lightCount < 8; i++) {
        if (!m_lightEnabled[i])
          continue;
        ViewLight& v = lights[lightCount++];
        v.light = &m_lights[i];
        transformPoint(m_view.data(), m_lights[i].position, v.position);
        transformDirection(m_view.data(), m_lights[i].direction, v.direction);
        v.range = m_lights[i].range > 0.0f ? m_lights[i].range : 3.4e38f;
        v.cosTheta = std::cos(m_lights[i].theta * 0.5f);
        v.cosPhi = std::cos(m_lights[i].phi * 0.5f);
      }
    }
    // Material colors from the vertex only when it has them.
    auto source = [&](uint32_t value) -> uint32_t {
      if (!rs[RsColorVertex]) return 0;
      if (value == 1 && from.diffuse < 0) return 0;
      if (value == 2 && from.specular < 0) return 0;
      return value <= 2 ? value : 0;
    };
    const uint32_t diffuseSource = source(rs[RsDiffuseMaterialSource]), ambientSource = source(rs[RsAmbientMaterialSource]);
    const uint32_t specularSource = source(rs[RsSpecularMaterialSource]), emissiveSource = source(rs[RsEmissiveMaterialSource]);
    float globalAmbient[4];
    colorToFloats(rs[RsAmbient], globalAmbient);

    const float vw = float(m_viewport.width), vh = float(m_viewport.height);
    for (uint32_t v = 0; v < count; v++) {
      const uint8_t* in = src + size_t(v) * srcStride;
      uint8_t* out = dst + size_t(v) * dstStride;
      float p[3];
      std::memcpy(p, in + from.position, sizeof(p));
      float view[3], clip[4];
      for (int c = 0; c < 3; c++)
        view[c] = p[0] * worldView[c] + p[1] * worldView[4 + c] + p[2] * worldView[8 + c] + worldView[12 + c];
      for (int c = 0; c < 4; c++)
        clip[c] = view[0] * m_projection[c] + view[1] * m_projection[4 + c] + view[2] * m_projection[8 + c] + m_projection[12 + c];
      const float rhw = clip[3] != 0.0f ? 1.0f / clip[3] : 1.0f;
      const float screen[4] = {
        float(m_viewport.x) + (1.0f + clip[0] * rhw) * vw * 0.5f,
        float(m_viewport.y) + (1.0f - clip[1] * rhw) * vh * 0.5f,
        m_viewport.minZ + clip[2] * rhw * (m_viewport.maxZ - m_viewport.minZ),
        rhw,
      };
      std::memcpy(out + to.position, screen, sizeof(screen));

      uint32_t color0 = 0xffffffffu, color1 = 0;
      if (from.diffuse >= 0) std::memcpy(&color0, in + from.diffuse, 4);
      if (from.specular >= 0) std::memcpy(&color1, in + from.specular, 4);
      if (lit) {
        float n[3] = {};
        std::memcpy(n, in + from.normal, sizeof(n));
        float normal[3];
        for (int c = 0; c < 3; c++)
          normal[c] = n[0] * normalMatrix[c] + n[1] * normalMatrix[4 + c] + n[2] * normalMatrix[8 + c];
        if (rs[RsNormalizeNormals]) {
          const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
          if (length > 0.0f)
            for (float& c : normal) c /= length;
        }
        float vertex0[4], vertex1[4];
        colorToFloats(color0, vertex0);
        colorToFloats(color1, vertex1);
        auto pick = [&](uint32_t which, const Color& material) -> const float* {
          return which == 1 ? vertex0 : which == 2 ? vertex1 : &material.r;
        };
        const float* diffuseColor = pick(diffuseSource, m_material.diffuse);
        const float* ambientColor = pick(ambientSource, m_material.ambient);
        const float* specularColor = pick(specularSource, m_material.specular);
        const float* emissiveColor = pick(emissiveSource, m_material.emissive);
        float diffuse[3] = {}, specular[3] = {}, ambient[3] = {};
        float toViewer[3] = { 0.0f, 0.0f, -1.0f };
        if (rs[RsLocalViewer]) {
          const float length = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
          if (length > 0.0f)
            for (int c = 0; c < 3; c++) toViewer[c] = -view[c] / length;
        }
        for (uint32_t i = 0; i < lightCount; i++) {
          const ViewLight& l = lights[i];
          float toLight[3];
          float attenuation = 1.0f;
          if (l.light->type == 3) {
            for (int c = 0; c < 3; c++) toLight[c] = -l.direction[c];
          } else {
            float offset[3];
            for (int c = 0; c < 3; c++) offset[c] = l.position[c] - view[c];
            const float distance = std::sqrt(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
            if (distance > l.range)
              continue;
            for (int c = 0; c < 3; c++) toLight[c] = distance > 0.0f ? offset[c] / distance : 0.0f;
            const float denominator = l.light->attenuation0 + l.light->attenuation1 * distance
                                    + l.light->attenuation2 * distance * distance;
            attenuation = denominator > 0.0f ? 1.0f / denominator : 1.0f;
            if (l.light->type == 2) {
              const float rho = -(toLight[0] * l.direction[0] + toLight[1] * l.direction[1] + toLight[2] * l.direction[2]);
              if (rho <= l.cosPhi)
                attenuation = 0.0f;
              else if (rho < l.cosTheta) {
                const float t = (rho - l.cosPhi) / (l.cosTheta - l.cosPhi);
                attenuation *= std::pow(t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t, l.light->falloff);
              }
            }
          }
          ambient[0] += attenuation * l.light->ambient.r;
          ambient[1] += attenuation * l.light->ambient.g;
          ambient[2] += attenuation * l.light->ambient.b;
          const float lambert = normal[0] * toLight[0] + normal[1] * toLight[1] + normal[2] * toLight[2];
          if (lambert > 0.0f) {
            diffuse[0] += attenuation * lambert * l.light->diffuse.r;
            diffuse[1] += attenuation * lambert * l.light->diffuse.g;
            diffuse[2] += attenuation * lambert * l.light->diffuse.b;
            if (rs[RsSpecularEnable]) {
              float halfway[3];
              for (int c = 0; c < 3; c++) halfway[c] = toLight[c] + toViewer[c];
              const float length = std::sqrt(halfway[0] * halfway[0] + halfway[1] * halfway[1] + halfway[2] * halfway[2]);
              float d = length > 0.0f ? (normal[0] * halfway[0] + normal[1] * halfway[1] + normal[2] * halfway[2]) / length : 0.0f;
              d = d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
              const float power = std::pow(d, m_material.power);
              specular[0] += attenuation * power * l.light->specular.r;
              specular[1] += attenuation * power * l.light->specular.g;
              specular[2] += attenuation * power * l.light->specular.b;
            }
          }
        }
        float lit0[4], lit1[4];
        for (int c = 0; c < 3; c++) {
          lit0[c] = emissiveColor[c] + ambientColor[c] * (globalAmbient[c] + ambient[c]) + diffuseColor[c] * diffuse[c];
          lit1[c] = rs[RsSpecularEnable] ? specularColor[c] * specular[c] : 0.0f;
        }
        lit0[3] = diffuseColor[3];
        lit1[3] = vertex1[3];  // the fog factor stays
        color0 = packColor(lit0);
        color1 = packColor(lit1);
        if (to.diffuse >= 0) std::memcpy(out + to.diffuse, &color0, 4);
        if (to.specular >= 0) std::memcpy(out + to.specular, &color1, 4);
      } else if (copyData) {
        if (to.diffuse >= 0) std::memcpy(out + to.diffuse, &color0, 4);
        if (to.specular >= 0) std::memcpy(out + to.specular, &color1, 4);
      }
      if (copyData) {
        for (uint32_t i = 0; i < to.texCount; i++) {
          float tc[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
          if (i < from.texCount)
            std::memcpy(tc, in + from.tex[i], 4 * from.texSize[i]);
          std::memcpy(out + to.tex[i], tc, 4 * to.texSize[i]);
        }
      }
    }
  }

  void Device::updateVertexConstants() {
    // Built in ordinary memory, then copied: the buffer's memory may be
    // write-combined video memory, where every read takes a bus round trip.
    VertexConstants constants;
    VertexConstants* c = &constants;
    std::memset(c, 0, sizeof(*c));
    const auto& rs = m_renderStates;
    const uint32_t fvf = m_ffpFvf;
    const uint32_t position = fvf & FvfPositionMask;
    const bool transformed = position == FvfXyzRhw;

    std::memcpy(c->projection, m_projection.data(), sizeof(c->projection));
    for (int i = 0; i < 8; i++)
      std::memcpy(c->textureMatrix[i], m_textureTransforms[i].data(), sizeof(c->textureMatrix[i]));

    // Pre-transformed vertices: window coordinates of the viewport to clip space.
    const float vw = float((std::max)(1u, m_viewport.width)), vh = float((std::max)(1u, m_viewport.height));
    const float depthRange = m_viewport.maxZ - m_viewport.minZ;
    c->rhwScale[0] = 2.0f / vw;
    c->rhwScale[1] = -2.0f / vh;
    c->rhwScale[2] = depthRange != 0.0f ? 1.0f / depthRange : 0.0f;
    c->rhwOffset[0] = -1.0f - 2.0f * float(m_viewport.x) / vw;
    c->rhwOffset[1] = 1.0f + 2.0f * float(m_viewport.y) / vh;
    c->rhwOffset[2] = depthRange != 0.0f ? -m_viewport.minZ / depthRange : 0.0f;

    std::memcpy(c->materialDiffuse, &m_material.diffuse, 16);
    std::memcpy(c->materialAmbient, &m_material.ambient, 16);
    std::memcpy(c->materialSpecular, &m_material.specular, 16);
    std::memcpy(c->materialEmissive, &m_material.emissive, 16);
    colorToFloats(rs[RsAmbient], c->globalAmbient);
    c->fogVertex[0] = m_material.power;
    c->fogVertex[1] = asFloat(rs[RsFogStart]);
    c->fogVertex[2] = asFloat(rs[RsFogEnd]);
    c->fogVertex[3] = asFloat(rs[RsFogDensity]);

    uint32_t lightCount = 0;
    if (!transformed && rs[RsLighting]) {
      for (size_t i = 0; i < m_lights.size() && lightCount < 8; i++) {
        if (!m_lightEnabled[i])
          continue;
        const Light& l = m_lights[i];
        auto& out = c->lights[lightCount++];
        std::memcpy(out.diffuse, &l.diffuse, 16);
        std::memcpy(out.specular, &l.specular, 16);
        std::memcpy(out.ambient, &l.ambient, 16);
        transformPoint(m_view.data(), l.position, out.position);
        transformDirection(m_view.data(), l.direction, out.direction);
        out.params0[0] = l.range > 0.0f ? l.range : 3.4e38f;
        out.params0[1] = l.falloff;
        out.params0[2] = l.attenuation0;
        out.params0[3] = l.attenuation1;
        out.params1[0] = l.attenuation2;
        out.params1[1] = std::cos(l.theta * 0.5f);
        out.params1[2] = std::cos(l.phi * 0.5f);
        out.params1[3] = float(l.type);
      }
    }
    uint32_t blend = 0;
    switch (rs[RsVertexBlend]) {
      case 1: blend = 1; break;
      case 2: blend = 2; break;
      case 3: blend = 3; break;
      default: break;
    }
    blend = (std::min)(blend, blendWeightCount(fvf));
    c->vertexFlags[0] = transformed ? 1 : 0;
    c->vertexFlags[1] = (!transformed && rs[RsLighting]) ? 1 : 0;
    c->vertexFlags[2] = blend | (rs[RsIndexedVertexBlendEnable] ? 0x100u : 0u);
    c->vertexFlags[3] = lightCount;

    // Material colors come from the vertex only when it has them.
    auto source = [&](uint32_t value) -> uint32_t {
      if (!rs[RsColorVertex]) return 0;
      if (value == 1 && !(fvf & FvfDiffuse)) return 0;
      if (value == 2 && !(fvf & FvfSpecular)) return 0;
      return value <= 2 ? value : 0;
    };
    c->colorSources[0] = source(rs[RsDiffuseMaterialSource]);
    c->colorSources[1] = source(rs[RsAmbientMaterialSource]);
    c->colorSources[2] = source(rs[RsSpecularMaterialSource]);
    c->colorSources[3] = source(rs[RsEmissiveMaterialSource]);

    const uint32_t fogVertexMode = rs[RsFogEnable] && !rs[RsFogTableMode] ? rs[RsFogVertexMode] : 0;
    // D3DFOG_EXP 1, EXP2 2, LINEAR 3: the shader's numbering.
    c->vertexFlags2[0] = rs[RsSpecularEnable] ? 1 : 0;
    c->vertexFlags2[1] = rs[RsLocalViewer] ? 1 : 0;
    c->vertexFlags2[2] = rs[RsNormalizeNormals] ? 1 : 0;
    c->vertexFlags2[3] = (fogVertexMode & 0xf) | (rs[RsRangeFogEnable] ? 0x10u : 0u);

    // User clip planes: world space, moved to view space. A point's view
    // position p = w V, so dot(w, n) = dot(p, n (V^-1)^T).
    const uint32_t clipMask = transformed ? 0u : (rs[RsClipPlaneEnable] & 0x3fu);
    c->clipFlags[0] = clipMask;
    if (clipMask) {
      float inverseView[16];
      if (!invert(m_view.data(), inverseView))
        identity(inverseView);
      for (uint32_t plane = 0; plane < 6; plane++) {
        if (!((clipMask >> plane) & 1))
          continue;
        const float* n = m_clipPlanes[plane].data();
        for (int r = 0; r < 4; r++)
          c->clipPlanes[plane][r] = n[0] * inverseView[r * 4] + n[1] * inverseView[r * 4 + 1] + n[2] * inverseView[r * 4 + 2]
                                  + n[3] * inverseView[r * 4 + 3];
      }
    }

    const uint32_t texCount = (std::min)((fvf & FvfTexCountMask) >> FvfTexCountShift, 8u);
    for (uint32_t stage = 0; stage < 8; stage++) {
      const uint32_t index = m_stageStates[stage][TssTexCoordIndex];
      const uint32_t set = index & 0xffff;
      const uint32_t generation = index >> 16;
      const uint32_t transform = m_stageStates[stage][TssTextureTransformFlags] & 0xff;
      c->texCoordSetup[stage][0] = set & 7;
      c->texCoordSetup[stage][1] = generation;
      c->texCoordSetup[stage][2] = transformed ? 0 : transform;
      c->texCoordSetup[stage][3] = set < texCount ? texCoordSize(fvf, set) : 4;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_context->Map(m_vertexConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      writeLines(static_cast<uint8_t*>(mapped.pData), c, sizeof(*c));
      m_context->Unmap(m_vertexConstants.get(), 0);
    }
    m_vertexConstantsDirty = false;
  }

  void Device::updateTransformConstants() {
    // The matrices vertex blending uses, the first alone without it.
    const uint32_t blend = m_renderStates[RsVertexBlend];
    const uint32_t count = m_renderStates[RsIndexedVertexBlendEnable] ? 4u : (blend >= 1 && blend <= 3 ? blend + 1 : 1u);
    TransformConstants constants;
    std::memset(&constants, 0, sizeof(constants));
    bool normals = m_renderStates[RsLighting] != 0;
    for (uint32_t stage = 0; stage < 8 && !normals; stage++) {
      const uint32_t generation = m_stageStates[stage][TssTexCoordIndex] >> 16;
      normals = generation == 1 || generation == 3 || generation == 4;
    }
    for (uint32_t i = 0; i < count; i++) {
      multiply(m_world[i].data(), m_view.data(), constants.worldView[i]);
      if (normals)
        normalMatrixOf(constants.worldView[i], constants.normalMatrix[i]);
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_context->Map(m_transformConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      writeLines(static_cast<uint8_t*>(mapped.pData), &constants, sizeof(constants));
      m_context->Unmap(m_transformConstants.get(), 0);
    }
    m_transformConstantsDirty = false;
  }

  void Device::updatePixelConstants() {
    PixelConstants constants;
    PixelConstants* c = &constants;
    std::memset(c, 0, sizeof(*c));
    const auto& rs = m_renderStates;
    uint32_t stageCount = 0;
    for (uint32_t stage = 0; stage < 8; stage++) {
      const auto& tss = m_stageStates[stage];
      if (tss[TssColorOp] == 1)
        break;
      stageCount = stage + 1;
      const uint32_t type = textureTypeOf(m_textures[stage]);
      const uint32_t flags = tss[TssTextureTransformFlags];
      const uint32_t projected = (flags & 256) ? 1u : 0u;
      c->stageOps[stage][0] = tss[TssColorOp];
      c->stageOps[stage][1] = tss[TssAlphaOp];
      c->stageOps[stage][2] = tss[TssResultArg] == 5 ? 1u : 0u;
      c->stageOps[stage][3] = type | (projected << 4) | ((flags & 0xf) << 8);
      c->stageArgs[stage][0] = tss[TssColorArg1];
      c->stageArgs[stage][1] = tss[TssColorArg2];
      c->stageArgs[stage][2] = tss[TssColorArg0];
      c->stageArgs[stage][3] = (tss[TssAlphaArg1] & 0xff) | ((tss[TssAlphaArg2] & 0xff) << 8) | ((tss[TssAlphaArg0] & 0xff) << 16);
      colorToFloats(tss[TssConstant], c->stageConstant[stage]);
      c->bumpMatrix[stage][0] = asFloat(tss[TssBumpEnvMat00]);
      c->bumpMatrix[stage][1] = asFloat(tss[TssBumpEnvMat01]);
      c->bumpMatrix[stage][2] = asFloat(tss[TssBumpEnvMat10]);
      c->bumpMatrix[stage][3] = asFloat(tss[TssBumpEnvMat11]);
      c->bumpLuminance[stage][0] = asFloat(tss[TssBumpEnvLScale]);
      c->bumpLuminance[stage][1] = asFloat(tss[TssBumpEnvLOffset]);
    }
    colorToFloats(rs[RsTextureFactor], c->textureFactor);
    colorToFloats(rs[RsFogColor], c->fogColor);
    c->fogTable[0] = asFloat(rs[RsFogStart]);
    c->fogTable[1] = asFloat(rs[RsFogEnd]);
    c->fogTable[2] = asFloat(rs[RsFogDensity]);
    // Fog per pixel: the table's, else the vertex mode's from the
    // interpolated distance (pre-transformed vertices bring their factor).
    const uint32_t tableMode = rs[RsFogTableMode] <= 3 ? rs[RsFogTableMode] : 0;
    const bool transformed = (m_ffpFvf & FvfPositionMask) == FvfXyzRhw;
    const uint32_t vertexMode = !transformed && rs[RsFogVertexMode] <= 3 ? rs[RsFogVertexMode] : 0;
    c->fogTable[3] = float(tableMode ? tableMode : vertexMode);
    c->alphaTest[0] = float(rs[RsAlphaRef] & 0xff) / 255.0f;
    c->alphaTest[1] = float(rs[RsAlphaFunc]);
    c->alphaTest[2] = rs[RsAlphaTestEnable] ? 1.0f : 0.0f;
    c->alphaTest[3] = rs[RsSpecularEnable] ? 1.0f : 0.0f;
    c->pixelFlags[0] = rs[RsFogEnable] ? 1 : 0;
    c->pixelFlags[1] = stageCount;
    c->pixelFlags[2] = !tableMode && vertexMode && rs[RsRangeFogEnable] ? 1 : 0;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_context->Map(m_pixelConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      writeLines(static_cast<uint8_t*>(mapped.pData), c, sizeof(*c));
      m_context->Unmap(m_pixelConstants.get(), 0);
    }
    m_pixelConstantsDirty = false;
  }

  // dxup_frame_dump.txt next to the DLL (DXUP Manager's "Record a problem").
  bool glitchReportsEnabled() {
    static const bool enabled = [] {
      HMODULE self = nullptr;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&glitchReportsEnabled), &self);
      wchar_t path[MAX_PATH] = {};
      if (!self || !GetModuleFileNameW(self, path, MAX_PATH))
        return false;
      wchar_t* name = wcsrchr(path, L'\\');
      if (!name)
        return false;
      wcscpy_s(name + 1, MAX_PATH - (name + 1 - path), L"dxup_frame_dump.txt");
      const DWORD attributes = GetFileAttributesW(path);
      return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }();
    return enabled;
  }

  // The float constants a shader reads: the first NaN or infinity of the
  // frame is kept for the log at Present.
  void Device::checkConstants(bool pixel, uint32_t floats) {
    const float* values = &m_constants[pixel].f[0][0];
    for (uint32_t i = 0; i < floats * 4; i++) {
      if (std::isfinite(values[i]))
        continue;
      const Shader* shader = pixel ? m_pixelShader : m_vertexShader;
      if (!m_nonFinite.uploads++) {
        m_nonFinite.pixel = pixel;
        m_nonFinite.reg = i / 4;
        m_nonFinite.component = i % 4;
        m_nonFinite.value = values[i];
        m_nonFinite.shader = shader ? shader->hash() : 0;
      }
      // Once per shader (8 at most): which constants, and what it does with
      // them (its HLSL as DXUP writes it).
      static std::mutex s_loggedMutex;
      static std::vector<uint64_t> s_logged;
      if (!shader)
        return;
      {
        std::lock_guard<std::mutex> lock(s_loggedMutex);
        if (s_logged.size() >= 8 || std::find(s_logged.begin(), s_logged.end(), shader->hash()) != s_logged.end())
          return;
        s_logged.push_back(shader->hash());
      }
      std::string registers;
      for (uint32_t j = i; j < floats * 4 && registers.size() < 400; j++) {
        if (!std::isfinite(values[j])) {
          char text[48];
          sprintf_s(text, " c%u.%c=%g", j / 4, "xyzw"[j % 4], values[j]);
          registers += text;
        }
      }
      std::string hlsl, error;
      ShaderInfo9 info;
      TranslateOptions9 options;
      driverBehavior(options);
      if (!translateShader9(shader->tokens().data(), shader->tokens().size(), options, hlsl, info, error))
        hlsl = "(not translated: " + error + ")";
      logMessage(LogLevel::Warn, "glitch: %s shader %016llX reads NaN or infinite constants:%s; its HLSL:\n%s",
        pixel ? "pixel" : "vertex", static_cast<unsigned long long>(shader->hash()), registers.c_str(), hlsl.c_str());
      return;
    }
  }

  void Device::logNonFiniteConstants() {
    m_nonFinite.presents++;
    if (!m_nonFinite.uploads)
      return;
    // Every frame with any for the first 20, then every 100th.
    m_nonFinite.frames++;
    if (m_nonFinite.frames <= 20 || m_nonFinite.frames % 100 == 0)
      logMessage(LogLevel::Warn, "glitch: present %u: %u draws read NaN or infinite constants (first: %s shader %016llX c%u.%c = %g)%s",
        m_nonFinite.presents, m_nonFinite.uploads, m_nonFinite.pixel ? "pixel" : "vertex",
        static_cast<unsigned long long>(m_nonFinite.shader), m_nonFinite.reg, "xyzw"[m_nonFinite.component],
        m_nonFinite.value, m_nonFinite.frames == 20 ? "; from now on every 100th such frame" : "");
    m_nonFinite.uploads = 0;
  }

  namespace {
    const char* declTypeName(uint8_t type) {
      static const char* const kNames[] = { "FLOAT1", "FLOAT2", "FLOAT3", "FLOAT4", "D3DCOLOR", "UBYTE4", "SHORT2",
        "SHORT4", "UBYTE4N", "SHORT2N", "SHORT4N", "USHORT2N", "USHORT4N", "UDEC3", "DEC3N", "FLOAT16_2", "FLOAT16_4",
        "UNUSED" };
      return type < sizeof(kNames) / sizeof(kNames[0]) ? kNames[type] : "?";
    }
  }

  void Device::dropDraw(DropReason reason, uint32_t bytes) {
    m_dropped.counts[reason]++;
    const Shader* shader = reason == DropVertexShader || reason == DropInputLayout ? m_vertexShader
                         : reason == DropPixelShader ? m_pixelShader : nullptr;
    uint64_t cause = fnv64Multiply(uint64_t(reason) + 1);
    cause = fnv64Multiply(cause ^ (shader ? shader->hash() : 0));
    if (reason == DropInputLayout && m_decl)
      cause = fnv64Multiply(cause ^ m_decl->hash());
    if (m_dropped.causes.size() >= 32
     || std::find(m_dropped.causes.begin(), m_dropped.causes.end(), cause) != m_dropped.causes.end())
      return;
    m_dropped.causes.push_back(cause);
    std::string what;
    char text[160];
    switch (reason) {
      case DropDeclaration:
        what = "no vertex declaration or FVF set";
        break;
      case DropVertexShader:
      case DropPixelShader:
        sprintf_s(text, "%s shader %016llX did not compile for it (see the compile error above)",
          reason == DropVertexShader ? "vertex" : "pixel", static_cast<unsigned long long>(shader ? shader->hash() : 0));
        what = text;
        break;
      case DropInputLayout: {
        sprintf_s(text, "no input layout for vertex shader %016llX and the declaration:",
          static_cast<unsigned long long>(shader ? shader->hash() : 0));
        what = text;
        if (m_decl) {
          for (const auto& e : m_decl->elements()) {
            sprintf_s(text, " stream %u +%u %s usage %u/%u%s;", e.stream, e.offset, declTypeName(e.type), e.usage,
              e.usageIndex, (m_streams[e.stream & 15].frequency & kInstanceData) ? " per instance" : "");
            what += text;
          }
        }
        if (shader) {
          what += " shader reads";
          for (const auto& in : shader->info().inputs) {
            sprintf_s(text, " v%u usage %u/%u;", in.reg, in.usage, in.usageIndex);
            what += text;
          }
        }
        break;
      }
      case DropUpload:
        sprintf_s(text, "its %u bytes of application vertices or indices did not fit a ring", bytes);
        what = text;
        break;
      case DropIndices:
        what = "no index buffer set for an indexed draw";
        break;
      default:
        break;
    }
    logMessage(LogLevel::Warn, "glitch: a draw was not drawn: %s", what.c_str());
  }

  // Target 0's probe pixel as its bytes (copied to a staging texture of
  // one texel: synchronizes, tracing only).
  void Device::traceProbe9() {
    Texture* target = m_renderTargets[0];
    if (!target || !target->resource() || m_trace9X < 0)
      return;
    const uint32_t sub = m_renderTargetSubresources[0];
    if (uint32_t(m_trace9X) >= target->width(sub) || uint32_t(m_trace9Y) >= target->height(sub))
      return;
    D3D11_TEXTURE2D_DESC td;
    target->resource()->GetDesc(&td);
    if (td.SampleDesc.Count > 1)
      return;
    if (!m_trace9Staging || m_trace9Format != td.Format) {
      D3D11_TEXTURE2D_DESC sd = {};
      sd.Width = sd.Height = 1;
      sd.MipLevels = sd.ArraySize = 1;
      sd.Format = td.Format;
      sd.SampleDesc.Count = 1;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(m_device->CreateTexture2D(&sd, nullptr, m_trace9Staging.put())))
        return;
      m_trace9Format = td.Format;
    }
    const D3D11_BOX box = { UINT(m_trace9X), UINT(m_trace9Y), 0, UINT(m_trace9X) + 1, UINT(m_trace9Y) + 1, 1 };
    m_context->CopySubresourceRegion(m_trace9Staging.get(), 0, 0, 0, 0, target->resource(), sub, &box);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(m_trace9Staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
      return;
    uint32_t words[2] = {};
    std::memcpy(words, mapped.pData, 8);
    m_context->Unmap(m_trace9Staging.get(), 0);
    logMessage(LogLevel::Warn, "trace9   probe %08x %08x (target %ux%u format %u)", words[0], words[1], target->width(sub),
      target->height(sub), uint32_t(target->desc().format));
  }

  // The bound depth buffer's mean and nearest value (24-bit formats).
  void Device::traceDepthMean9() {
    if (!m_depthStencil || !m_depthStencil->resource())
      return;
    D3D11_TEXTURE2D_DESC td;
    m_depthStencil->resource()->GetDesc(&td);
    if (td.SampleDesc.Count > 1 || td.ArraySize > 1 || td.MipLevels > 1
     || (td.Format != DXGI_FORMAT_R24G8_TYPELESS && td.Format != DXGI_FORMAT_D24_UNORM_S8_UINT))
      return;
    D3D11_TEXTURE2D_DESC sd = {};
    if (m_trace9DepthStaging)
      m_trace9DepthStaging->GetDesc(&sd);
    if (!m_trace9DepthStaging || sd.Width != td.Width || sd.Height != td.Height) {
      sd = td;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      if (FAILED(m_device->CreateTexture2D(&sd, nullptr, m_trace9DepthStaging.put())))
        return;
    }
    m_context->CopyResource(m_trace9DepthStaging.get(), m_depthStencil->resource());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(m_trace9DepthStaging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
      return;
    uint64_t sum = 0;
    uint32_t nearest = 0xffffff;
    for (uint32_t y = 0; y < td.Height; y += 4) {
      const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch);
      for (uint32_t x = 0; x < td.Width; x += 4) {
        const uint32_t d = row[x] & 0xffffffu;
        sum += d;
        nearest = (std::min)(nearest, d);
      }
    }
    m_context->Unmap(m_trace9DepthStaging.get(), 0);
    const double samples = double((td.Width + 3) / 4) * ((td.Height + 3) / 4);
    logMessage(LogLevel::Warn, "trace9   depth mean %.6f nearest %.6f", sum / samples / 16777215.0, nearest / 16777215.0);
  }

  void Device::traceDepth9(const char* when) {
    if (!m_trace9 || m_trace9DepthDone || !m_depthStencil || !m_depthStencil->resource())
      return;
    m_trace9DepthDone = true;
    D3D11_TEXTURE2D_DESC td;
    m_depthStencil->resource()->GetDesc(&td);
    if (td.SampleDesc.Count > 1 || td.ArraySize > 1 || td.MipLevels > 1)
      return;
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    Ref<ID3D11Texture2D> staging;
    if (FAILED(m_device->CreateTexture2D(&sd, nullptr, staging.put())))
      return;
    m_context->CopyResource(staging.get(), m_depthStencil->resource());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
      return;
    // 24-bit depth in the low bits (D24S8, X8 variants); others as 16 or 32.
    const bool d16 = td.Format == DXGI_FORMAT_D16_UNORM || td.Format == DXGI_FORMAT_R16_TYPELESS;
    const bool f32 = td.Format == DXGI_FORMAT_D32_FLOAT || td.Format == DXGI_FORMAT_R32_TYPELESS;
    auto depthAt = [&](uint32_t x, uint32_t y) {
      const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
      if (d16) { uint16_t v; std::memcpy(&v, row + x * 2, 2); return v / 65535.0; }
      uint32_t v; std::memcpy(&v, row + x * 4, 4);
      if (f32) { float f; std::memcpy(&f, &v, 4); return double(f); }
      return (v & 0xffffffu) / 16777215.0;
    };
    double minimum = 2.0, maximum = -1.0, sum = 0.0;
    uint64_t zeros = 0, ones = 0;
    std::vector<uint8_t> image(size_t(td.Width) * td.Height * 2);
    for (uint32_t y = 0; y < td.Height; y++) {
      for (uint32_t x = 0; x < td.Width; x++) {
        const double d = depthAt(x, y);
        minimum = (std::min)(minimum, d);
        maximum = (std::max)(maximum, d);
        sum += d;
        zeros += d == 0.0;
        ones += d == 1.0;
        // 16 bits, big endian (PGM).
        const uint16_t v = uint16_t((std::max)(0.0, (std::min)(1.0, d)) * 65535.0 + 0.5);
        image[(size_t(y) * td.Width + x) * 2] = uint8_t(v >> 8);
        image[(size_t(y) * td.Width + x) * 2 + 1] = uint8_t(v);
      }
    }
    const double probe = m_trace9X >= 0 && uint32_t(m_trace9X) < td.Width && uint32_t(m_trace9Y) < td.Height
      ? depthAt(m_trace9X, m_trace9Y) : -1.0;
    m_context->Unmap(staging.get(), 0);
    logMessage(LogLevel::Warn, "trace9   depth %s: %ux%u format %u min %.7f max %.7f mean %.7f, %llu at 0, %llu at 1, probe %.7f",
      when, td.Width, td.Height, unsigned(td.Format), minimum, maximum, sum / (double(td.Width) * td.Height),
      static_cast<unsigned long long>(zeros), static_cast<unsigned long long>(ones), probe);
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&glitchReportsEnabled), &self);
    if (self && GetModuleFileNameW(self, path, MAX_PATH)) {
      if (wchar_t* name = wcsrchr(path, L'\\')) {
        wcscpy_s(name + 1, MAX_PATH - (name + 1 - path), L"dxup_trace9_depth.pgm");
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"wb") == 0 && f) {
          std::fprintf(f, "P5 %u %u 65535 ", td.Width, td.Height);
          std::fwrite(image.data(), 1, image.size(), f);
          std::fclose(f);
        }
      }
    }
  }

  void Device::traceDraw9(const DrawArgs& a) {
    char line[1024];
    int n = sprintf_s(line, "trace9 #%u type %u %s count %u start %u base %d min %u verts %u%s vs %016llX ps %016llX",
      ++m_trace9Draw, unsigned(a.type), a.indexed ? "indexed" : "", a.indexed ? a.indexCount : a.vertexCount,
      a.indexed ? a.startIndex : a.startVertex, a.baseVertex, a.minIndex, a.vertexCount, a.userVertices ? " UP" : "",
      static_cast<unsigned long long>(m_vertexShader ? m_vertexShader->hash() : 0),
      static_cast<unsigned long long>(m_pixelShader ? m_pixelShader->hash() : 0));
    (void)n;
    logMessage(LogLevel::Warn, "%s", line);
    {
      // Targets and the states that decide which pixels pass.
      const Texture* rt = m_renderTargets[0];
      const Texture* ds = m_depthStencil;
      const uint32_t rtSub = m_renderTargetSubresources[0];
      logMessage(LogLevel::Warn, "trace9   state rt %p %ux%u fmt %u ds %p %ux%u fmt %u z %u func %u write %u cw %x cull %u "
        "stencil %u func %u ref %u twosided %u alphatest %u blend %u vp %u,%u %ux%u %g-%g scissor %u %d,%d-%d,%d bias %08x %08x",
        static_cast<const void*>(rt), rt ? rt->width(rtSub) : 0, rt ? rt->height(rtSub) : 0, rt ? uint32_t(rt->desc().format) : 0,
        static_cast<const void*>(ds), ds ? ds->width(0) : 0, ds ? ds->height(0) : 0, ds ? uint32_t(ds->desc().format) : 0,
        m_renderStates[RsZEnable], m_renderStates[RsZFunc], m_renderStates[RsZWriteEnable], m_renderStates[RsColorWriteEnable],
        m_renderStates[RsCullMode], m_renderStates[RsStencilEnable], m_renderStates[RsStencilFunc], m_renderStates[RsStencilRef],
        m_renderStates[RsTwoSidedStencilMode], m_renderStates[RsAlphaTestEnable], m_renderStates[RsAlphaBlendEnable],
        m_viewport.x, m_viewport.y, m_viewport.width, m_viewport.height, m_viewport.minZ, m_viewport.maxZ,
        m_renderStates[RsScissorTestEnable], m_scissor.left, m_scissor.top, m_scissor.right, m_scissor.bottom,
        m_renderStates[RsDepthBias], m_renderStates[RsSlopeScaleDepthBias]);
    }
    if (m_decl) {
      std::string decl;
      for (const auto& e : m_decl->elements()) {
        char text[64];
        sprintf_s(text, " s%u+%u t%u u%u/%u", e.stream, e.offset, e.type, e.usage, e.usageIndex);
        decl += text;
      }
      logMessage(LogLevel::Warn, "trace9   decl%s", decl.c_str());
    }
    for (uint32_t stream = 0; stream < kMaxStreams; stream++) {
      if (!m_decl || !((m_decl->streamMask() >> stream) & 1))
        continue;
      const Stream& s = m_streams[stream];
      std::string bytes;
      const uint8_t* data = nullptr;
      uint32_t first = 0, size = 0;
      if (stream == 0 && a.userVertices) {
        data = static_cast<const uint8_t*>(a.userVertices);
        size = a.userStride * 2;
      } else if (s.buffer) {
        const int64_t vertex = (s.frequency & kInstanceData) ? 0 : a.indexed ? int64_t(a.baseVertex) + a.minIndex : a.startVertex;
        first = uint32_t((std::max<int64_t>)(0, int64_t(s.offset) + vertex * s.stride));
        data = s.buffer->data();
        size = s.buffer->size();
      }
      for (uint32_t i = 0; data && i < 32 && first + i < size; i++) {
        char text[4];
        sprintf_s(text, "%02x", data[first + i]);
        bytes += text;
        if ((i & 3) == 3) bytes += ' ';
      }
      logMessage(LogLevel::Warn, "trace9   stream %u buffer %p size %u offset %u stride %u freq 0x%x%s: %s", stream,
        static_cast<void*>(s.buffer), s.buffer ? s.buffer->size() : 0, s.offset, s.stride, s.frequency,
        s.buffer && s.buffer->dynamic() ? " dynamic" : "", bytes.c_str());
    }
    for (uint32_t sampler = 0; sampler < kSamplerCount; sampler++) {
      Texture* t = m_textures[sampler];
      if (!t)
        continue;
      const TextureDesc& d = t->desc();
      const auto& data = t->levelData(0);
      uint64_t sum = 0;
      uint32_t nonzero = 0;
      for (size_t i = 0; i < data.size(); i++) { sum += data[i]; nonzero += data[i] != 0; }
      logMessage(LogLevel::Warn, "trace9   s%u texture %p %ux%u levels %u format %u%s%s%s, level 0 copy %zu bytes, %u nonzero, sum %llu",
        sampler, static_cast<void*>(t), d.width, d.height, d.levels, uint32_t(d.format), d.renderTarget ? " target" : "",
        d.depthStencil ? " depth" : "", d.cube ? " cube" : "", data.size(), nonzero, static_cast<unsigned long long>(sum));
    }
    for (int pixel = 0; pixel < 2; pixel++) {
      const Shader* shader = pixel ? m_pixelShader : m_vertexShader;
      if (!shader)
        continue;
      const uint32_t floats = (std::min)(shader->info().floatConstantCount, 32u);
      std::string text;
      for (uint32_t i = 0; i < floats; i++) {
        char c[96];
        const float* f = m_constants[pixel].f[i];
        sprintf_s(c, " c%u(%g %g %g %g)", i, f[0], f[1], f[2], f[3]);
        text += c;
      }
      logMessage(LogLevel::Warn, "trace9   %s constants%s", pixel ? "pixel" : "vertex", text.c_str());
    }
  }

  // Occlusion queries that saw no pixel at all, frame after frame: the game
  // then hides nearly everything it tests (a wrong depth buffer or vertex
  // shader before the tests). Games normally see some of their tests.
  void Device::logOcclusionResults() {
    const uint32_t results = m_occlusion.results, zeros = m_occlusion.zeros;
    m_occlusion.results = m_occlusion.zeros = 0;
    if (results < 16)
      return;
    if (zeros * 20 < results * 19) {
      m_occlusion.blindFrames = 0;
      return;
    }
    if (++m_occlusion.blindFrames == 30 && !m_occlusion.logged) {
      m_occlusion.logged = true;
      logMessage(LogLevel::Warn, "glitch: present %u: for 30 frames nearly every occlusion query saw no pixel (now %u of %u) - "
        "the game hides what it tests: the depth buffer or a vertex shader before the tests is likely wrong", m_nonFinite.presents,
        zeros, results);
    }
  }

  void Device::logDroppedDraws() {
    uint32_t total = 0;
    for (uint32_t count : m_dropped.counts)
      total += count;
    if (!total)
      return;
    // Every frame with any for the first 20, then every 100th.
    m_dropped.frames++;
    if (m_dropped.frames <= 20 || m_dropped.frames % 100 == 0) {
      static const char* const kReasons[DropReasons] = { "without declaration", "vertex shader not compiled",
        "pixel shader not compiled", "no input layout", "data too large", "without index buffer" };
      std::string reasons;
      for (uint32_t i = 0; i < DropReasons; i++) {
        if (!m_dropped.counts[i])
          continue;
        char text[64];
        sprintf_s(text, "%s%u %s", reasons.empty() ? "" : ", ", m_dropped.counts[i], kReasons[i]);
        reasons += text;
      }
      logMessage(LogLevel::Warn, "glitch: present %u: %u draws not drawn (%s)%s", m_nonFinite.presents, total,
        reasons.c_str(), m_dropped.frames == 20 ? "; from now on every 100th such frame" : "");
    }
    std::memset(m_dropped.counts, 0, sizeof(m_dropped.counts));
  }

  void Device::updateShaderConstants(bool pixel) {
    const Shader::Variant* variant = pixel ? m_psVariant : m_vsVariant;
    if (!variant)
      return;
    // The floats the shader reads, in a window of 16-constant steps of the
    // stage's ring: appended without waiting, discarded when it wraps.
    const uint32_t floats = (std::min)(variant->info.floatConstantCount, 256u);
    const uint32_t window = (std::max)(16u, (floats + 15) & ~15u);
    const uint32_t bytes = window * 16;
    uint32_t& offset = m_constantRingOffsets[pixel];
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (offset + bytes > kConstantRingSize) {
      mode = D3D11_MAP_WRITE_DISCARD;
      offset = 0;
    }
    ID3D11Buffer* ring = m_constantRings[pixel].get();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(ring, 0, mode, 0, &mapped)))
      return;
    // Whole 64-byte lines: the ring is write-combined memory, and partly
    // written lines wait in the CPU until the next locked instruction.
    const uint32_t copied = (std::min)((floats + 3) & ~3u, 256u);
    if (glitchReportsEnabled())
      checkConstants(pixel, floats);
    std::memcpy(static_cast<uint8_t*>(mapped.pData) + offset, m_constants[pixel].f, size_t(copied) * 16);
    m_context->Unmap(ring, 0);
    const UINT first = offset / 16, count = window;
    if (pixel)
      m_context1->PSSetConstantBuffers1(0, 1, &ring, &first, &count);
    else
      m_context1->VSSetConstantBuffers1(0, 1, &ring, &first, &count);
    offset += bytes;
    m_shaderConstantsDirty[pixel] = false;
  }

  void Device::updateShaderInts(bool pixel) {
    ID3D11Buffer* buffer = m_shaderIntConstants[pixel].get();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      writeLines(static_cast<uint8_t*>(mapped.pData), m_constants[pixel].i, sizeof(ConstantStore) - offsetof(ConstantStore, i));
      m_context->Unmap(buffer, 0);
    }
    m_shaderIntsDirty[pixel] = false;
  }

  void Device::updateShaderFixedConstants() {
    const auto& rs = m_renderStates;
    ShaderFixedConstants c;
    std::memset(&c, 0, sizeof(c));
    c.alphaTest[0] = float(rs[RsAlphaRef] & 0xff) / 255.0f;
    c.alphaTest[1] = float(rs[RsAlphaFunc]);
    c.alphaTest[2] = rs[RsAlphaTestEnable] ? 1.0f : 0.0f;
    colorToFloats(rs[RsFogColor], c.fogColor);
    c.fogParams[0] = asFloat(rs[RsFogStart]);
    c.fogParams[1] = asFloat(rs[RsFogEnd]);
    c.fogParams[2] = asFloat(rs[RsFogDensity]);
    c.fogParams[3] = 1.0f / (std::max)(c.fogParams[1] - c.fogParams[0], 1e-6f);
    c.flags[0] = rs[RsFogEnable] ? 1 : 0;
    c.flags[1] = rs[RsFogTableMode] <= 3 ? rs[RsFogTableMode] : 0;
    // D3DCMP_NEVER (1) to ALWAYS (8) minus one: less, equal, greater bits;
    // not equal and always pass unordered (NaN) values too.
    const uint32_t func = rs[RsAlphaFunc];
    if (rs[RsAlphaTestEnable] && func >= 1 && func <= 7)
      c.flags[2] = 0x10u | ((func - 1) & 7) | (func == 6 ? 8u : 0u);
    for (uint32_t stage = 0; stage < kFixedStages; stage++) {
      const auto& tss = m_stageStates[stage];
      c.bumpMatrix[stage][0] = asFloat(tss[TssBumpEnvMat00]);
      c.bumpMatrix[stage][1] = asFloat(tss[TssBumpEnvMat01]);
      c.bumpMatrix[stage][2] = asFloat(tss[TssBumpEnvMat10]);
      c.bumpMatrix[stage][3] = asFloat(tss[TssBumpEnvMat11]);
      c.bumpLuminance[stage][0] = asFloat(tss[TssBumpEnvLScale]);
      c.bumpLuminance[stage][1] = asFloat(tss[TssBumpEnvLOffset]);
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_context->Map(m_shaderFixedConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      writeLines(static_cast<uint8_t*>(mapped.pData), &c, sizeof(c));
      m_context->Unmap(m_shaderFixedConstants.get(), 0);
    }
    m_shaderFixedDirty = false;
  }

  void Device::bindTargets() {
    const bool srgb = m_renderStates[RsSrgbWriteEnable] != 0;
    ID3D11RenderTargetView* rtvs[kMaxRenderTargets] = {};
    UINT count = 0;
    for (uint32_t i = 0; i < kMaxRenderTargets; i++) {
      Texture* target = m_renderTargets[i];
      if (target && !target->nullTarget())
        rtvs[i] = target->rtv(m_renderTargetSubresources[i], srgb);
      if (rtvs[i])
        count = i + 1;
    }
    ID3D11DepthStencilView* dsv = m_depthStencil ? m_depthStencil->dsv() : nullptr;
    // Unbind textures that are now targets.
    ID3D11ShaderResourceView* nulls[24] = {};
    m_context->PSSetShaderResources(0, 24, nulls);
    m_context->VSSetShaderResources(0, 4, nulls);
    std::memset(m_boundViews, 0, sizeof(m_boundViews));
    std::memset(m_boundVertexViews, 0, sizeof(m_boundVertexViews));
    m_textureDirtyMask = (1u << kSamplerCount) - 1;
    m_context->OMSetRenderTargets(count, rtvs, dsv);
    // Direct3D 9 samples pixel centers at integer coordinates, D3D11 at
    // half: the viewport moves half a pixel.
    D3D11_VIEWPORT vp = { float(m_viewport.x) + 0.5f, float(m_viewport.y) + 0.5f, float(m_viewport.width),
                          float(m_viewport.height), m_viewport.minZ, m_viewport.maxZ };
    m_context->RSSetViewports(1, &vp);
    D3D11_RECT scissor = { m_scissor.left, m_scissor.top, m_scissor.right, m_scissor.bottom };
    m_context->RSSetScissorRects(1, &scissor);
    m_targetsDirty = false;
  }

  void Device::selectShaders() {
    // Pixel stage first: the vertex shader writes what it reads.
    uint32_t pixelVaryings = kFixedPixelVaryings;
    m_psVariant = nullptr;
    if (m_pixelShader) {
      TranslateOptions9 options;
      uint8_t types[16] = {};
      if (m_pixelShader->info().major == 1) {
        for (uint32_t i = 0; i < 16; i++) {
          const uint32_t type = (m_psTextureTypes >> (i * 2)) & 3;
          types[i] = type == 2 ? SamplerCube : type == 3 ? SamplerVolume : Sampler2D;
        }
        options.boundSamplerTypes = types;
      }
      uint16_t shadow = 0;
      for (uint32_t i = 0; i < 16; i++)
        if ((m_shadowMask >> i) & 1 && (m_pixelShader->info().samplerTypes[i] == Sampler2D || m_pixelShader->info().major == 1))
          shadow |= uint16_t(1u << i);
      options.shadowSamplers = shadow;
      m_psVariant = m_pixelShader->variant(options);
      if (m_psVariant)
        pixelVaryings = m_psVariant->info.varyingCount;
    }
    m_vsVariant = nullptr;
    if (m_vertexShader) {
      if (!m_pixelShader)
        m_fixedPixelsWithShaders = true;
      TranslateOptions9 options;
      options.minVaryings = pixelVaryings;
      // How the inputs' bits become floats (dxup_inputModes): integers of
      // the declaration's UBYTE4 and SHORT types.
      uint32_t inputModes = 0, inputWOne = 0;
      if (m_decl) {
        for (const auto& in : m_vertexShader->info().inputs) {
          for (const auto& e : m_decl->elements()) {
            if (e.usage != in.usage || e.usageIndex != in.usageIndex)
              continue;
            if (declUint(e.type)) inputModes |= 2u << (in.reg * 2);
            if (declSint(e.type)) inputModes |= 1u << (in.reg * 2);
            if (e.type == DeclDec3N) inputModes |= 3u << (in.reg * 2);
            if (declWOne(e.type)) inputWOne |= 1u << in.reg;
            break;
          }
        }
      }
      if (m_constants[0].limits[1] != inputModes || m_constants[0].limits[2] != inputWOne) {
        m_constants[0].limits[1] = inputModes;
        m_constants[0].limits[2] = inputWOne;
        m_shaderIntsDirty[0] = true;
      }
      options.clipPlanes = uint8_t(m_renderStates[RsClipPlaneEnable] & 0x3f);
      m_vsVariant = m_vertexShader->variant(options);
    }
    // The fixed-function pixel stage binds textures by type.
    const bool ffpPixel = !m_psVariant;
    if (ffpPixel != m_boundFfpPixel) {
      m_boundFfpPixel = ffpPixel;
      m_textureDirtyMask = m_samplerDirtyMask = (1u << kSamplerCount) - 1;
    }
  }

  void Device::bindTextures() {
    const bool ffpPixel = !m_psVariant;
    auto usable = [&](Texture* texture) {
      if (!texture || texture == m_depthStencil)
        return false;
      for (Texture* target : m_renderTargets)
        if (target == texture) return false;
      return true;
    };
    if (m_textureDirtyMask & ((1u << kPixelSamplers) - 1)) {
      ID3D11ShaderResourceView* views[24] = {};
      if (ffpPixel) {
        for (uint32_t stage = 0; stage < kFixedStages; stage++) {
          Texture* texture = m_textures[stage];
          const uint32_t type = textureTypeOf(texture);
          if (type && usable(texture))
            views[(type - 1) * 8 + stage] = texture->srv(m_samplerStatesValues[stage][SampSrgbTexture] != 0);
        }
      } else {
        for (uint32_t sampler = 0; sampler < kPixelSamplers; sampler++) {
          Texture* texture = m_textures[sampler];
          if (textureTypeOf(texture) && usable(texture))
            views[sampler] = texture->srv(m_samplerStatesValues[sampler][SampSrgbTexture] != 0);
        }
      }
      // Runs of changed slots in one call each.
      uint32_t slot = 0;
      while (slot < 24) {
        if (views[slot] == m_boundViews[slot]) {
          slot++;
          continue;
        }
        uint32_t end = slot + 1;
        while (end < 24 && views[end] != m_boundViews[end])
          end++;
        m_context->PSSetShaderResources(slot, end - slot, &views[slot]);
        std::memcpy(&m_boundViews[slot], &views[slot], (end - slot) * sizeof(views[0]));
        slot = end;
      }
    }
    if (m_textureDirtyMask >> kVertexSamplerBase) {
      ID3D11ShaderResourceView* views[4] = {};
      for (uint32_t i = 0; i < 4; i++) {
        Texture* texture = m_textures[kVertexSamplerBase + i];
        if (textureTypeOf(texture) && usable(texture))
          views[i] = texture->srv(m_samplerStatesValues[kVertexSamplerBase + i][SampSrgbTexture] != 0);
      }
      if (std::memcmp(views, m_boundVertexViews, sizeof(views)) != 0) {
        m_context->VSSetShaderResources(0, 4, views);
        std::memcpy(m_boundVertexViews, views, sizeof(views));
      }
    }
    m_textureDirtyMask = 0;

    if (m_samplerDirtyMask) {
      const uint32_t pixelSamplers = ffpPixel ? kFixedStages : kPixelSamplers;
      for (uint32_t sampler = 0; sampler < kSamplerCount; sampler++) {
        if (!(m_samplerDirtyMask & (1u << sampler)))
          continue;
        const bool vertexSampler = sampler >= kVertexSamplerBase;
        if (!vertexSampler && sampler >= pixelSamplers)
          continue;
        const bool compare = !ffpPixel && !vertexSampler && ((m_shadowMask >> sampler) & 1);
        ID3D11SamplerState* state = d3dSampler(sampler, compare);
        if (state == m_boundSamplers[sampler])
          continue;
        m_boundSamplers[sampler] = state;
        if (vertexSampler)
          m_context->VSSetSamplers(sampler - kVertexSamplerBase, 1, &state);
        else
          m_context->PSSetSamplers(sampler, 1, &state);
      }
      m_samplerDirtyMask = 0;
    }
  }

  void Device::applyState() {
    if (m_targetsDirty) {
      bindTargets();
      m_texturesPending = true;
    }
    // Pixels the application wrote into the targets and textures come
    // before the draw (all bound ones: shaders may change before the next).
    if (m_texturesPending) {
      m_texturesPending = false;
      for (Texture* target : m_renderTargets)
        if (target)
          target->flush();
      for (Texture* texture : m_textures)
        if (texture)
          texture->flush();
    }
    if (m_dirty & DirtyShaders)
      selectShaders();
    const bool ffpVertex = !m_vertexShader;
    const bool ffpPixel = !m_psVariant;
    if (m_dirty & DirtyShaders) {
      m_context->VSSetShader(m_vsVariant ? m_vsVariant->vs.get() : m_ffpVs.get(), nullptr, 0);
      m_context->PSSetShader(m_psVariant ? m_psVariant->ps.get() : m_ffpPs.get(), nullptr, 0);
      ID3D11Buffer* vsConstants[4] = { m_vertexConstants.get(), nullptr, nullptr, m_transformConstants.get() };
      if (!ffpVertex) {
        vsConstants[0] = nullptr;
        vsConstants[2] = m_shaderIntConstants[0].get();
        vsConstants[3] = m_clipPlaneConstants.get();
        m_shaderConstantsDirty[0] = true;   // its window binds b0
      }
      ID3D11Buffer* psConstants[3] = { nullptr, m_pixelConstants.get(), nullptr };
      if (!ffpPixel) {
        psConstants[1] = m_shaderFixedConstants.get();
        psConstants[2] = m_shaderIntConstants[1].get();
        m_shaderConstantsDirty[1] = true;
      }
      m_context->VSSetConstantBuffers(0, 4, vsConstants);
      m_context->PSSetConstantBuffers(0, 3, psConstants);
    }
    if (ffpVertex) {
      if (m_vertexConstantsDirty)
        updateVertexConstants();
      if (m_transformConstantsDirty)
        updateTransformConstants();
    } else {
      if (m_shaderConstantsDirty[0])
        updateShaderConstants(false);
      if (m_shaderIntsDirty[0])
        updateShaderInts(false);
      if (m_clipPlanesDirty && m_vsVariant && (m_renderStates[RsClipPlaneEnable] & 0x3f)) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(m_context->Map(m_clipPlaneConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
          writeLines(static_cast<uint8_t*>(mapped.pData), m_clipPlanes.data(), sizeof(m_clipPlanes));
          m_context->Unmap(m_clipPlaneConstants.get(), 0);
        }
        m_clipPlanesDirty = false;
      }
    }
    if (ffpPixel) {
      if (m_pixelConstantsDirty)
        updatePixelConstants();
    } else {
      if (m_shaderConstantsDirty[1])
        updateShaderConstants(true);
      if (m_shaderIntsDirty[1])
        updateShaderInts(true);
      if (m_shaderFixedDirty)
        updateShaderFixedConstants();
    }

    if (!m_defaultStreamBound) {
      ID3D11Buffer* defaults = m_defaultVertex.get();
      const UINT zero = 0;
      m_context->IASetVertexBuffers(kDefaultStream, 1, &defaults, &zero, &zero);
      m_defaultStreamBound = true;
    }
    if (m_dirty & (DirtyLayout | DirtyShaders)) {
      ID3D11InputLayout* layout = inputLayout();
      if (layout != m_boundLayout) {
        m_context->IASetInputLayout(layout);
        m_boundLayout = layout;
      }
    }
    bindTextures();
    if (m_dirty & DirtyBlend) {
      float factor[4];
      colorToFloats(m_renderStates[RsBlendFactor], factor);
      m_context->OMSetBlendState(blendState(), factor, 0xffffffffu);
    }
    if (m_dirty & (DirtyDepth | DirtyStencilRef))
      m_context->OMSetDepthStencilState(depthStencilState(), m_renderStates[RsStencilRef] & 0xff);
    if (m_dirty & DirtyRaster)
      m_context->RSSetState(rasterizerState());
    m_dirty = 0;
  }

  // A ring for one draw's data larger than it: twice as large, enough for
  // it (256 MB at most). Bindings of the old one are set again by pointer.
  bool Device::growRing(Ref<ID3D11Buffer>& ring, uint32_t& ringSize, uint32_t& ringOffset, uint32_t size, UINT bind) {
    if (size > (256u << 20))
      return false;
    uint32_t grown = ringSize;
    while (grown < size)
      grown *= 2;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = grown;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = bind;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* buffer = nullptr;
    if (FAILED(m_device->CreateBuffer(&bd, nullptr, &buffer)))
      return false;
    logMessage(LogLevel::Info, "D3D9: %s ring %u -> %u KB for a draw of %u KB", bind == D3D11_BIND_INDEX_BUFFER ? "index" : "vertex",
      ringSize >> 10, grown >> 10, size >> 10);
    *ring.put() = buffer;
    ringSize = grown;
    ringOffset = grown;   // the next map discards
    return true;
  }

  uint32_t Device::uploadVertices(const void* data, uint32_t size) {
    if (((size + 63) & ~63u) > m_vertexRingSize
     && !growRing(m_vertexRing, m_vertexRingSize, m_vertexRingOffset, (size + 63) & ~63u, D3D11_BIND_VERTEX_BUFFER))
      return UINT32_MAX;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (m_vertexRingOffset + ((size + 63) & ~63u) > m_vertexRingSize) {
      mode = D3D11_MAP_WRITE_DISCARD;
      m_vertexRingOffset = 0;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(m_vertexRing.get(), 0, mode, 0, &mapped)))
      return UINT32_MAX;
    writeLines(static_cast<uint8_t*>(mapped.pData) + m_vertexRingOffset, data, size);
    m_context->Unmap(m_vertexRing.get(), 0);
    const uint32_t offset = m_vertexRingOffset;
    m_vertexRingOffset += (size + 63) & ~63u;
    return offset;
  }

  uint32_t Device::uploadIndices(const void* data, uint32_t size) {
    if (((size + 63) & ~63u) > m_indexRingSize
     && !growRing(m_indexRing, m_indexRingSize, m_indexRingOffset, (size + 63) & ~63u, D3D11_BIND_INDEX_BUFFER))
      return UINT32_MAX;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (m_indexRingOffset + ((size + 63) & ~63u) > m_indexRingSize) {
      mode = D3D11_MAP_WRITE_DISCARD;
      m_indexRingOffset = 0;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(m_indexRing.get(), 0, mode, 0, &mapped)))
      return UINT32_MAX;
    writeLines(static_cast<uint8_t*>(mapped.pData) + m_indexRingOffset, data, size);
    m_context->Unmap(m_indexRing.get(), 0);
    const uint32_t offset = m_indexRingOffset;
    m_indexRingOffset += (size + 63) & ~63u;
    return offset;
  }

  namespace {
    D3D11_PRIMITIVE_TOPOLOGY topology(PrimitiveType type) {
      switch (type) {
        case PrimitivePointList:     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        case PrimitiveLineList:      return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrimitiveLineStrip:     return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case PrimitiveTriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        default:                     return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      }
    }

    // Triangle fans as lists: (0, i, i + 1).
    template<typename T>
    std::vector<uint32_t> fanToList(const T* indices, uint32_t count) {
      std::vector<uint32_t> list;
      if (count < 3)
        return list;
      list.reserve(size_t(count - 2) * 3);
      for (uint32_t i = 1; i + 1 < count; i++) {
        list.push_back(indices ? uint32_t(indices[0]) : 0u);
        list.push_back(indices ? uint32_t(indices[i]) : i);
        list.push_back(indices ? uint32_t(indices[i + 1]) : i + 1);
      }
      return list;
    }
  }

  void Device::markTargetsWritten() {
    for (Texture* target : m_renderTargets)
      if (target)
        target->markGpuWritten();
  }

  // Games give vertex ranges their indices do not keep to (Direct3D 9
  // drivers do not need them); DXUP uploads and streams that range only, so
  // vertices beyond it were missing: the range the indices reach instead.
  // Not for application vertices: Direct3D 9 copies the range given (and
  // memory beyond it may not be there).
  void Device::widenVertexRange(DrawArgs& a) {
    if (a.userVertices)
      return;
    uint32_t lo = 0, hi = 0;
    if (a.userIndices) {
      lo = UINT32_MAX;
      for (uint32_t i = 0; i < a.indexCount; i++) {
        const uint32_t index = a.userIndex32 ? static_cast<const uint32_t*>(a.userIndices)[i]
                                             : static_cast<const uint16_t*>(a.userIndices)[i];
        lo = (std::min)(lo, index);
        hi = (std::max)(hi, index);
      }
    } else if (!m_indices || !m_indices->indexRange(a.startIndex, a.indexCount, m_index32, lo, hi)) {
      return;
    }
    const uint32_t end = a.minIndex + a.vertexCount;
    if (lo >= a.minIndex && hi < end)
      return;
    if (m_rangeReports < 8) {
      m_rangeReports++;
      logMessage(LogLevel::Warn, "D3D9: a draw's indices reach vertices %u to %u, the game gave %u to %u: drawn with the vertices the indices reach%s",
        lo, hi, a.minIndex, end ? end - 1 : 0, m_rangeReports == 8 ? " (no more such lines)" : "");
    }
    const uint32_t first = (std::min)(lo, a.minIndex);
    a.vertexCount = (std::max)(hi + 1, end) - first;
    a.minIndex = first;
  }

  void Device::drawWith(const DrawArgs& args) {
    DrawArgs a = args;
    if (a.indexed && a.indexCount)
      widenVertexRange(a);
    if (m_trace9)
      traceDraw9(a);
    struct ProbeAfter {
      Device* device;
      ~ProbeAfter() {
        if (!device->m_trace9)
          return;
        device->traceProbe9();
        if (!device->m_trace9DepthDone && device->m_renderStates[RsZEnable] && device->m_renderStates[RsZWriteEnable])
          device->traceDepthMean9();
      }
    } probeAfter{ this };
    if (!m_decl)
      return dropDraw(DropDeclaration);
    // Application vertices into the ring first: its offset binds stream 0.
    uint32_t userOffset = 0;
    if (a.userVertices) {
      const uint32_t vertices = a.indexed ? a.minIndex + a.vertexCount : a.vertexCount;
      userOffset = uploadVertices(a.userVertices, vertices * a.userStride);
      if (userOffset == UINT32_MAX)
        return dropDraw(DropUpload, vertices * a.userStride);
    }
    markTargetsWritten();
    applyState();
    if (m_vertexShader && !m_vsVariant)
      return dropDraw(DropVertexShader);
    if (m_pixelShader && !m_psVariant)
      return dropDraw(DropPixelShader);
    if (!m_boundLayout)
      return dropDraw(DropInputLayout);

    // Instances: stream 0 counts them, others step per instance.
    uint32_t instances = 1;
    if (a.indexed && (m_streams[0].frequency & kIndexedData))
      instances = (std::max)(1u, m_streams[0].frequency & 0x3fffffffu);

    // Streams: the ranges the draw reads uploaded, changed bindings set.
    // Dynamic buffers put that range into the vertex ring; the draw is then
    // rebased to its first vertex, and the other streams with it.
    const uint32_t mask = m_decl->streamMask() & ((1u << kMaxStreams) - 1);
    const int64_t firstVertex = a.indexed ? int64_t(a.baseVertex) + a.minIndex : int64_t(a.startVertex);
    bool rebase = false;
    if (!a.userVertices && firstVertex >= 0) {
      for (uint32_t bits = mask; bits && !rebase; bits &= bits - 1) {
        const Stream& s = m_streams[lowestBit(bits)];
        rebase = s.buffer && s.buffer->dynamic() && !(s.frequency & kInstanceData);
      }
    }
    for (uint32_t bits = mask; bits; bits &= bits - 1) {
      const uint32_t stream = lowestBit(bits);
      ID3D11Buffer* buffer = nullptr;
      UINT offset = 0, stride = 0;
      if (stream == 0 && a.userVertices) {
        buffer = m_vertexRing.get();
        offset = userOffset;
        stride = a.userStride;
      } else if (Stream& s = m_streams[stream]; s.buffer) {
        buffer = s.buffer->resource();
        offset = s.offset;
        stride = s.stride;
        const bool instanced = (s.frequency & kInstanceData) != 0;
        if (instanced) {
          s.buffer->prepare(s.offset, 0);
        } else if (rebase && s.buffer->dynamic()) {
          s.buffer->noteStreamedDraw();
          const uint64_t begin = s.offset + uint64_t(firstVertex) * stride;
          const uint64_t bytes = uint64_t(a.vertexCount) * stride;
          const uint32_t ringOffset = begin + bytes <= s.buffer->size() && bytes
            ? uploadVertices(s.buffer->data() + begin, uint32_t(bytes)) : UINT32_MAX;
          if (ringOffset != UINT32_MAX) {
            buffer = m_vertexRing.get();
            offset = ringOffset;
          } else {
            s.buffer->prepare(uint32_t(begin), uint32_t(bytes));
            offset = UINT(begin);
          }
        } else if (a.indexed) {
          s.buffer->prepare(s.offset + uint32_t((std::max<int64_t>)(0, firstVertex) * stride), a.vertexCount * stride);
          if (rebase) offset = UINT(s.offset + uint64_t(firstVertex) * stride);
        } else {
          s.buffer->prepare(s.offset + a.startVertex * stride, a.vertexCount * stride);
          if (rebase) offset = UINT(s.offset + uint64_t(firstVertex) * stride);
        }
      }
      if (buffer != m_boundStreams[stream] || offset != m_boundStreamOffsets[stream] || stride != m_boundStreamStrides[stream]) {
        m_context->IASetVertexBuffers(stream, 1, &buffer, &stride, &offset);
        m_boundStreams[stream] = buffer;
        m_boundStreamOffsets[stream] = offset;
        m_boundStreamStrides[stream] = stride;
      }
    }

    ID3D11GeometryShader* gs = nullptr;
    if (a.type == PrimitivePointList && expandPoints()) {
      gs = pointShader(m_vsVariant ? m_vsVariant->info.varyingCount : 11u, m_vsVariant ? m_vsVariant->clipPlanes != 0 : true);
      updatePointConstants();
    }
    if (gs != m_boundGs) {
      m_context->GSSetShader(gs, nullptr, 0);
      if (gs) {
        ID3D11Buffer* constants = m_pointConstants.get();
        m_context->GSSetConstantBuffers(0, 1, &constants);
      }
      m_boundGs = gs;
    }

    if (a.type == PrimitiveTriangleFan) {
      std::vector<uint32_t> list;
      int32_t base = 0;
      if (a.indexed) {
        const void* source = a.userIndices;
        bool index32 = a.userIndex32;
        if (!source && m_indices) {
          index32 = m_index32;
          source = m_indices->data() + size_t(a.startIndex) * (index32 ? 4 : 2);
        }
        if (!source)
          return dropDraw(DropIndices);
        list = index32 ? fanToList(static_cast<const uint32_t*>(source), a.indexCount)
                       : fanToList(static_cast<const uint16_t*>(source), a.indexCount);
        base = rebase ? int32_t(int64_t(a.baseVertex) - firstVertex) : a.baseVertex;
      } else {
        list = fanToList(static_cast<const uint32_t*>(nullptr), a.vertexCount);
        base = rebase ? 0 : int32_t(a.startVertex);
      }
      if (list.empty())
        return;
      const uint32_t indexOffset = uploadIndices(list.data(), uint32_t(list.size() * 4));
      if (indexOffset == UINT32_MAX)
        return dropDraw(DropUpload, uint32_t(list.size() * 4));
      m_context->IASetIndexBuffer(m_indexRing.get(), DXGI_FORMAT_R32_UINT, indexOffset);
      m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      if (instances > 1)
        m_context->DrawIndexedInstanced(UINT(list.size()), instances, 0, base, 0);
      else
        m_context->DrawIndexed(UINT(list.size()), 0, base);
      return;
    }
    m_context->IASetPrimitiveTopology(topology(a.type));
    if (!a.indexed) {
      m_context->Draw(a.vertexCount, rebase ? 0 : a.startVertex);
      return;
    }
    const INT baseVertex = rebase ? INT(int64_t(a.baseVertex) - firstVertex) : a.baseVertex;
    UINT startIndex = a.startIndex;
    const uint32_t indexSize = m_index32 ? 4 : 2;
    if (!a.userIndices && m_indices && m_indices->dynamic()
     && uint64_t(a.startIndex + a.indexCount) * indexSize <= m_indices->size()) {
      // Dynamic indices: the range drawn through the index ring.
      m_indices->noteStreamedDraw();
      const uint32_t indexOffset = uploadIndices(m_indices->data() + size_t(a.startIndex) * indexSize, a.indexCount * indexSize);
      if (indexOffset != UINT32_MAX) {
        m_context->IASetIndexBuffer(m_indexRing.get(), m_index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, indexOffset);
        if (instances > 1)
          m_context->DrawIndexedInstanced(a.indexCount, instances, 0, baseVertex, 0);
        else
          m_context->DrawIndexed(a.indexCount, 0, baseVertex);
        return;
      }
    }
    if (a.userIndices) {
      const uint32_t size = a.indexCount * (a.userIndex32 ? 4 : 2);
      const uint32_t indexOffset = uploadIndices(a.userIndices, size);
      if (indexOffset == UINT32_MAX)
        return dropDraw(DropUpload, size);
      m_context->IASetIndexBuffer(m_indexRing.get(), a.userIndex32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, indexOffset);
      startIndex = 0;
    } else {
      if (!m_indices)
        return dropDraw(DropIndices);
      m_indices->prepare(a.startIndex * (m_index32 ? 4 : 2), a.indexCount * (m_index32 ? 4 : 2));
      m_context->IASetIndexBuffer(m_indices->resource(), m_index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
    }
    if (instances > 1)
      m_context->DrawIndexedInstanced(a.indexCount, instances, startIndex, baseVertex, 0);
    else
      m_context->DrawIndexed(a.indexCount, startIndex, baseVertex);
  }

  bool Device::expandPoints() const {
    const auto& rs = m_renderStates;
    return rs[RsPointSpriteEnable] || rs[RsPointScaleEnable] || asFloat(rs[RsPointSize]) != 1.0f
        || (m_vsVariant && m_vsVariant->info.writesPointSize);
  }

  ID3D11GeometryShader* Device::pointShader(uint32_t slots, bool clip) {
    const uint32_t key = slots | (clip ? 0x100u : 0u);
    auto& shader = m_pointShaders[key];
    if (shader)
      return shader;
    // The vertex stage's outputs in, four of them out: a quad around the
    // point, its size in pixels, sprites with coordinates over the quad.
    std::string s =
      "cbuffer Points : register(b0) {\n"
      "  float4 viewport;   // width, height, 1 / width, 1 / height\n"
      "  float4 sizes;      // size, minimum, maximum, sprites\n"
      "  float4 scale;      // A, B, C, scaled (fixed function)\n"
      "  uint4  flags;      // fixed-function vertex stage\n"
      "};\n"
      "struct V {\n  float4 position : SV_Position;\n";
    for (uint32_t i = 0; i < slots; i++)
      s += "  float4 s" + std::to_string(i) + " : " + varyingSemantic9(i) + ";\n";
    if (clip)
      s += "  float4 clip0 : SV_ClipDistance0;\n  float2 clip1 : SV_ClipDistance1;\n";
    s += "};\n"
         "[maxvertexcount(4)]\n"
         "void main(point V input[1], inout TriangleStream<V> stream) {\n"
         "  V v = input[0];\n"
         "  float size = sizes.x;\n";
    if (slots)
      s += "  if (flags.x != 0u) {\n"
           "    if (scale.w != 0.0f) {\n"
           "      float d = v.s0.z;\n"
           "      size = viewport.y * sizes.x * sqrt(1.0f / max(scale.x + scale.y * d + scale.z * d * d, 1e-20f));\n"
           "    }\n"
           "  } else if (v.s0.w == 1.0f) {\n"
           "    size = v.s0.z;\n"
           "  }\n";
    s += "  size = clamp(size, sizes.y, sizes.z);\n"
         "  const float2 extent = size * viewport.zw * v.position.w;\n"
         "  const float2 corners[4] = { float2(-1.0f, 1.0f), float2(1.0f, 1.0f), float2(-1.0f, -1.0f), float2(1.0f, -1.0f) };\n"
         "  [unroll] for (uint i = 0; i < 4; i++) {\n"
         "    V o = v;\n"
         "    o.position.xy += corners[i] * extent;\n"
         "    if (sizes.w != 0.0f) {\n"
         "      const float4 tc = float4(corners[i].x * 0.5f + 0.5f, 0.5f - corners[i].y * 0.5f, 0.0f, 1.0f);\n";
    for (uint32_t i = 3; i < (std::min)(slots, 19u); i++)
      s += "      o.s" + std::to_string(i) + " = tc;\n";
    s += "    }\n"
         "    stream.Append(o);\n"
         "  }\n"
         "}\n";
    CompileFunction compile = compiler();
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (!compile || FAILED(compile(s.data(), s.size(), "points", nullptr, nullptr, "main", "gs_5_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
      logMessage(LogLevel::Warn, "point sprite shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler");
      if (errors) errors->Release();
      return nullptr;
    }
    m_device->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
    code->Release();
    if (errors) errors->Release();
    return shader;
  }

  void Device::updatePointConstants() {
    const auto& rs = m_renderStates;
    const float w = float((std::max)(1u, m_viewport.width)), h = float((std::max)(1u, m_viewport.height));
    const float c[16] = {
      w, h, 1.0f / w, 1.0f / h,
      asFloat(rs[RsPointSize]), asFloat(rs[RsPointSizeMin]), (std::min)(asFloat(rs[RsPointSizeMax]), 256.0f),
      rs[RsPointSpriteEnable] ? 1.0f : 0.0f,
      asFloat(rs[158]), asFloat(rs[159]), asFloat(rs[160]), rs[RsPointScaleEnable] ? 1.0f : 0.0f,
      0.0f, 0.0f, 0.0f, 0.0f,
    };
    uint32_t words[16];
    std::memcpy(words, c, sizeof(words));
    words[12] = m_vsVariant ? 0u : 1u;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_context->Map(m_pointConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      writeLines(static_cast<uint8_t*>(mapped.pData), words, sizeof(words));
      m_context->Unmap(m_pointConstants.get(), 0);
    }
  }

  void Device::drawPrimitive(PrimitiveType type, uint32_t startVertex, uint32_t vertexCount) {
    DrawArgs a;
    a.type = type;
    a.startVertex = startVertex;
    a.vertexCount = vertexCount;
    drawWith(a);
  }

  void Device::drawIndexedPrimitive(PrimitiveType type, int32_t baseVertex, uint32_t minIndex, uint32_t numVertices,
                                    uint32_t startIndex, uint32_t indexCount) {
    DrawArgs a;
    a.type = type;
    a.indexed = true;
    a.baseVertex = baseVertex;
    a.minIndex = minIndex;
    a.vertexCount = numVertices;
    a.startIndex = startIndex;
    a.indexCount = indexCount;
    drawWith(a);
  }

  void Device::drawPrimitiveUP(PrimitiveType type, uint32_t vertexCount, const void* vertices, uint32_t stride) {
    if (!vertices || !stride)
      return;
    DrawArgs a;
    a.type = type;
    a.vertexCount = vertexCount;
    a.userVertices = vertices;
    a.userStride = stride;
    drawWith(a);
  }

  void Device::drawIndexedPrimitiveUP(PrimitiveType type, uint32_t minIndex, uint32_t numVertices, uint32_t indexCount,
                                      const void* indices, bool index32, const void* vertices, uint32_t stride) {
    if (!vertices || !stride || !indices)
      return;
    DrawArgs a;
    a.type = type;
    a.indexed = true;
    a.minIndex = minIndex;
    a.vertexCount = numVertices;
    a.indexCount = indexCount;
    a.userVertices = vertices;
    a.userStride = stride;
    a.userIndices = indices;
    a.userIndex32 = index32;
    drawWith(a);
  }

  // ---- Flexible vertex format draws (Direct3D 7) ----

  void Device::drawUP(PrimitiveType type, uint32_t fvf, const void* vertices, uint32_t stride, uint32_t vertexCount,
                      const void* indices, bool index32, uint32_t indexCount) {
    if (!vertices || !vertexCount || !stride)
      return;
    setVertexDeclaration(fvfDeclaration(fvf));
    if (indices && indexCount)
      drawIndexedPrimitiveUP(type, 0, vertexCount, indexCount, indices, index32, vertices, stride);
    else
      drawPrimitiveUP(type, vertexCount, vertices, stride);
  }

  void Device::draw(PrimitiveType type, uint32_t fvf, Buffer* vertices, uint32_t stride, uint32_t startVertex,
                    uint32_t vertexCount, Buffer* indices, bool index32, uint32_t startIndex, uint32_t indexCount,
                    int32_t baseVertex) {
    if (!vertices || !stride)
      return;
    setVertexDeclaration(fvfDeclaration(fvf));
    setStreamSource(0, vertices, 0, stride);
    if (indices) {
      setIndices(indices, index32);
      drawIndexedPrimitive(type, baseVertex, startVertex, vertexCount, startIndex, indexCount);
    } else {
      drawPrimitive(type, startVertex, vertexCount);
    }
  }

  void Device::drawIndexedVB(PrimitiveType type, uint32_t fvf, Buffer* vertices, uint32_t stride, int32_t baseVertex,
                             uint32_t vertexCount, const void* indices, bool index32, uint32_t indexCount) {
    if (!vertices || !stride || !indices || !indexCount)
      return;
    setVertexDeclaration(fvfDeclaration(fvf));
    setStreamSource(0, vertices, 0, stride);
    DrawArgs a;
    a.type = type;
    a.indexed = true;
    a.baseVertex = baseVertex;
    a.vertexCount = vertexCount;
    a.indexCount = indexCount;
    a.userIndices = indices;
    a.userIndex32 = index32;
    drawWith(a);
  }

  // ---- Quads: partial clears, blits, presentation ----

  void Device::drawQuad(ID3D11PixelShader* shader, const float rect[4], const float texRect[4], const float color[4],
                        float depth, float keyEnabled) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(m_quadConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
      return;
    auto* c = static_cast<QuadConstants*>(mapped.pData);
    std::memcpy(c->rect, rect, 16);
    if (texRect) std::memcpy(c->texRect, texRect, 16); else std::memset(c->texRect, 0, 16);
    if (color) std::memcpy(c->color, color, 16); else std::memset(c->color, 0, 16);
    c->depth[0] = depth;
    c->depth[1] = keyEnabled;
    c->depth[2] = c->depth[3] = 0.0f;
    m_context->Unmap(m_quadConstants.get(), 0);
    ID3D11Buffer* constants[3] = { nullptr, nullptr, m_quadConstants.get() };
    m_context->VSSetConstantBuffers(0, 3, constants);
    m_context->PSSetConstantBuffers(0, 3, constants);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    if (m_boundGs) {
      m_context->GSSetShader(nullptr, nullptr, 0);
      m_boundGs = nullptr;
    }
    m_context->VSSetShader(m_quadVs.get(), nullptr, 0);
    m_context->PSSetShader(shader, nullptr, 0);
    m_context->RSSetState(m_quadRaster.get());
    m_context->Draw(4, 0);
    // The next draw sets everything again.
    invalidateBindings();
  }

  void Device::clear(uint32_t count, const Rect* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (m_targetsDirty)
      bindTargets();
    const bool srgb = m_renderStates[RsSrgbWriteEnable] != 0;
    ID3D11RenderTargetView* rtvs[kMaxRenderTargets] = {};
    for (uint32_t i = 0; i < kMaxRenderTargets; i++) {
      Texture* target = m_renderTargets[i];
      if (!target || target->nullTarget() || !(flags & ClearTarget))
        continue;
      target->flush();
      rtvs[i] = target->rtv(m_renderTargetSubresources[i], srgb);
    }
    ID3D11DepthStencilView* dsv = (flags & (ClearZBuffer | ClearStencil)) && m_depthStencil ? m_depthStencil->dsv() : nullptr;
    float rgba[4];
    colorToFloats(color, rgba);
    const Rect full = { int32_t(m_viewport.x), int32_t(m_viewport.y), int32_t(m_viewport.x + m_viewport.width),
                        int32_t(m_viewport.y + m_viewport.height) };
    Texture* first = m_renderTargets[0] ? m_renderTargets[0] : m_depthStencil;
    const uint32_t targetWidth = first ? first->width(m_renderTargets[0] ? m_renderTargetSubresources[0] : 0) : full.right;
    const uint32_t targetHeight = first ? first->height(m_renderTargets[0] ? m_renderTargetSubresources[0] : 0) : full.bottom;
    const bool whole = !count && full.left == 0 && full.top == 0 && uint32_t(full.right) >= targetWidth
      && uint32_t(full.bottom) >= targetHeight;
    for (uint32_t i = 0; i < kMaxRenderTargets; i++)
      if (rtvs[i])
        m_renderTargets[i]->markGpuWritten();
    if (whole) {
      for (auto* rtv : rtvs)
        if (rtv)
          m_context->ClearRenderTargetView(rtv, rgba);
      if (dsv) {
        UINT dsFlags = 0;
        if (flags & ClearZBuffer) dsFlags |= D3D11_CLEAR_DEPTH;
        if (flags & ClearStencil) dsFlags |= D3D11_CLEAR_STENCIL;
        m_context->ClearDepthStencilView(dsv, dsFlags, z, UINT8(stencil));
      }
      return;
    }
    // Parts: quads writing what is cleared, inside the viewport.
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = (flags & ClearZBuffer) != 0 && dsv;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = (flags & ClearStencil) != 0 && dsv;
    dd.StencilReadMask = dd.StencilWriteMask = 0xff;
    dd.FrontFace = dd.BackFace = { D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE,
                                   D3D11_COMPARISON_ALWAYS };
    Ref<ID3D11DepthStencilState> depthState;
    m_device->CreateDepthStencilState(&dd, depthState.put());
    D3D11_BLEND_DESC bd = {};
    bd.IndependentBlendEnable = TRUE;
    for (uint32_t i = 0; i < kMaxRenderTargets; i++)
      bd.RenderTarget[i].RenderTargetWriteMask = rtvs[i] ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
    Ref<ID3D11BlendState> blend;
    m_device->CreateBlendState(&bd, blend.put());
    m_context->OMSetDepthStencilState(depthState.get(), stencil & 0xff);
    m_context->OMSetBlendState(blend.get(), nullptr, 0xffffffffu);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, float(targetWidth), float(targetHeight), 0.0f, 1.0f };
    const uint32_t rectCount = count ? count : 1;
    for (uint32_t i = 0; i < rectCount; i++) {
      Rect r = count ? rects[i] : full;
      r.left = (std::max)(r.left, full.left);
      r.top = (std::max)(r.top, full.top);
      r.right = (std::min)(r.right, full.right);
      r.bottom = (std::min)(r.bottom, full.bottom);
      if (r.left >= r.right || r.top >= r.bottom)
        continue;
      // drawQuad unbinds everything: targets again each time.
      ID3D11DepthStencilView* view = dsv ? dsv : (m_depthStencil ? m_depthStencil->dsv() : nullptr);
      ID3D11RenderTargetView* targets[kMaxRenderTargets] = {};
      for (uint32_t t = 0; t < kMaxRenderTargets; t++)
        targets[t] = m_renderTargets[t] && !m_renderTargets[t]->nullTarget()
          ? m_renderTargets[t]->rtv(m_renderTargetSubresources[t], srgb) : nullptr;
      m_context->OMSetRenderTargets(kMaxRenderTargets, targets, view);
      m_context->OMSetDepthStencilState(depthState.get(), stencil & 0xff);
      m_context->OMSetBlendState(blend.get(), nullptr, 0xffffffffu);
      m_context->RSSetViewports(1, &vp);
      D3D11_RECT scissor = { r.left, r.top, r.right, r.bottom };
      m_context->RSSetScissorRects(1, &scissor);
      const float clip[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
      drawQuad(m_clearPs.get(), clip, nullptr, rgba, z, 0.0f);
    }
  }

  void Device::blit(Texture* dst, uint32_t dstSubresource, const Rect* dstRect, Texture* src, uint32_t srcSubresource,
                    const Rect* srcRect, bool linear, bool colorKey, uint32_t keyColor) {
    if (!dst || !src)
      return;
    const Rect dr = dstRect ? *dstRect : Rect{ 0, 0, int32_t(dst->width(dstSubresource)), int32_t(dst->height(dstSubresource)) };
    const Rect sr = srcRect ? *srcRect : Rect{ 0, 0, int32_t(src->width(srcSubresource)), int32_t(src->height(srcSubresource)) };
    if (dr.right <= dr.left || dr.bottom <= dr.top || sr.right <= sr.left || sr.bottom <= sr.top)
      return;

    // Application memory onto a GPU target: uploaded into a texture of the
    // GPU and drawn (on the CPU, the whole target was read back and uploaded
    // again for each blit: video frames into a 4K back buffer).
    if (src->desc().systemMemory && !dst->desc().systemMemory && dst->rtv(dstSubresource)
     && !formatInfo(src->desc().format).block && src->desc().format != FormatP8) {
      const uint32_t w = src->width(srcSubresource), h = src->height(srcSubresource);
      const TextureDesc& sd = src->desc();
      if (!m_blitSource || m_blitSource->desc().width != w || m_blitSource->desc().height != h
       || m_blitSource->desc().format != sd.format) {
        TextureDesc td;
        td.width = w;
        td.height = h;
        td.format = sd.format;
        m_blitSource = std::make_shared<Texture>(this, td);
        if (!m_blitSource->valid() || !m_blitSource->srv())
          m_blitSource.reset();
      }
      void* from = nullptr;
      void* to = nullptr;
      uint32_t fromPitch = 0, toPitch = 0;
      if (m_blitSource && src->lock(srcSubresource, nullptr, true, &from, &fromPitch)) {
        if (m_blitSource->lock(0, nullptr, false, &to, &toPitch, true)) {
          const size_t rowBytes = size_t(w) * formatInfo(sd.format).bytesPerBlock;
          for (uint32_t y = 0; y < h; y++)
            std::memcpy(static_cast<uint8_t*>(to) + size_t(y) * toPitch, static_cast<const uint8_t*>(from) + size_t(y) * fromPitch, rowBytes);
          m_blitSource->unlock(0);
          src->unlock(srcSubresource);
          blit(dst, dstSubresource, &dr, m_blitSource.get(), 0, &sr, linear, colorKey, keyColor);
          return;
        }
        src->unlock(srcSubresource);
      }
    }

    // Application memory on either side: copied on the CPU.
    if (dst->desc().systemMemory || src->desc().systemMemory || !dst->rtv(dstSubresource) || !src->srv()) {
      void* srcData = nullptr;
      void* dstData = nullptr;
      uint32_t srcPitch = 0, dstPitch = 0;
      if (!src->lock(srcSubresource, nullptr, true, &srcData, &srcPitch))
        return;
      if (!dst->lock(dstSubresource, nullptr, false, &dstData, &dstPitch)) {
        src->unlock(srcSubresource);
        return;
      }
      const Format sf = src->desc().format, df = dst->desc().format;
      const FormatInfo si = formatInfo(sf), di = formatInfo(df);
      const int32_t w = dr.right - dr.left, h = dr.bottom - dr.top;
      const int32_t sw = sr.right - sr.left, sh = sr.bottom - sr.top;
      if (si.block || di.block) {
        if (sf == df && w == sw && h == sh) {
          const uint32_t rows = (uint32_t(h) + 3) / 4;
          const uint32_t bytes = ((uint32_t(w) + 3) / 4) * si.bytesPerBlock;
          for (uint32_t y = 0; y < rows; y++)
            std::memcpy(static_cast<uint8_t*>(dstData) + size_t(dr.top / 4 + y) * dstPitch + size_t(dr.left / 4) * di.bytesPerBlock,
              static_cast<const uint8_t*>(srcData) + size_t(sr.top / 4 + y) * srcPitch + size_t(sr.left / 4) * si.bytesPerBlock, bytes);
        }
      } else {
        const int32_t dstWidth = int32_t(dst->width(dstSubresource)), dstHeight = int32_t(dst->height(dstSubresource));
        const int32_t srcWidth = int32_t(src->width(srcSubresource)), srcHeight = int32_t(src->height(srcSubresource));
        for (int32_t y = 0; y < h; y++) {
          const int32_t dy = dr.top + y;
          const int32_t sy = sr.top + (y * sh) / h;
          if (dy < 0 || dy >= dstHeight || sy < 0 || sy >= srcHeight)
            continue;
          const uint8_t* srow = static_cast<const uint8_t*>(srcData) + size_t(sy) * srcPitch;
          uint8_t* drow = static_cast<uint8_t*>(dstData) + size_t(dy) * dstPitch;
          if (sf == df && w == sw && !colorKey) {
            const int32_t x0 = (std::max)(0, -dr.left);
            const int32_t x1 = (std::min)(w, dstWidth - dr.left);
            if (x1 > x0 && sr.left + x1 <= srcWidth)
              std::memcpy(drow + size_t(dr.left + x0) * di.bytesPerBlock, srow + size_t(sr.left + x0) * si.bytesPerBlock,
                size_t(x1 - x0) * di.bytesPerBlock);
            continue;
          }
          for (int32_t x = 0; x < w; x++) {
            const int32_t dx = dr.left + x;
            const int32_t sx = sr.left + (x * sw) / w;
            if (dx < 0 || dx >= dstWidth || sx < 0 || sx >= srcWidth)
              continue;
            const uint8_t* stexel = srow + size_t(sx) * si.bytesPerBlock;
            if (colorKey) {
              uint32_t raw = 0;
              std::memcpy(&raw, stexel, si.bytesPerBlock);
              if (raw == keyColor)
                continue;
            }
            if (sf == df)
              std::memcpy(drow + size_t(dx) * di.bytesPerBlock, stexel, si.bytesPerBlock);
            else
              fromArgb(df, toArgb(sf, stexel, nullptr), drow + size_t(dx) * di.bytesPerBlock);
          }
        }
      }
      dst->unlock(dstSubresource);
      src->unlock(srcSubresource);
      return;
    }

    // On the GPU: a quad sampling the source.
    src->flush();
    dst->flush();
    dst->markGpuWritten();
    ID3D11RenderTargetView* rtv = dst->rtv(dstSubresource);
    ID3D11ShaderResourceView* nulls[24] = {};
    m_context->PSSetShaderResources(0, 24, nulls);
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    const float dw = float(dst->width(dstSubresource)), dh = float(dst->height(dstSubresource));
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, dw, dh, 0.0f, 1.0f };
    m_context->RSSetViewports(1, &vp);
    D3D11_RECT scissor = { (std::max)(0L, LONG(dr.left)), (std::max)(0L, LONG(dr.top)), LONG(dr.right), LONG(dr.bottom) };
    m_context->RSSetScissorRects(1, &scissor);
    m_context->OMSetBlendState(m_opaqueBlend.get(), nullptr, 0xffffffffu);
    m_context->OMSetDepthStencilState(m_noDepth.get(), 0);
    ID3D11ShaderResourceView* srv = src->srv();
    m_context->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* sampler = linear ? m_linearSampler.get() : m_pointSampler.get();
    m_context->PSSetSamplers(0, 1, &sampler);
    const float sw = float(src->width(srcSubresource)), sh = float(src->height(srcSubresource));
    const float clip[4] = { float(dr.left) / dw * 2.0f - 1.0f, 1.0f - float(dr.top) / dh * 2.0f,
                            float(dr.right) / dw * 2.0f - 1.0f, 1.0f - float(dr.bottom) / dh * 2.0f };
    const float tex[4] = { float(sr.left) / sw, float(sr.top) / sh, float(sr.right) / sw, float(sr.bottom) / sh };
    float key[4] = {};
    if (colorKey) {
      // The key in the source's format, as a color.
      uint8_t raw[4] = {};
      std::memcpy(raw, &keyColor, 4);
      colorToFloats(toArgb(src->desc().format, raw, nullptr), key);
    }
    drawQuad(m_blitPs.get(), clip, tex, key, 0.0f, colorKey ? 1.0f : 0.0f);
    ID3D11ShaderResourceView* none = nullptr;
    m_context->PSSetShaderResources(0, 1, &none);
  }

  void Device::fill(Texture* dst, uint32_t dstSubresource, const Rect* dstRect, uint32_t color) {
    if (!dst)
      return;
    const Rect r = dstRect ? *dstRect : Rect{ 0, 0, int32_t(dst->width(dstSubresource)), int32_t(dst->height(dstSubresource)) };
    if (dst->desc().systemMemory || !dst->rtv(dstSubresource)) {
      void* data = nullptr;
      uint32_t pitch = 0;
      if (!dst->lock(dstSubresource, nullptr, false, &data, &pitch))
        return;
      const uint32_t bpp = formatInfo(dst->desc().format).bytesPerBlock;
      const int32_t w = int32_t(dst->width(dstSubresource)), h = int32_t(dst->height(dstSubresource));
      for (int32_t y = (std::max)(0, r.top); y < (std::min)(h, r.bottom); y++)
        for (int32_t x = (std::max)(0, r.left); x < (std::min)(w, r.right); x++)
          std::memcpy(static_cast<uint8_t*>(data) + size_t(y) * pitch + size_t(x) * bpp, &color, bpp);
      dst->unlock(dstSubresource);
      return;
    }
    dst->flush();
    dst->markGpuWritten();
    uint8_t raw[4] = {};
    std::memcpy(raw, &color, 4);
    float rgba[4];
    colorToFloats(toArgb(dst->desc().format, raw, nullptr), rgba);
    ID3D11RenderTargetView* rtv = dst->rtv(dstSubresource);
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    const float dw = float(dst->width(dstSubresource)), dh = float(dst->height(dstSubresource));
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, dw, dh, 0.0f, 1.0f };
    m_context->RSSetViewports(1, &vp);
    D3D11_RECT scissor = { (std::max)(0L, LONG(r.left)), (std::max)(0L, LONG(r.top)), LONG(r.right), LONG(r.bottom) };
    m_context->RSSetScissorRects(1, &scissor);
    m_context->OMSetBlendState(m_opaqueBlend.get(), nullptr, 0xffffffffu);
    m_context->OMSetDepthStencilState(m_noDepth.get(), 0);
    const float clip[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    drawQuad(m_clearPs.get(), clip, nullptr, rgba, 0.0f, 0.0f);
  }

  HRESULT Device::present(Texture* source, const Rect* sourceRect, HWND window, uint32_t syncInterval,
                          const uint16_t* gammaRamp) {
    if (m_trace9) {
      m_trace9 = false;
      logMessage(LogLevel::Warn, "trace9: end of frame, %u draws", m_trace9Draw);
    }
    if (!m_trace9Done && !(m_trace9Presents++ & 63)) {
      wchar_t path[MAX_PATH] = {};
      HMODULE self = nullptr;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&glitchReportsEnabled), &self);
      if (self && GetModuleFileNameW(self, path, MAX_PATH)) {
        if (wchar_t* name = wcsrchr(path, L'\\')) {
          wcscpy_s(name + 1, MAX_PATH - (name + 1 - path), L"dxup_trace9_go.txt");
          if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            m_trace9 = m_trace9Done = true;
            m_trace9DepthDone = false;
            m_trace9Draw = 0;
            // "x y": the probe pixel.
            FILE* f = nullptr;
            if (_wfopen_s(&f, path, L"r") == 0 && f) {
              if (fscanf_s(f, "%d %d", &m_trace9X, &m_trace9Y) != 2)
                m_trace9X = m_trace9Y = -1;
              std::fclose(f);
            }
            logMessage(LogLevel::Warn, "trace9: next frame, probe %d, %d", m_trace9X, m_trace9Y);
          }
        }
      }
    }
    if (glitchReportsEnabled()) {
      logNonFiniteConstants();
      logDroppedDraws();
      logOcclusionResults();
    } else {
      std::memset(m_dropped.counts, 0, sizeof(m_dropped.counts));
    }
    // A ramp other than the identity goes through its lookup table.
    ID3D11ShaderResourceView* gamma = nullptr;
    if (gammaRamp) {
      bool identity = true;
      uint64_t hash = 14695981039346656037ull;
      for (uint32_t i = 0; i < 768; i++) {
        const int expected = int(i % 256) * 257;
        if (std::abs(int(gammaRamp[i]) - expected) > 257)
          identity = false;
        hash = fnv64Multiply(hash ^ gammaRamp[i]);
      }
      if (!identity) {
        if (!m_gammaTexture) {
          D3D11_TEXTURE2D_DESC td = {};
          td.Width = 256;
          td.Height = 1;
          td.MipLevels = td.ArraySize = 1;
          td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
          td.SampleDesc.Count = 1;
          td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
          if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_gammaTexture.put()))
           || FAILED(m_device->CreateShaderResourceView(m_gammaTexture.get(), nullptr, m_gammaView.put())))
            m_gammaTexture.reset();
          m_gammaHash = 0;
        }
        if (m_gammaTexture && hash != m_gammaHash) {
          uint16_t texels[256 * 4];
          for (uint32_t i = 0; i < 256; i++) {
            texels[i * 4] = gammaRamp[i];
            texels[i * 4 + 1] = gammaRamp[256 + i];
            texels[i * 4 + 2] = gammaRamp[512 + i];
            texels[i * 4 + 3] = 0xffff;
          }
          m_context->UpdateSubresource(m_gammaTexture.get(), 0, nullptr, texels, sizeof(texels), 0);
          m_gammaHash = hash;
        }
        gamma = m_gammaView.get();
      }
    }

    if (!source || !source->srv() || !window)
      return E_INVALIDARG;
    RECT client = {};
    GetClientRect(window, &client);
    const UINT width = (std::max)(1L, client.right - client.left);
    const UINT height = (std::max)(1L, client.bottom - client.top);

    SwapChain* chain = nullptr;
    for (auto& existing : m_swapChains)
      if (existing->window == window) chain = existing.get();
    if (!chain) {
      auto created = std::make_unique<SwapChain>();
      created->window = window;
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      desc.Width = width;
      desc.Height = height;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.BufferCount = 2;
      desc.Scaling = DXGI_SCALING_STRETCH;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      // Presents without VSync (Direct3D 9's IMMEDIATE, DirectDraw's NOVSYNC)
      // tear rather than wait for the display: flip model swap chains
      // otherwise run at its refresh rate.
      if (m_tearing < 0) {
        Ref<IDXGIFactory5> factory5;
        BOOL allow = FALSE;
        m_tearing = SUCCEEDED(m_factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(factory5.put())))
          && SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) && allow;
      }
      if (m_tearing)
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
      created->flags = desc.Flags;
      HRESULT hr = m_factory->CreateSwapChainForHwnd(m_device.get(), window, &desc, nullptr, nullptr, created->swapChain.put());
      if (FAILED(hr))
        return hr;
      m_factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
      created->width = width;
      created->height = height;
      chain = created.get();
      m_swapChains.push_back(std::move(created));
    } else if (chain->width != width || chain->height != height) {
      m_context->OMSetRenderTargets(0, nullptr, nullptr);
      chain->swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, chain->flags);
      chain->width = width;
      chain->height = height;
    }

    Ref<ID3D11Texture2D> backBuffer;
    if (FAILED(chain->swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.put()))))
      return E_FAIL;
    Ref<ID3D11RenderTargetView> rtv;
    if (FAILED(m_device->CreateRenderTargetView(backBuffer.get(), nullptr, rtv.put())))
      return E_FAIL;
    source->flush();
    ID3D11RenderTargetView* target = rtv.get();
    ID3D11ShaderResourceView* nulls[24] = {};
    m_context->PSSetShaderResources(0, 24, nulls);
    m_context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
    m_context->RSSetViewports(1, &vp);
    D3D11_RECT scissor = { 0, 0, LONG(width), LONG(height) };
    m_context->RSSetScissorRects(1, &scissor);
    m_context->OMSetBlendState(m_opaqueBlend.get(), nullptr, 0xffffffffu);
    m_context->OMSetDepthStencilState(m_noDepth.get(), 0);
    ID3D11ShaderResourceView* srv = source->srv();
    m_context->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* sampler = m_linearSampler.get();
    m_context->PSSetSamplers(0, 1, &sampler);
    const Rect sr = sourceRect ? *sourceRect : Rect{ 0, 0, int32_t(source->width(0)), int32_t(source->height(0)) };
    const float sw = float(source->width(0)), sh = float(source->height(0));
    const float clip[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    const float tex[4] = { float(sr.left) / sw, float(sr.top) / sh, float(sr.right) / sw, float(sr.bottom) / sh };
    if (gamma)
      m_context->PSSetShaderResources(1, 1, &gamma);
    drawQuad(gamma ? m_gammaPs.get() : m_blitPs.get(), clip, tex, nullptr, 0.0f, 0.0f);
    ID3D11ShaderResourceView* none[2] = {};
    m_context->PSSetShaderResources(0, 2, none);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    const UINT presentFlags = !syncInterval && (chain->flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    return chain->swapChain->Present(syncInterval, presentFlags);
  }

  void Device::fillArgb(Texture* dst, uint32_t dstSubresource, const Rect* dstRect, uint32_t argb) {
    if (!dst)
      return;
    uint8_t raw[16] = {};
    fromArgb(dst->desc().format, argb, raw);
    uint32_t color = 0;
    std::memcpy(&color, raw, 4);
    if (dst->desc().systemMemory || !dst->rtv(dstSubresource)) {
      fill(dst, dstSubresource, dstRect, color);
      return;
    }
    const Rect r = dstRect ? *dstRect : Rect{ 0, 0, int32_t(dst->width(dstSubresource)), int32_t(dst->height(dstSubresource)) };
    dst->flush();
    dst->markGpuWritten();
    float rgba[4];
    colorToFloats(argb, rgba);
    ID3D11RenderTargetView* rtv = dst->rtv(dstSubresource);
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    const float dw = float(dst->width(dstSubresource)), dh = float(dst->height(dstSubresource));
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, dw, dh, 0.0f, 1.0f };
    m_context->RSSetViewports(1, &vp);
    D3D11_RECT scissor = { (std::max)(0L, LONG(r.left)), (std::max)(0L, LONG(r.top)), LONG(r.right), LONG(r.bottom) };
    m_context->RSSetScissorRects(1, &scissor);
    m_context->OMSetBlendState(m_opaqueBlend.get(), nullptr, 0xffffffffu);
    m_context->OMSetDepthStencilState(m_noDepth.get(), 0);
    const float clip[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    drawQuad(m_clearPs.get(), clip, nullptr, rgba, 0.0f, 0.0f);
  }

}
