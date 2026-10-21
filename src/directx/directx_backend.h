#pragma once

#include <cstdint>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <iterator>
#include <set>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "../util/util_types.h"

// Forward declarations for Windows/D3D types when needed
#ifdef _WIN32
#include <dxgi.h>
#else
// Minimal type definitions for non-Windows builds
typedef unsigned int UINT;
typedef long HRESULT;
#define DXGI_FORMAT_UNKNOWN 0
typedef int DXGI_FORMAT;
#endif

namespace dxup {

  // The typeless formats (a family's name; views give the type).
  inline bool isTypelessFormat(DXGI_FORMAT format) {
    switch (format) {
      case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32_TYPELESS:
      case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R32G32_TYPELESS:
      case DXGI_FORMAT_R32G8X24_TYPELESS:     case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      case DXGI_FORMAT_R8G8B8A8_TYPELESS:     case DXGI_FORMAT_R16G16_TYPELESS:
      case DXGI_FORMAT_R32_TYPELESS:          case DXGI_FORMAT_R24G8_TYPELESS:
      case DXGI_FORMAT_R8G8_TYPELESS:         case DXGI_FORMAT_R16_TYPELESS:
      case DXGI_FORMAT_R8_TYPELESS:           case DXGI_FORMAT_BC1_TYPELESS:
      case DXGI_FORMAT_BC2_TYPELESS:          case DXGI_FORMAT_BC3_TYPELESS:
      case DXGI_FORMAT_BC4_TYPELESS:          case DXGI_FORMAT_BC5_TYPELESS:
      case DXGI_FORMAT_B8G8R8A8_TYPELESS:     case DXGI_FORMAT_B8G8R8X8_TYPELESS:
      case DXGI_FORMAT_BC6H_TYPELESS:         case DXGI_FORMAT_BC7_TYPELESS:
        return true;
      default:
        return false;
    }
  }

  // Reference-counted pointer (simplified)
  template<typename T>
  using Rc = std::shared_ptr<T>;

  // Defers the destruction of backend objects while commands that reference
  // them may still run: commands queued for a worker thread, and command
  // lists recorded by deferred contexts. Objects handed out by wrap() retire
  // here when their last reference is dropped.
  class BackendRetireQueue {
  public:
    void retire(std::shared_ptr<void> object) {
      std::unique_lock<std::mutex> lock(m_mutex);
      if (m_deferred || !m_holds.empty()) {
        m_objects.push_back(std::move(object));
        m_sequence++;
        return;
      }
      lock.unlock();
      object.reset();
    }

    // A worker thread takes the retired objects with each command chunk and
    // destroys them after executing it.
    void setDeferred(bool deferred) {
      std::vector<std::shared_ptr<void>> released;
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deferred = deferred;
        if (!deferred)
          takeReleasable(released);
      }
    }

    // Moves the objects no hold protects into out. Returns whether any
    // were taken.
    bool takeRetired(std::vector<std::shared_ptr<void>>& out) {
      std::lock_guard<std::mutex> lock(m_mutex);
      return takeReleasable(out);
    }

    // Keeps every object retired from now on until the hold is released.
    // Deferred contexts hold while they record a command list, which then
    // holds until it is destroyed.
    uint64_t acquireHold() {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_holds.insert(m_sequence);
      return m_sequence;
    }

    void releaseHold(uint64_t hold) {
      std::vector<std::shared_ptr<void>> released;
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_holds.find(hold);
        if (it != m_holds.end())
          m_holds.erase(it);
        if (!m_deferred)
          takeReleasable(released);
      }
    }

    template<typename Interface, typename Object>
    static Rc<Interface> wrap(const std::shared_ptr<BackendRetireQueue>& queue, std::shared_ptr<Object> object) {
      if (!queue || !object)
        return object;
      Interface* raw = object.get();
      return Rc<Interface>(raw,
        [queue, owner = std::shared_ptr<void>(std::move(object))](Interface*) mutable {
          queue->retire(std::move(owner));
        });
    }

  private:
    bool takeReleasable(std::vector<std::shared_ptr<void>>& out) {
      const uint64_t limit = m_holds.empty() ? UINT64_MAX : *m_holds.begin();
      // Objects are numbered in retirement order: m_objects[i] is number
      // first + i, and only those below the oldest hold may go.
      const uint64_t first = m_sequence - m_objects.size();
      const uint64_t allowed = limit > first ? limit - first : 0;
      const size_t count = allowed < m_objects.size() ? size_t(allowed) : m_objects.size();
      if (!count)
        return false;
      // Usually all of them, without holds: handed over whole.
      if (count == m_objects.size() && out.empty()) {
        out.swap(m_objects);
        return true;
      }
      out.insert(out.end(), std::make_move_iterator(m_objects.begin()),
        std::make_move_iterator(m_objects.begin() + count));
      m_objects.erase(m_objects.begin(), m_objects.begin() + count);
      return true;
    }

    std::mutex                m_mutex;
    // A vector: a deque of these allocates a block per element on MSVC.
    std::vector<std::shared_ptr<void>> m_objects;
    std::multiset<uint64_t>   m_holds;
    uint64_t                  m_sequence = 0;
    bool                      m_deferred = false;
  };

  // ============================================================
  // Enumerations
  // ============================================================

  enum class BackendType {
    D3D12
  };

  enum class ShaderStage : uint32_t {
    Vertex   = 0,
    Hull     = 1,
    Domain   = 2,
    Geometry = 3,
    Pixel    = 4,
    Compute  = 5,
    Count    = 6
  };

  enum class IndexType {
    UInt16,
    UInt32
  };

  enum class MapMode {
    Read,
    Write,
    ReadWrite,
    WriteDiscard,
    WriteNoOverwrite
  };

  enum class BackendFeature {
    GeometryShader,
    TessellationShader,
    ComputeShader,
    StreamOutput,
    MultiViewport,
    IndirectDraw,
    OcclusionQuery,
    TimestampQuery,
    PipelineStatisticsQuery
  };

  enum class ResourceUsage : uint32_t {
    Default   = 0,
    Immutable = 1,
    Dynamic   = 2,
    Staging   = 3
  };

  enum class CpuAccess : uint32_t {
    None  = 0,
    Read  = 1,
    Write = 2
  };

  enum class BindFlags : uint32_t {
    None              = 0,
    VertexBuffer      = (1 << 0),
    IndexBuffer       = (1 << 1),
    ConstantBuffer    = (1 << 2),
    ShaderResource    = (1 << 3),
    StreamOutput      = (1 << 4),
    RenderTarget      = (1 << 5),
    DepthStencil      = (1 << 6),
    UnorderedAccess   = (1 << 7)
  };

  inline BindFlags operator|(BindFlags a, BindFlags b) {
    return static_cast<BindFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
  }
  inline BindFlags operator&(BindFlags a, BindFlags b) {
    return static_cast<BindFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
  }

  enum class ImageType {
    Tex1D,
    Tex2D,
    Tex3D
  };

  enum class ImageViewType {
    View1D,
    View1DArray,
    View2D,
    View2DArray,
    View2DMS,
    View2DMSArray,
    View3D,
    ViewCube,
    ViewCubeArray
  };

  enum class ImageViewUsage : uint32_t {
    None            = 0,
    ShaderResource  = (1 << 0),
    RenderTarget    = (1 << 1),
    DepthStencil    = (1 << 2),
    UnorderedAccess = (1 << 3)
  };

  inline ImageViewUsage operator|(ImageViewUsage a, ImageViewUsage b) {
    return static_cast<ImageViewUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
  }
  inline ImageViewUsage operator&(ImageViewUsage a, ImageViewUsage b) {
    return static_cast<ImageViewUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
  }

  enum class ImageViewFlags : uint32_t {
    None            = 0,
    ReadOnlyDepth   = (1 << 0),
    ReadOnlyStencil = (1 << 1)
  };

  inline ImageViewFlags operator|(ImageViewFlags a, ImageViewFlags b) {
    return static_cast<ImageViewFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
  }

  inline ImageViewFlags operator&(ImageViewFlags a, ImageViewFlags b) {
    return static_cast<ImageViewFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
  }

  enum class QueryType {
    Occlusion,
    Timestamp,
    TimestampDisjoint,
    PipelineStatistics,
    StreamOutputStatistics,
    Event
  };

  struct QueryDesc {
    QueryType type = QueryType::Occlusion;
    // Occlusion: only whether any sample passed (predicates).
    bool      binary = false;
    // Stream output: stream 0-3, or kAllStreams for the sum of all four.
    uint32_t  stream = 0;

    static constexpr uint32_t kAllStreams = 4;
  };

  // Results of a finished query; the fields that apply to its type are set.
  struct QueryData {
    uint64_t samples = 0;
    uint64_t timestamp = 0;
    uint64_t frequency = 0;
    bool     disjoint = false;
    uint64_t pipelineStatistics[11] = {};
    uint64_t primitivesWritten = 0;
    uint64_t primitivesStorageNeeded = 0;
  };

  enum class QueryStatus {
    Ready,
    // Commands up to End still have to be submitted (the context must flush).
    NotSubmitted,
    // Submitted; the GPU has not finished them yet.
    Pending,
    // Begun but not ended, or never issued.
    Invalid
  };

  enum class ComparisonFunc {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
  };

  enum class StencilOp {
    Keep,
    Zero,
    Replace,
    IncrSat,
    DecrSat,
    Invert,
    Incr,
    Decr
  };

  enum class BlendFactor {
    Zero,
    One,
    SrcColor,
    InvSrcColor,
    SrcAlpha,
    InvSrcAlpha,
    DstAlpha,
    InvDstAlpha,
    DstColor,
    InvDstColor,
    SrcAlphaSat,
    BlendFactor,
    InvBlendFactor,
    Src1Color,
    InvSrc1Color,
    Src1Alpha,
    InvSrc1Alpha
  };

  enum class BlendOp {
    Add,
    Subtract,
    RevSubtract,
    Min,
    Max
  };

  enum class FillMode {
    Wireframe,
    Solid
  };

  enum class CullMode {
    None,
    Front,
    Back
  };

  enum class FilterMode {
    MinMagMipPoint,
    MinMagPointMipLinear,
    MinPointMagLinearMipPoint,
    MinPointMagMipLinear,
    MinLinearMagMipPoint,
    MinLinearMagPointMipLinear,
    MinMagLinearMipPoint,
    MinMagMipLinear,
    Anisotropic,
    ComparisonMinMagMipPoint,
    ComparisonMinMagMipLinear,
    ComparisonAnisotropic
  };

  enum class TextureAddressMode {
    Wrap,
    Mirror,
    Clamp,
    Border,
    MirrorOnce
  };

  enum class PrimitiveTopology {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
    LineListAdj,
    LineStripAdj,
    TriangleListAdj,
    TriangleStripAdj,
    PatchList1,
    PatchList2,
    PatchList3,
    PatchList4,
    PatchList5,
    PatchList6,
    PatchList7,
    PatchList8,
    PatchList9,
    PatchList10,
    PatchList11,
    PatchList12,
    PatchList13,
    PatchList14,
    PatchList15,
    PatchList16,
    PatchList17,
    PatchList18,
    PatchList19,
    PatchList20,
    PatchList21,
    PatchList22,
    PatchList23,
    PatchList24,
    PatchList25,
    PatchList26,
    PatchList27,
    PatchList28,
    PatchList29,
    PatchList30,
    PatchList31,
    PatchList32,
    Undefined
  };

  enum class InputClassification {
    PerVertex,
    PerInstance
  };

  enum class ClearFlags : uint32_t {
    Depth   = 1,
    Stencil = 2
  };

  inline ClearFlags operator|(ClearFlags a, ClearFlags b) {
    return static_cast<ClearFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
  }

  // ============================================================
  // Descriptor Structures
  // ============================================================

  // D3D11 misc flags the backend acts on (BufferDesc/ImageDesc::miscFlags).
  constexpr uint32_t kResourceMiscTilePool = 0x20000;  // D3D11_RESOURCE_MISC_TILE_POOL
  constexpr uint32_t kResourceMiscTiled    = 0x40000;  // D3D11_RESOURCE_MISC_TILED
  // D3D11_RESOURCE_MISC_SHARED, _SHARED_KEYEDMUTEX, _SHARED_NTHANDLE
  constexpr uint32_t kResourceMiscShared   = 0x2 | 0x100 | 0x800;
  // DXUP's own: views in the other format of the family too, as swap chain
  // buffers take them (an sRGB view of a UNORM back buffer).
  constexpr uint32_t kResourceMiscCastable = 0x80000000u;
  constexpr uint64_t kTileSizeInBytes      = 65536;

  // Tiled resource structures, laid out like their D3D11/D3D12 counterparts.
  struct TileCoordinate {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
    uint32_t subresource = 0;
  };

  struct TileRegionSize {
    uint32_t numTiles = 0;
    int32_t  useBox = 0;
    uint32_t width = 0;
    uint16_t height = 0;
    uint16_t depth = 0;
  };

  class IBackendBuffer;
  class IBackendImage;

  // A tiled buffer or texture.
  struct TiledResourceRef {
    IBackendBuffer* buffer = nullptr;
    IBackendImage*  image = nullptr;
  };

  // UpdateTileMappings arguments with the arrays owned.
  struct TileMappingUpdate {
    TiledResourceRef            resource;
    IBackendBuffer*             tilePool = nullptr;
    std::vector<TileCoordinate> coordinates;
    std::vector<TileRegionSize> sizes;
    std::vector<uint32_t>       rangeFlags;
    std::vector<uint32_t>       poolOffsets;
    std::vector<uint32_t>       rangeTileCounts;
    uint32_t                    flags = 0;
  };

  struct BufferDesc {
    uint64_t        size        = 0;
    // The bytes the application addresses, when fewer than size: constant
    // buffers take whole 256-byte units and read 0 past their data.
    uint32_t        dataSize    = 0;
    uint32_t        structStride = 0;
    ResourceUsage   usage       = ResourceUsage::Default;
    BindFlags       bindFlags   = BindFlags::None;
    CpuAccess       cpuAccess   = CpuAccess::None;
    uint32_t        miscFlags   = 0;
  };

  struct ImageDesc {
    ImageType       type        = ImageType::Tex2D;
    uint32_t        width       = 1;
    uint32_t        height      = 1;
    uint32_t        depth       = 1;
    uint32_t        mipLevels   = 1;
    uint32_t        arraySize   = 1;
    DXGI_FORMAT     format      = DXGI_FORMAT_UNKNOWN;
    uint32_t        sampleCount = 1;
    uint32_t        sampleQuality = 0;
    ResourceUsage   usage       = ResourceUsage::Default;
    BindFlags       bindFlags   = BindFlags::None;
    CpuAccess       cpuAccess   = CpuAccess::None;
    uint32_t        miscFlags   = 0;
  };

  struct BufferViewDesc {
    DXGI_FORMAT     format      = DXGI_FORMAT_UNKNOWN;
    uint64_t        offset      = 0;
    uint64_t        size        = 0;
    bool            raw         = false;
    bool            hasCounter  = false;
  };

  struct ImageViewDesc {
    ImageViewType   type        = ImageViewType::View2D;
    DXGI_FORMAT     format      = DXGI_FORMAT_UNKNOWN;
    ImageViewUsage  usage       = ImageViewUsage::ShaderResource;
    ImageViewFlags  flags       = ImageViewFlags::None;
    uint32_t        baseMipLevel   = 0;
    uint32_t        mipLevelCount  = 1;
    uint32_t        baseArrayLayer = 0;
    uint32_t        arrayLayerCount = 1;
    // Shader resource views: D3D12 Shader4ComponentMapping, 0 the default
    // (DXUP's Direct3D 9 layer reads ATI2 as green, red, 1, 1).
    uint32_t        componentMapping = 0;
  };

  struct SamplerDesc {
    FilterMode      filter      = FilterMode::MinMagMipLinear;
    TextureAddressMode addressU = TextureAddressMode::Wrap;
    TextureAddressMode addressV = TextureAddressMode::Wrap;
    TextureAddressMode addressW = TextureAddressMode::Wrap;
    float           mipLodBias  = 0.0f;
    uint32_t        maxAnisotropy = 1;
    ComparisonFunc  comparisonFunc = ComparisonFunc::Never;
    float           borderColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float           minLod      = 0.0f;
    float           maxLod      = 3.402823466e+38f;
  };

  struct ShaderDesc {
    ShaderStage     stage;
    const void*     bytecode    = nullptr;
    size_t          bytecodeSize = 0;
  };

  struct InputElementDesc {
    const char*         semanticName;
    uint32_t            semanticIndex;
    DXGI_FORMAT         format;
    uint32_t            inputSlot;
    uint32_t            byteOffset;
    InputClassification inputClass;
    uint32_t            instanceDataStepRate;
  };

  struct InputLayoutDesc {
    const InputElementDesc* elements;
    uint32_t                numElements;
  };

  struct StencilOpDesc {
    StencilOp       stencilFailOp      = StencilOp::Keep;
    StencilOp       stencilDepthFailOp = StencilOp::Keep;
    StencilOp       stencilPassOp      = StencilOp::Keep;
    ComparisonFunc  stencilFunc        = ComparisonFunc::Always;
  };

  struct DepthStencilState {
    bool            depthEnable        = true;
    bool            depthWriteEnable   = true;
    ComparisonFunc  depthFunc          = ComparisonFunc::Less;
    bool            stencilEnable      = false;
    uint8_t         stencilReadMask    = 0xFF;
    uint8_t         stencilWriteMask   = 0xFF;
    StencilOpDesc   frontFace;
    StencilOpDesc   backFace;
  };

  struct RenderTargetBlendDesc {
    bool            blendEnable        = false;
    BlendFactor     srcBlend           = BlendFactor::One;
    BlendFactor     dstBlend           = BlendFactor::Zero;
    BlendOp         blendOp            = BlendOp::Add;
    BlendFactor     srcBlendAlpha      = BlendFactor::One;
    BlendFactor     dstBlendAlpha      = BlendFactor::Zero;
    BlendOp         blendOpAlpha       = BlendOp::Add;
    uint8_t         writeMask          = 0x0F;
    // Output merger logic op; the values are D3D11_LOGIC_OP / D3D12_LOGIC_OP.
    bool            logicOpEnable      = false;
    uint8_t         logicOp            = 0;
  };

  struct BlendState {
    bool                  alphaToCoverageEnable  = false;
    bool                  independentBlendEnable = false;
    RenderTargetBlendDesc renderTargets[8];
  };

  struct RasterizerState {
    FillMode        fillMode           = FillMode::Solid;
    CullMode        cullMode           = CullMode::Back;
    bool            frontCounterClockwise = false;
    int32_t         depthBias          = 0;
    float           depthBiasClamp     = 0.0f;
    float           slopeScaledDepthBias = 0.0f;
    bool            depthClipEnable    = true;
    bool            scissorEnable      = false;
    bool            multisampleEnable  = false;
    bool            antialiasedLineEnable = false;
    // UAV-only rendering at this sample count (0: the targets' count).
    uint32_t        forcedSampleCount  = 0;
    bool            conservativeRaster = false;
  };

  struct Viewport {
    float x, y, width, height;
    float minDepth, maxDepth;
  };

  struct Rect {
    int32_t left, top, right, bottom;
  };

  struct ImageSubresource {
    uint32_t mipLevel       = 0;
    uint32_t arrayLayer     = 0;
    uint32_t arrayLayerCount = 1;
  };

  struct ImageCopyRegion {
    uint32_t x      = 0;
    uint32_t y      = 0;
    uint32_t z      = 0;
    uint32_t width  = 1;
    uint32_t height = 1;
    uint32_t depth  = 1;
  };

  // Content hashes of the fixed-function states used in the PSO cache key.
  // State objects are immutable, so the D3D11 layer computes these once at
  // creation and passes them in GraphicsPipelineDesc instead of rehashing on
  // every pipeline lookup. The values also name the on-disk PSO cache.
  namespace state_hash {
    constexpr uint64_t kSeed = 1469598103934665603ull;

    inline void append(uint64_t& hash, const void* data, size_t size) {
      const auto* bytes = static_cast<const uint8_t*>(data);
      for (size_t i = 0; i < size; i++) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash = fnv64Multiply(hash);
      }
    }

    inline void appendBool(uint64_t& hash, bool value) {
      const uint8_t normalized = value ? 1u : 0u;
      append(hash, &normalized, sizeof(normalized));
    }

    inline void appendUint32(uint64_t& hash, uint32_t value) {
      append(hash, &value, sizeof(value));
    }

    inline uint64_t blend(const BlendState& state) {
      uint64_t hash = kSeed;
      appendBool(hash, state.alphaToCoverageEnable);
      appendBool(hash, state.independentBlendEnable);
      for (const auto& target : state.renderTargets) {
        appendBool(hash, target.blendEnable);
        appendUint32(hash, static_cast<uint32_t>(target.srcBlend));
        appendUint32(hash, static_cast<uint32_t>(target.dstBlend));
        appendUint32(hash, static_cast<uint32_t>(target.blendOp));
        appendUint32(hash, static_cast<uint32_t>(target.srcBlendAlpha));
        appendUint32(hash, static_cast<uint32_t>(target.dstBlendAlpha));
        appendUint32(hash, static_cast<uint32_t>(target.blendOpAlpha));
        append(hash, &target.writeMask, sizeof(target.writeMask));
        appendBool(hash, target.logicOpEnable);
        append(hash, &target.logicOp, sizeof(target.logicOp));
      }
      return hash;
    }

    inline uint64_t rasterizer(const RasterizerState& state) {
      uint64_t hash = kSeed;
      appendUint32(hash, static_cast<uint32_t>(state.fillMode));
      appendUint32(hash, static_cast<uint32_t>(state.cullMode));
      appendBool(hash, state.frontCounterClockwise);
      append(hash, &state.depthBias, sizeof(state.depthBias));
      append(hash, &state.depthBiasClamp, sizeof(state.depthBiasClamp));
      append(hash, &state.slopeScaledDepthBias, sizeof(state.slopeScaledDepthBias));
      appendBool(hash, state.depthClipEnable);
      appendBool(hash, state.multisampleEnable);
      appendBool(hash, state.antialiasedLineEnable);
      appendUint32(hash, state.forcedSampleCount);
      appendBool(hash, state.conservativeRaster);
      return hash;
    }

    inline void appendStencil(uint64_t& hash, const StencilOpDesc& state) {
      appendUint32(hash, static_cast<uint32_t>(state.stencilFailOp));
      appendUint32(hash, static_cast<uint32_t>(state.stencilDepthFailOp));
      appendUint32(hash, static_cast<uint32_t>(state.stencilPassOp));
      appendUint32(hash, static_cast<uint32_t>(state.stencilFunc));
    }

    inline uint64_t depthStencil(const DepthStencilState& state) {
      uint64_t hash = kSeed;
      appendBool(hash, state.depthEnable);
      appendBool(hash, state.depthWriteEnable);
      appendUint32(hash, static_cast<uint32_t>(state.depthFunc));
      appendBool(hash, state.stencilEnable);
      append(hash, &state.stencilReadMask, sizeof(state.stencilReadMask));
      append(hash, &state.stencilWriteMask, sizeof(state.stencilWriteMask));
      appendStencil(hash, state.frontFace);
      appendStencil(hash, state.backFace);
      return hash;
    }
  }

  struct GraphicsPipelineDesc {
    const void*       vsBytecode      = nullptr;
    size_t            vsBytecodeSize  = 0;
    uint64_t          vsBytecodeHash  = 0;
    const void*       psBytecode      = nullptr;
    size_t            psBytecodeSize  = 0;
    uint64_t          psBytecodeHash  = 0;
    const void*       gsBytecode      = nullptr;
    size_t            gsBytecodeSize  = 0;
    uint64_t          gsBytecodeHash  = 0;
    const void*       hsBytecode      = nullptr;
    size_t            hsBytecodeSize  = 0;
    uint64_t          hsBytecodeHash  = 0;
    const void*       dsBytecode      = nullptr;
    size_t            dsBytecodeSize  = 0;
    uint64_t          dsBytecodeHash  = 0;

    InputLayoutDesc   inputLayout     = {};
    uint64_t          inputLayoutHash = 0;
    BlendState        blendState;
    RasterizerState   rasterizerState;
    DepthStencilState depthStencilState;
    // Optional precomputed state_hash values of the three states; 0 means
    // the backend hashes the state itself.
    uint64_t          blendStateHash        = 0;
    uint64_t          rasterizerStateHash   = 0;
    uint64_t          depthStencilStateHash = 0;
    PrimitiveTopology topology        = PrimitiveTopology::TriangleList;
    IndexType         stripCutIndexType = IndexType::UInt16;

    uint32_t          numRenderTargets = 0;
    DXGI_FORMAT       rtvFormats[8]    = {};
    DXGI_FORMAT       dsvFormat        = DXGI_FORMAT_UNKNOWN;
    uint32_t          sampleCount      = 1;
    // The targets' quality (D3D11 and D3D12 share the standard and center
    // pattern values).
    uint32_t          sampleQuality    = 0;
    uint32_t          sampleMask       = 0xFFFFFFFF;
    // The pixel shader runs once per sample (supersampling).
    bool              sampleShading    = false;

    // Stream output declaration (for GS with SO)
    struct SOEntry {
      uint32_t    stream;
      const char* semanticName;
      uint32_t    semanticIndex;
      uint8_t     startComponent;
      uint8_t     componentCount;
      uint8_t     outputSlot;
    };
    const SOEntry*    soEntries         = nullptr;
    uint32_t          numSOEntries      = 0;
    const uint32_t*   soBufferStrides   = nullptr;
    uint32_t          numSOStrides      = 0;
    uint32_t          rasterizedStream  = 0xFFFFFFFF; // 0xFFFFFFFF = no rasterization
  };

  struct ComputePipelineDesc {
    const void*       csBytecode      = nullptr;
    size_t            csBytecodeSize  = 0;
    uint64_t          csBytecodeHash  = 0;
  };

  struct BackendLimits {
    uint32_t maxTexture1DSize;
    uint32_t maxTexture2DSize;
    uint32_t maxTexture3DSize;
    uint32_t maxTextureCubeSize;
    uint32_t maxTextureArraySize;
    uint32_t maxConstantBufferSize;
    uint32_t maxVertexBuffers;
    uint32_t maxRenderTargets;
    uint32_t maxViewports;
    uint32_t maxComputeThreadGroupSize[3];
    uint32_t maxComputeThreadGroupCount[3];
  };

  // ============================================================
  // Abstract Resource Interfaces
  // ============================================================

  // The GPU work that uses a resource the CPU can map (staging buffers and
  // textures), so that Map waits for exactly that work.
  struct BackendGpuUse {
    // Commands that use the resource and wait on a CS thread to be recorded.
    std::atomic<uint32_t> queuedOps{0};
    // Fence value of the immediate context submission that last used it.
    std::atomic<uint64_t> fenceValue{0};
    // The same for uses that may write it (anything but a copy from it).
    std::atomic<uint64_t> writeFenceValue{0};
  };

  class IBackendBuffer {
  public:
    virtual ~IBackendBuffer() = default;
    virtual const BufferDesc& getDesc() const = 0;

    BackendGpuUse gpuUse;
    // Contents a command list recorded with WRITE_DISCARD: later discards
    // give the buffer other memory instead of replacing this in place.
    bool pinnedContents = false;
  };

  // Where a subresource of a CPU-accessible image lies in its memory.
  struct ImageSubresourceLayout {
    uint64_t offset     = 0;
    uint32_t rowPitch   = 0;
    uint32_t depthPitch = 0;
  };

  class IBackendImage {
  public:
    virtual ~IBackendImage() = default;
    virtual const ImageDesc& getDesc() const = 0;

    // Staging images live in CPU-visible memory. mapStaging returns a D3D11
    // subresource in the layout D3D11 maps it with (the caller waited for
    // the GPU); unmapStaging stores what the CPU wrote.
    virtual bool isCpuAccessible() const { return false; }
    virtual uint8_t* mapStaging(uint32_t subresource, MapMode mode, ImageSubresourceLayout& layout) {
      (void)subresource;
      (void)mode;
      (void)layout;
      return nullptr;
    }
    virtual void unmapStaging(uint32_t subresource) { (void)subresource; }

    // D3D11 resource min LOD as last set by the application. Views created
    // afterwards start clamped to it.
    std::atomic<float> minLod{0.0f};

    BackendGpuUse gpuUse;
  };

  // The use tracking of a resource Map may wait for, or null.
  inline BackendGpuUse* stagingGpuUse(IBackendBuffer* buffer) {
    return buffer && buffer->getDesc().usage == ResourceUsage::Staging ? &buffer->gpuUse : nullptr;
  }

  inline BackendGpuUse* stagingGpuUse(IBackendImage* image) {
    return image && image->getDesc().usage == ResourceUsage::Staging ? &image->gpuUse : nullptr;
  }

  class IBackendBufferView {
  public:
    virtual ~IBackendBufferView() = default;
    virtual const BufferViewDesc& getDesc() const = 0;
    virtual IBackendBuffer* getBuffer() const = 0;
    virtual IBackendBuffer* getCounterBuffer() const { return nullptr; }
    virtual bool retargetBuffer(IBackendBuffer* buffer) { (void)buffer; return false; }
    virtual uint64_t getDescriptorVersion() const { return 0; }
  };

  class IBackendImageView {
  public:
    virtual ~IBackendImageView() = default;
    virtual const ImageViewDesc& getDesc() const = 0;
    virtual IBackendImage* getImage() const = 0;
    // Changes whenever the view's descriptors are rewritten.
    virtual uint64_t getDescriptorVersion() const { return 0; }
  };

  class IBackendSampler {
  public:
    virtual ~IBackendSampler() = default;
    virtual const SamplerDesc& getDesc() const = 0;
  };

  // What a pixel shader's code does, as far as running it per sample is
  // concerned (IBackendShader::getPixelShaderTraits).
  enum PixelShaderTraits : uint32_t {
    // It interpolates inputs other than the pixel position, so samples of
    // a pixel can get different results.
    PixelShaderInterpolates  = 1u << 0,
    // It discards pixels (alpha testing).
    PixelShaderDiscards      = 1u << 1,
    // It writes unordered access views, which per-sample runs would repeat.
    PixelShaderWritesUavs    = 1u << 2,
    // It reads the coverage mask, which means another thing per sample.
    PixelShaderReadsCoverage = 1u << 3,
  };

  class IBackendShader {
  public:
    virtual ~IBackendShader() = default;
    virtual ShaderStage getStage() const = 0;
    virtual const void* getBytecode() const = 0;
    virtual size_t getBytecodeSize() const = 0;
    virtual uint64_t getBytecodeHash() const = 0;
    // PixelShaderTraits of a pixel shader; 0 for other stages.
    virtual uint32_t getPixelShaderTraits() const { return 0; }
    // The shader resource slots (t#) the shader declares, 128 bits; null
    // when unknown (any slot may be read).
    virtual const uint64_t* getShaderResourceSlots() const { return nullptr; }
  };

  class IBackendPipeline {
  public:
    virtual ~IBackendPipeline() = default;
  };

  struct BufferUpdateRegion {
    uint64_t offset = 0;
    uint64_t size = 0;
    const void* data = nullptr;
  };

  // Commands recorded by a deferred context, executable any number of times.
  class IBackendCommandList {
  public:
    virtual ~IBackendCommandList() = default;
  };

  class IBackendQuery {
  public:
    virtual ~IBackendQuery() = default;
    virtual QueryType getType() const = 0;
    virtual QueryStatus getData(QueryData& data) = 0;

    // Begin/End calls queued for a worker thread but not recorded yet.
    std::atomic<uint32_t> queuedOps{0};
  };

  // ============================================================
  // Abstract Context Interface
  // ============================================================

  class IBackendContext {
  public:
    virtual ~IBackendContext() = default;

    // Resource Binding
    virtual void bindVertexBuffer(uint32_t slot, IBackendBuffer* buffer, uint32_t offset, uint32_t stride) = 0;
    virtual void bindIndexBuffer(IBackendBuffer* buffer, uint32_t offset, IndexType type) = 0;
    virtual void bindConstantBuffer(ShaderStage stage, uint32_t slot, IBackendBuffer* buffer) = 0;
    virtual void bindConstantBufferRange(ShaderStage stage, uint32_t slot, IBackendBuffer* buffer,
                                          uint32_t firstConstant, uint32_t numConstants) {
      bindConstantBuffer(stage, slot, buffer);
    }
    virtual void bindShaderResource(ShaderStage stage, uint32_t slot, IBackendImageView* view) = 0;
    virtual void bindShaderResource(ShaderStage stage, uint32_t slot, IBackendBufferView* view) = 0;
    virtual void bindUnorderedAccess(ShaderStage stage, uint32_t slot, IBackendImageView* view) = 0;
    virtual void bindUnorderedAccess(ShaderStage stage, uint32_t slot, IBackendBufferView* view) = 0;
    virtual void setUnorderedAccessCounter(IBackendBufferView* view, uint32_t value) { (void)view; (void)value; }
    virtual void bindSampler(ShaderStage stage, uint32_t slot, IBackendSampler* sampler) = 0;

    // Stream Output
    virtual void bindStreamOutputTargets(uint32_t count, IBackendBuffer** buffers, const uint32_t* offsets) {
      (void)count; (void)buffers; (void)offsets;
    }
    // Bytes stream output wrote into the buffer so far.
    virtual uint64_t getStreamOutputFilledSize(IBackendBuffer* buffer) { (void)buffer; return 0; }
    // Draws the vertices stream output wrote into the buffer.
    virtual void drawAuto(IBackendBuffer* vertexBuffer, uint32_t stride) { (void)vertexBuffer; (void)stride; }

    // Render Targets
    virtual void bindRenderTargets(uint32_t count, IBackendImageView** rtvs, IBackendImageView* dsv) = 0;
    virtual bool outputMergerStateNeedsRebind() const { return true; }
    // False when outputMergerStateNeedsRebind never returns true.
    virtual bool outputMergerStateCanNeedRebind() const { return true; }

    // State
    virtual void clearState() {}
    // Forgets every bound object without recording commands. Called when
    // the owning context is destroyed, before its bindings are released.
    virtual void releaseBindings() {}
    virtual void setViewports(uint32_t count, const Viewport* viewports) = 0;
    virtual void setScissorRects(uint32_t count, const Rect* rects) = 0;
    virtual void setBlendState(const BlendState& state) = 0;
    virtual void setRasterizerState(const RasterizerState& state) = 0;
    virtual void setDepthStencilState(const DepthStencilState& state) = 0;
    virtual void setStencilRef(uint32_t ref) = 0;
    virtual void setBlendFactor(const float factor[4]) = 0;
    virtual void setPrimitiveTopology(PrimitiveTopology topology) = 0;

    // Pipeline
    virtual void bindGraphicsPipeline(IBackendPipeline* pipeline) = 0;
    virtual void bindComputePipeline(IBackendPipeline* pipeline) = 0;

    // Draw Commands
    virtual void draw(uint32_t vertexCount, uint32_t instanceCount,
                      uint32_t firstVertex, uint32_t firstInstance) = 0;
    virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                             uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) = 0;
    virtual void dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;

    // Indirect commands
    virtual void drawIndirect(IBackendBuffer* buffer, uint64_t offset) = 0;
    virtual void drawIndexedIndirect(IBackendBuffer* buffer, uint64_t offset) = 0;
    virtual void dispatchIndirect(IBackendBuffer* buffer, uint64_t offset) = 0;

    // Copy Operations
    virtual void copyBuffer(IBackendBuffer* dst, uint64_t dstOffset,
                           IBackendBuffer* src, uint64_t srcOffset, uint64_t size) = 0;
    virtual void copyImage(IBackendImage* dst, const ImageSubresource& dstSubres,
                          IBackendImage* src, const ImageSubresource& srcSubres) = 0;
    virtual void copyImageRegion(IBackendImage* dst, const ImageSubresource& dstSubres,
                   uint32_t dstX, uint32_t dstY, uint32_t dstZ,
                   IBackendImage* src, const ImageSubresource& srcSubres,
                   const ImageCopyRegion* srcRegion) = 0;
    virtual void copyBufferToImage(IBackendImage* dst, const ImageSubresource& dstSubres,
                                   IBackendBuffer* src, uint64_t srcOffset,
                                   const ImageCopyRegion* region = nullptr) = 0;
    // Writes a whole subresource from application memory through the
    // context's own upload memory; false where the caller uploads itself.
    virtual bool uploadImageSubresource(IBackendImage* dst, const ImageSubresource& dstSubres,
                                        const void* data, uint32_t rowPitch, uint32_t depthPitch) {
      (void)dst; (void)dstSubres; (void)data; (void)rowPitch; (void)depthPitch;
      return false;
    }
    virtual void copyImageToBuffer(IBackendBuffer* dst, uint64_t dstOffset,
                                   IBackendImage* src, const ImageSubresource& srcSubres) = 0;
    virtual void copyStructureCount(IBackendBuffer* dst, uint64_t dstOffset, IBackendBufferView* view) {
      (void)dst;
      (void)dstOffset;
      (void)view;
    }

    // Resource Mapping
    virtual void* mapBuffer(IBackendBuffer* buffer, MapMode mode) = 0;
    virtual void unmapBuffer(IBackendBuffer* buffer) = 0;
    virtual void updateBuffer(IBackendBuffer* buffer, uint64_t offset, uint64_t size, const void* data) = 0;
    virtual void updateBufferRegions(IBackendBuffer* buffer, const BufferUpdateRegion* regions, uint32_t regionCount) {
      if (!buffer || !regions)
        return;

      for (uint32_t i = 0; i < regionCount; i++) {
        if (regions[i].data && regions[i].size)
          updateBuffer(buffer, regions[i].offset, regions[i].size, regions[i].data);
      }
    }
    virtual void retainTemporaryBuffer(Rc<IBackendBuffer> buffer) { (void)buffer; }
    // Bytes into a dynamic buffer's memory where writes after
    // WRITE_NO_OVERWRITE go (DXUP's Direct3D 9 layer): contexts with a
    // worker thread let it write, so the application thread writes no
    // write-combined memory (each locked instruction after such writes waits
    // until they left the CPU). Bytes from size to padded are zeros (whole
    // 64-byte lines: partly written lines wait in the CPU).
    virtual void writeMappedBuffer(IBackendBuffer* buffer, uint64_t offset, const void* data, size_t size, size_t padded) {
      if (void* mapped = mapBuffer(buffer, MapMode::WriteNoOverwrite)) {
        std::memcpy(static_cast<uint8_t*>(mapped) + offset, data, size);
        if (padded > size)
          std::memset(static_cast<uint8_t*>(mapped) + offset + size, 0, padded - size);
        unmapBuffer(buffer);
      }
    }
    // The same with the bytes from fn(user, target, size, data, dataSize):
    // pack(packUser, data) fills its dataSize bytes right away, fn runs
    // where the bytes are written (the worker thread) and keeps its own
    // state there. Without a buffer, fn(user, nullptr, 0, data, dataSize)
    // runs after the commands before.
    using WriteFn = void (*)(void* user, uint8_t* target, size_t size, const void* data, size_t dataSize);
    using PackFn = void (*)(void* packUser, uint8_t* data);
    virtual void writeMappedBufferWith(IBackendBuffer* buffer, uint64_t offset, size_t size, WriteFn fn, void* user,
                                       PackFn pack, void* packUser, size_t dataSize) {
      std::vector<uint8_t> data(dataSize);
      if (pack && dataSize)
        pack(packUser, data.data());
      if (!buffer) {
        fn(user, nullptr, 0, data.data(), dataSize);
      } else if (void* mapped = mapBuffer(buffer, MapMode::WriteNoOverwrite)) {
        fn(user, static_cast<uint8_t*>(mapped) + offset, size, data.data(), dataSize);
        unmapBuffer(buffer);
      }
    }
    // WRITE_DISCARD on a buffer that can take new memory in place: returns
    // where the application writes; commands recorded before keep reading
    // the old memory. Null when the buffer has to be renamed instead.
    virtual void* discardBuffer(IBackendBuffer* buffer) { (void)buffer; return nullptr; }
    // Points views at the buffer that replaced theirs, in order with the
    // commands: those recorded before still read the old buffer.
    virtual void retargetBufferViews(IBackendBuffer* buffer, const std::vector<IBackendBufferView*>& views) {
      for (IBackendBufferView* view : views)
        view->retargetBuffer(buffer);
    }
    // Keeps retired backend objects until the GPU finished the commands
    // recorded so far; moves them out of objects.
    virtual void retireAfterGpu(std::vector<std::shared_ptr<void>>& objects) { objects.clear(); }

    // Queries
    virtual void beginQuery(IBackendQuery* query) = 0;
    virtual void endQuery(IBackendQuery* query) = 0;
    // Skips draws, dispatches, clears and copies while the predicate's
    // result equals value. A null predicate turns predication off.
    virtual void setPredication(IBackendQuery* predicate, bool value) { (void)predicate; (void)value; }
    // Clamps sampling of the image to mips at or above minLod, in order with
    // the other commands (D3D11 SetResourceMinLOD).
    virtual void setImageMinLod(IBackendImage* image, float minLod) { (void)image; (void)minLod; }
    // The image is the resource (an ID3D12Resource; null: none) from now
    // on, in order with the other commands: a swap chain buffer the
    // application draws into directly, the real buffer Present shows next.
    // Its views are written again; it starts in the present state.
    virtual void retargetImage(IBackendImage* image, IUnknown* resource) { (void)image; (void)resource; }
    // A buffer of a swap chain on this device (an ID3D12Resource), for
    // dxup_memory_stats.txt.
    virtual void trackSwapChainBuffer(IUnknown* resource) { (void)resource; }
    // Submits the commands so far, then makes the queue signal the fence
    // (an ID3D12Fence) to value, or wait for it before later commands.
    virtual void signalFence(IUnknown* fence, uint64_t value) { (void)fence; (void)value; }
    virtual void waitFence(IUnknown* fence, uint64_t value) { (void)fence; (void)value; }
    // Submits the commands so far and sets the event when the GPU is done.
    virtual void notifyOnCompletion(void* event) { (void)event; }
    // Tiled resources (D3D11.2). Tile pools grow by newSize - current size.
    virtual void updateTileMappings(const TileMappingUpdate& update) { (void)update; }
    virtual void copyTileMappings(TiledResourceRef dst, const TileCoordinate& dstCoordinate,
                                  TiledResourceRef src, const TileCoordinate& srcCoordinate,
                                  const TileRegionSize& size, uint32_t flags) {
      (void)dst; (void)dstCoordinate; (void)src; (void)srcCoordinate; (void)size; (void)flags;
    }
    virtual void copyTiles(TiledResourceRef resource, const TileCoordinate& coordinate, const TileRegionSize& size,
                           IBackendBuffer* buffer, uint64_t bufferOffset, uint32_t flags) {
      (void)resource; (void)coordinate; (void)size; (void)buffer; (void)bufferOffset; (void)flags;
    }
    // data holds numTiles * kTileSizeInBytes bytes.
    virtual void updateTiles(TiledResourceRef resource, const TileCoordinate& coordinate, const TileRegionSize& size,
                             const void* data, uint32_t flags) {
      (void)resource; (void)coordinate; (void)size; (void)data; (void)flags;
    }
    virtual bool resizeTilePool(IBackendBuffer* tilePool, uint64_t newSize) { (void)tilePool; (void)newSize; return false; }
    virtual void tiledResourceBarrier() {}
    // Keeps predication off for internal work such as the present copy.
    virtual void suspendPredication(bool suspended) { (void)suspended; }

    // Synchronization
    virtual void flush() = 0;
    virtual void synchronize() = 0;
    // Waits until the GPU finished the recorded work that uses a staging
    // resource, submitting it if needed. With doNotWait it only submits
    // and returns false while the GPU still uses the resource.
    virtual bool waitForGpuUse(BackendGpuUse& use, bool doNotWait) {
      (void)use;
      (void)doNotWait;
      synchronize();
      return true;
    }

    // A staging image the CPU writes while the GPU still reads it (an
    // upload copied every frame): gives it other memory holding the same
    // contents instead of waiting for the GPU. False when it was not
    // renamed (no GPU work uses it, the GPU writes it, too big, mapped).
    virtual bool renameStagingImage(IBackendImage* image) {
      (void)image;
      return false;
    }

    // Deferred context support
    virtual void closeForSubmission() {}
    virtual void submitRecordedCommands() {}

    // Present support (transition back buffer + flush)
    virtual void presentImage(IBackendImage* backBuffer) { flush(); }

    // D3D11 maximum frame latency: blocks until at most maxLatency presented
    // frames are still being rendered. Called after the swap chain Present.
    virtual void waitForFrameLatency(uint32_t maxLatency) { (void)maxLatency; }

    // Ends a frame whose present was issued through executeOnBackend, so
    // that waitForFrameLatency can count it.
    virtual void endFrame() {}

    // Runs fn with the context that records the commands, in order with
    // every command issued before. Contexts that record on the calling
    // thread run it right away.
    virtual void executeOnBackend(std::function<void(IBackendContext*)> fn) { fn(this); }

    // Blocks until every command issued so far has been recorded.
    virtual void waitForWorker() {}
    // A flush not run yet submits everything issued so far.
    virtual bool flushPending() const { return false; }

    // Deferred contexts: hands out everything recorded since the last call.
    virtual Rc<IBackendCommandList> finishCommandList() { return nullptr; }
    // Executes a command list after the commands issued so far. It starts
    // from the default state and leaves its state bound.
    virtual void executeCommandList(const Rc<IBackendCommandList>& list) { (void)list; }

    // Clear
    virtual void clearRenderTarget(IBackendImageView* rtv, const float color[4]) = 0;
    virtual void clearDepthStencil(IBackendImageView* dsv, float depth, uint8_t stencil, uint32_t flags) = 0;
    virtual void clearUnorderedAccessViewUint(IBackendImageView* view, const uint32_t values[4]) { (void)view; (void)values; }
    virtual void clearUnorderedAccessViewUint(IBackendBufferView* view, const uint32_t values[4]) { (void)view; (void)values; }
    virtual void clearUnorderedAccessViewFloat(IBackendImageView* view, const float values[4]) { (void)view; (void)values; }
    virtual void clearUnorderedAccessViewFloat(IBackendBufferView* view, const float values[4]) { (void)view; (void)values; }
    // ClearView: clears a render target or image UAV, limited to rects when
    // rectCount is not zero.
    virtual void clearView(IBackendImageView* view, bool renderTarget, const float color[4],
                           uint32_t rectCount, const Rect* rects) {
      (void)rectCount;
      (void)rects;
      if (renderTarget)
        clearRenderTarget(view, color);
      else
        clearUnorderedAccessViewFloat(view, color);
    }
    // ClearView of a buffer UAV: rects give element ranges (left to right).
    virtual void clearBufferView(IBackendBufferView* view, const float color[4], uint32_t rectCount, const Rect* rects) {
      (void)rectCount;
      (void)rects;
      clearUnorderedAccessViewFloat(view, color);
    }

    // Misc
    virtual void generateMips(IBackendImageView* srv) = 0;
    virtual void resolveSubresource(IBackendImage* dst, uint32_t dstSubres,
                                     IBackendImage* src, uint32_t srcSubres, DXGI_FORMAT format) = 0;

    // Forced multisampling. upsampleImage writes a single-sampled image into
    // every sample of a multisampled one of the same size: color in format,
    // depth (format a depth view format, readFormat how a shader reads the
    // source) without stencil. resolveSampleZero writes the first sample of
    // each pixel of a multisampled image into a single-sampled one: depth
    // (format a depth view format, readFormat how a shader reads it) or color
    // (readFormat the same as format).
    virtual void upsampleImage(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format,
                               DXGI_FORMAT readFormat) = 0;
    virtual void resolveSampleZero(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format,
                                   DXGI_FORMAT readFormat) = 0;
    // The average of each pixel's finite samples of a multisampled float
    // color image (the first sample where none is: data packed into float
    // bits stays as it is): a shader that yields NaN at one sample position
    // does not blacken the pixel.
    virtual void resolveFinite(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format) = 0;
    // The stencil of one depth-stencil image into another of the same size
    // (a multisampled one and a single-sampled one, either way; sample 0 of
    // a multisampled source): format is their depth-stencil format,
    // readFormat the view format of the stencil plane.
    virtual void copyStencil(IBackendImage* dst, IBackendImage* src, DXGI_FORMAT format, DXGI_FORMAT readFormat) = 0;
  };

  // ============================================================
  // Abstract Device Interface
  // ============================================================

  class IBackendDevice {
  public:
    virtual ~IBackendDevice() = default;

    // Resource Creation
    virtual Rc<IBackendBuffer> createBuffer(const BufferDesc& desc) = 0;
    virtual Rc<IBackendImage> createImage(const ImageDesc& desc) = 0;
    virtual Rc<IBackendSampler> createSampler(const SamplerDesc& desc) = 0;

    // View Creation
    virtual Rc<IBackendBufferView> createBufferView(IBackendBuffer* buffer, const BufferViewDesc& desc) = 0;
    virtual Rc<IBackendImageView> createImageView(IBackendImage* image, const ImageViewDesc& desc) = 0;

    // Shader/Pipeline
    virtual Rc<IBackendShader> createShader(const ShaderDesc& desc) = 0;
    // The device keeps every pipeline it returns until it is destroyed, so
    // contexts and the commands they queue refer to pipelines by pointer.
    virtual Rc<IBackendPipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual Rc<IBackendPipeline> createComputePipeline(const ComputePipelineDesc& desc) = 0;
    // Asynchronous compilation (d3d11.asyncShaders). A pipeline that is not
    // compiled yet is handed to background threads: requestGraphicsPipeline
    // then returns null with pending set until it is ready. Without
    // background threads it compiles like createGraphicsPipeline. Pipelines
    // being compiled make the create functions wait for them.
    virtual bool asyncPipelines() const { return false; }
    // Draws skipped since the last call because their pipeline could not be
    // compiled (glitch reports).
    virtual uint32_t takeFailedPipelineDraws() { return 0; }
    // Background threads compile pipelines (precompiled ones, compute
    // pipelines of new shaders); nothing is skipped while they do.
    virtual bool compilesInBackground() const { return false; }
    virtual Rc<IBackendPipeline> requestGraphicsPipeline(const GraphicsPipelineDesc& desc, bool& pending) {
      pending = false;
      return createGraphicsPipeline(desc);
    }
    // Starts compiling a compute pipeline in the background.
    virtual void requestComputePipeline(const ComputePipelineDesc& desc) { (void)desc; }
    // A pipeline not compiled yet compiles on a background thread; the
    // pipeline returned stands for it, and the context that binds it waits
    // for the compiled one when it executes the bind (on its command thread,
    // not the application's). Null only for pipelines that failed before.
    virtual Rc<IBackendPipeline> createGraphicsPipelineDeferred(const GraphicsPipelineDesc& desc) {
      return createGraphicsPipeline(desc);
    }

    // Query
    virtual Rc<IBackendQuery> createQuery(const QueryDesc& desc) = 0;

    // Context
    virtual Rc<IBackendContext> createContext() = 0;
    // Lightweight context for copy-only operations (no descriptor heaps/root signature)
    virtual Rc<IBackendContext> createCopyContext() { return createContext(); }

    // Initial data of new resources: record into the context between these
    // calls (from any thread). The uploads reach the GPU before the next
    // submission of the immediate context, so resources are never used
    // before their data arrived.
    virtual Rc<IBackendContext> beginInitialData() { return createCopyContext(); }
    virtual void endInitialData(IBackendContext* context) { if (context) context->flush(); }

    // Feature Support
    virtual bool checkFeatureSupport(BackendFeature feature) = 0;
    virtual BackendLimits getLimits() = 0;
    virtual BackendType getBackendType() const = 0;
    virtual uint32_t checkMultisampleQualityLevels(DXGI_FORMAT format, uint32_t sampleCount) {
      return (sampleCount == 1) ? 1 : 0;
    }

    // Native queue access for swap chain creation
    virtual void* getNativeCommandQueue() { return nullptr; }

    // Set when objects must outlive the commands a worker thread has queued.
    virtual std::shared_ptr<BackendRetireQueue> getRetireQueue() const { return nullptr; }
  };

}
