#include "legacy_direct.h"

#include "../d3d11/d3d11_device.h"
#include "../d3d12/d3d12_backend_device.h"
#include "../d3d12/d3d12_backend_context.h"
#include "../util/util_paths.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace dxup::legacy {

  namespace {

    constexpr UINT kStreams = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    constexpr UINT kConstantSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    constexpr UINT kViewSlots = 32;   // the legacy core binds no more
    constexpr UINT kSamplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
    constexpr UINT kTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
    constexpr UINT kViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    // Calls the legacy core does not make: seen in the log if that changes.
    void unusedCall(const char* name) {
      static std::atomic<uint32_t> reports{0};
      if (reports.fetch_add(1, std::memory_order_relaxed) < 32)
        logMessage(LogLevel::Error, "d3d9.backend = d3d12: the legacy core called %s, which is not translated", name);
    }

    uint32_t nextObjectId() {
      static std::atomic<uint32_t> next{1};
      return next.fetch_add(1, std::memory_order_relaxed);
    }

    class DirectDevice;
    void retainDevice(DirectDevice* device);
    void releaseDevice(DirectDevice* device);

    // A device child. References of the application count as in COM. A
    // binding keeps an object as a context reference does in Direct3D 11 (the
    // core tells bound objects by their addresses), without a locked
    // instruction per bind: the core's calls on the context do not overlap,
    // and it releases bound objects only between them.
    template<typename Base>
    class Child : public Base {
    public:
      explicit Child(DirectDevice* device) : m_device(device) { retainDevice(device); }
      virtual ~Child() { releaseDevice(m_device); }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object)
          return E_POINTER;
        *object = nullptr;
        bool known = riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(Base);
        if constexpr (std::is_base_of_v<ID3D11Resource, Base>)
          known = known || riid == __uuidof(ID3D11Resource);
        if constexpr (std::is_base_of_v<ID3D11View, Base>)
          known = known || riid == __uuidof(ID3D11View);
        if constexpr (std::is_base_of_v<ID3D11Asynchronous, Base>)
          known = known || riid == __uuidof(ID3D11Asynchronous);
        if (!known)
          return E_NOINTERFACE;
        AddRef();
        *object = static_cast<Base*>(this);
        return S_OK;
      }

      ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
      }

      ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!refs && !m_bound)
          destroy();
        return refs;
      }

      void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override;
      HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT* size, void*) override {
        if (size)
          *size = 0;
        return DXGI_ERROR_NOT_FOUND;
      }
      HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
      HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

      void noteBound() { m_bound++; }
      void noteUnbound() {
        if (!--m_bound && !m_refs.load(std::memory_order_acquire))
          destroy();
      }

    protected:
      DirectDevice* m_device;

    private:
      void destroy() {
        if (!processExiting() && !m_destroying.exchange(true))
          delete this;
      }

      std::atomic<ULONG> m_refs{1};
      uint32_t           m_bound = 0;
      std::atomic<bool>  m_destroying{false};
    };

    class DirectBuffer final : public Child<ID3D11Buffer> {
    public:
      DirectBuffer(DirectDevice* device, const D3D11_BUFFER_DESC& desc, Rc<IBackendBuffer> buffer)
      : Child(device), raw(buffer.get()), m_desc(desc), m_buffer(std::move(buffer)) { }

      void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dimension) override { *dimension = D3D11_RESOURCE_DIMENSION_BUFFER; }
      void STDMETHODCALLTYPE SetEvictionPriority(UINT) override { }
      UINT STDMETHODCALLTYPE GetEvictionPriority() override { return DXGI_RESOURCE_PRIORITY_NORMAL; }
      void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* desc) override { *desc = m_desc; }

      const D3D11_BUFFER_DESC& desc() const { return m_desc; }

      // New memory for WRITE_DISCARD; the old for the commands recorded before.
      Rc<IBackendBuffer> replace(Rc<IBackendBuffer> buffer, IBackendContext* context) {
        Rc<IBackendBuffer> old = std::move(m_buffer);
        m_buffer = std::move(buffer);
        raw = m_buffer.get();
        if (old && m_buffer)
          context->noteBufferRenamed(old, m_buffer);
        return old;
      }

      IBackendBuffer* raw;

    private:
      D3D11_BUFFER_DESC  m_desc;
      Rc<IBackendBuffer> m_buffer;
    };

    class DirectTexture2D final : public Child<ID3D11Texture2D> {
    public:
      DirectTexture2D(DirectDevice* device, const D3D11_TEXTURE2D_DESC& desc, Rc<IBackendImage> image)
      : Child(device), raw(image.get()), m_desc(desc), m_image(std::move(image)) { }

      void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dimension) override { *dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
      void STDMETHODCALLTYPE SetEvictionPriority(UINT) override { }
      UINT STDMETHODCALLTYPE GetEvictionPriority() override { return DXGI_RESOURCE_PRIORITY_NORMAL; }
      void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* desc) override { *desc = m_desc; }

      IBackendImage* raw;

    private:
      D3D11_TEXTURE2D_DESC m_desc;
      Rc<IBackendImage>    m_image;
    };

    class DirectTexture3D final : public Child<ID3D11Texture3D> {
    public:
      DirectTexture3D(DirectDevice* device, const D3D11_TEXTURE3D_DESC& desc, Rc<IBackendImage> image)
      : Child(device), raw(image.get()), m_desc(desc), m_image(std::move(image)) { }

      void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dimension) override { *dimension = D3D11_RESOURCE_DIMENSION_TEXTURE3D; }
      void STDMETHODCALLTYPE SetEvictionPriority(UINT) override { }
      UINT STDMETHODCALLTYPE GetEvictionPriority() override { return DXGI_RESOURCE_PRIORITY_NORMAL; }
      void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE3D_DESC* desc) override { *desc = m_desc; }

      IBackendImage* raw;

    private:
      D3D11_TEXTURE3D_DESC m_desc;
      Rc<IBackendImage>    m_image;
    };

    IBackendImage* imageOf(ID3D11Resource* resource) {
      D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dimension);
      if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return static_cast<DirectTexture2D*>(static_cast<ID3D11Texture2D*>(resource))->raw;
      if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
        return static_cast<DirectTexture3D*>(static_cast<ID3D11Texture3D*>(resource))->raw;
      return nullptr;
    }

    DirectBuffer* bufferOf(ID3D11Resource* resource) {
      D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      resource->GetType(&dimension);
      return dimension == D3D11_RESOURCE_DIMENSION_BUFFER
        ? static_cast<DirectBuffer*>(static_cast<ID3D11Buffer*>(resource)) : nullptr;
    }

    // The part of a resource a view covers (d3d11_binding_hazards.h). Planes
    // 1: color or depth, 2: stencil.
    struct ViewRange {
      ID3D11Resource* resource = nullptr;
      uint32_t mipBegin = 0, mipEnd = 0, layerBegin = 0, layerEnd = 0;
      uint32_t planes = 1;
    };

    inline bool rangesOverlap(const ViewRange& a, const ViewRange& b) {
      return a.resource == b.resource && (a.planes & b.planes)
        && a.mipBegin < b.mipEnd && b.mipBegin < a.mipEnd
        && a.layerBegin < b.layerEnd && b.layerBegin < a.layerEnd;
    }

    ViewRange rangeOf(ID3D11Resource* resource, const IBackendImage* image, const ImageViewDesc& desc) {
      ViewRange range;
      range.resource = resource;
      range.mipBegin = desc.baseMipLevel;
      range.mipEnd = desc.baseMipLevel + desc.mipLevelCount;
      // Volume textures have one layer: their slices count as all of it.
      const bool volume = image->getDesc().type == ImageType::Tex3D;
      range.layerBegin = volume ? 0u : desc.baseArrayLayer;
      range.layerEnd = volume ? 1u : desc.baseArrayLayer + desc.arrayLayerCount;
      return range;
    }

    template<typename Base>
    class DirectView : public Child<Base> {
    public:
      DirectView(DirectDevice* device, ID3D11Resource* resource, Rc<IBackendImageView> view, const ViewRange& viewRange)
      : Child<Base>(device), raw(view.get()), range(viewRange), m_view(std::move(view)) {
        range.resource->AddRef();
      }
      ~DirectView() override {
        ID3D11Resource* resource = range.resource;
        m_view = nullptr;
        resource->Release();
      }

      void STDMETHODCALLTYPE GetResource(ID3D11Resource** resource) override {
        range.resource->AddRef();
        *resource = range.resource;
      }

      IBackendImageView* raw;
      ViewRange          range;

    private:
      Rc<IBackendImageView> m_view;
    };

    class DirectSrv final : public DirectView<ID3D11ShaderResourceView> {
    public:
      DirectSrv(DirectDevice* device, ID3D11Resource* resource, Rc<IBackendImageView> view, const ViewRange& range,
                const D3D11_SHADER_RESOURCE_VIEW_DESC& desc)
      : DirectView(device, resource, std::move(view), range), m_desc(desc) { }
      void STDMETHODCALLTYPE GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* desc) override { *desc = m_desc; }
    private:
      D3D11_SHADER_RESOURCE_VIEW_DESC m_desc;
    };

    class DirectRtv final : public DirectView<ID3D11RenderTargetView> {
    public:
      DirectRtv(DirectDevice* device, ID3D11Resource* resource, Rc<IBackendImageView> view, const ViewRange& range,
                const D3D11_RENDER_TARGET_VIEW_DESC& desc, DXGI_FORMAT format, uint32_t samples, uint32_t quality)
      : DirectView(device, resource, std::move(view), range), pipelineFormat(format), sampleCount(samples),
        sampleQuality(quality), m_desc(desc) { }
      void STDMETHODCALLTYPE GetDesc(D3D11_RENDER_TARGET_VIEW_DESC* desc) override { *desc = m_desc; }
      DXGI_FORMAT pipelineFormat;
      uint32_t    sampleCount, sampleQuality;
    private:
      D3D11_RENDER_TARGET_VIEW_DESC m_desc;
    };

    class DirectDsv final : public DirectView<ID3D11DepthStencilView> {
    public:
      DirectDsv(DirectDevice* device, ID3D11Resource* resource, Rc<IBackendImageView> view, const ViewRange& range,
                const D3D11_DEPTH_STENCIL_VIEW_DESC& desc, DXGI_FORMAT format, uint32_t samples, uint32_t quality)
      : DirectView(device, resource, std::move(view), range), pipelineFormat(format), sampleCount(samples),
        sampleQuality(quality), m_desc(desc) { }
      void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC* desc) override { *desc = m_desc; }
      DXGI_FORMAT pipelineFormat;
      uint32_t    sampleCount, sampleQuality;
    private:
      D3D11_DEPTH_STENCIL_VIEW_DESC m_desc;
    };

    template<typename Base>
    class DirectShader final : public Child<Base> {
    public:
      DirectShader(DirectDevice* device, Rc<IBackendShader> shader)
      : Child<Base>(device), id(nextObjectId()), raw(shader.get()), m_shader(std::move(shader)) { }
      uint32_t        id;
      IBackendShader* raw;
    private:
      Rc<IBackendShader> m_shader;
    };
    using DirectVertexShader = DirectShader<ID3D11VertexShader>;
    using DirectPixelShader = DirectShader<ID3D11PixelShader>;
    using DirectGeometryShader = DirectShader<ID3D11GeometryShader>;

    class DirectInputLayout final : public Child<ID3D11InputLayout> {
    public:
      DirectInputLayout(DirectDevice* device, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count)
      : Child(device), id(nextObjectId()), m_names(count), m_elements(count) {
        for (UINT i = 0; i < count; i++) {
          m_names[i] = elements[i].SemanticName ? elements[i].SemanticName : "";
          InputElementDesc& e = m_elements[i];
          e.semanticName = m_names[i].c_str();
          e.semanticIndex = elements[i].SemanticIndex;
          e.format = elements[i].Format;
          e.inputSlot = elements[i].InputSlot;
          e.byteOffset = elements[i].AlignedByteOffset;
          e.inputClass = elements[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA
            ? InputClassification::PerInstance : InputClassification::PerVertex;
          e.instanceDataStepRate = elements[i].InstanceDataStepRate;
        }
        hash = hashInputLayout(m_elements);
      }
      const std::vector<InputElementDesc>& elements() const { return m_elements; }
      uint32_t id;
      uint64_t hash = 0;
    private:
      std::vector<std::string>      m_names;
      std::vector<InputElementDesc> m_elements;
    };

    class DirectBlendState final : public Child<ID3D11BlendState> {
    public:
      DirectBlendState(DirectDevice* device, const D3D11_BLEND_DESC& desc)
      : Child(device), id(nextObjectId()), state(convertBlendDesc(&desc)), hash(state_hash::blend(state)), m_desc(desc) { }
      void STDMETHODCALLTYPE GetDesc(D3D11_BLEND_DESC* desc) override { *desc = m_desc; }
      uint32_t   id;
      BlendState state;
      uint64_t   hash;
    private:
      D3D11_BLEND_DESC m_desc;
    };

    class DirectDepthState final : public Child<ID3D11DepthStencilState> {
    public:
      DirectDepthState(DirectDevice* device, const D3D11_DEPTH_STENCIL_DESC& desc)
      : Child(device), id(nextObjectId()), state(convertDepthStencilDesc(&desc)), hash(state_hash::depthStencil(state)), m_desc(desc) { }
      void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_DESC* desc) override { *desc = m_desc; }
      uint32_t          id;
      DepthStencilState state;
      uint64_t          hash;
    private:
      D3D11_DEPTH_STENCIL_DESC m_desc;
    };

    class DirectRasterState final : public Child<ID3D11RasterizerState> {
    public:
      DirectRasterState(DirectDevice* device, const D3D11_RASTERIZER_DESC& desc)
      : Child(device), id(nextObjectId()), state(convertRasterizerDesc(&desc)), hash(state_hash::rasterizer(state)), m_desc(desc) { }
      void STDMETHODCALLTYPE GetDesc(D3D11_RASTERIZER_DESC* desc) override { *desc = m_desc; }
      uint32_t        id;
      RasterizerState state;
      uint64_t        hash;
    private:
      D3D11_RASTERIZER_DESC m_desc;
    };

    class DirectSampler final : public Child<ID3D11SamplerState> {
    public:
      DirectSampler(DirectDevice* device, const D3D11_SAMPLER_DESC& desc, Rc<IBackendSampler> sampler)
      : Child(device), raw(sampler.get()), m_desc(desc), m_sampler(std::move(sampler)) { }
      void STDMETHODCALLTYPE GetDesc(D3D11_SAMPLER_DESC* desc) override { *desc = m_desc; }
      IBackendSampler* raw;
    private:
      D3D11_SAMPLER_DESC  m_desc;
      Rc<IBackendSampler> m_sampler;
    };

    class DirectQuery final : public Child<ID3D11Query> {
    public:
      DirectQuery(DirectDevice* device, const D3D11_QUERY_DESC& desc, Rc<IBackendQuery> query)
      : Child(device), raw(query.get()), m_desc(desc), m_query(std::move(query)) { }
      UINT STDMETHODCALLTYPE GetDataSize() override {
        switch (m_desc.Query) {
          case D3D11_QUERY_EVENT: return sizeof(BOOL);
          case D3D11_QUERY_OCCLUSION: case D3D11_QUERY_TIMESTAMP: return sizeof(UINT64);
          case D3D11_QUERY_TIMESTAMP_DISJOINT: return sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT);
          default: return 0;
        }
      }
      void STDMETHODCALLTYPE GetDesc(D3D11_QUERY_DESC* desc) override { *desc = m_desc; }
      // One flush per query end: games poll many queries in a row.
      bool requestFlush() { return !m_flushRequested.exchange(true, std::memory_order_relaxed); }
      void resetFlushRequest() { m_flushRequested.store(false, std::memory_order_relaxed); }
      IBackendQuery* raw;
    private:
      D3D11_QUERY_DESC  m_desc;
      Rc<IBackendQuery> m_query;
      std::atomic<bool> m_flushRequested{false};
    };

    // What a graphics pipeline is made of. The context keeps it up to date
    // call by call, so a draw that needs another pipeline only hashes it.
    // Objects count by numbers of their own: an address may come back with
    // another object. The fields are 32 bits each: hashed right after being
    // written, narrower ones would wait in the store buffer.
    struct PipelineKey {
      uint32_t vs = 0, ps = 0, gs = 0, layout = 0, blend = 0, raster = 0, depth = 0;
      uint32_t sampleMask = D3D11_DEFAULT_SAMPLE_MASK;
      uint32_t formats[2] = {};      // render target formats, a byte each
      uint32_t dsvFormat = 0;
      uint32_t samples = 1;          // sample count, render target count << 16
      uint32_t sampleQuality = 0;
      uint32_t topology = 0;         // topology, strip cut index type << 8

      uint32_t hash() const {
        constexpr size_t kWords = sizeof(PipelineKey) / sizeof(uint32_t);
        static_assert(kWords % 2 == 0, "two lanes");
        const auto* w = reinterpret_cast<const uint32_t*>(this);
        uint32_t a = 0x9e3779b9u, b = 0x85ebca6bu;
        for (size_t i = 0; i < kWords; i += 2) {
          a = _rotl((a ^ w[i]) * 0xcc9e2d51u, 15);
          b = _rotl((b ^ w[i + 1]) * 0x1b873593u, 13);
        }
        uint32_t h = a ^ _rotl(b, 16);
        h ^= h >> 15;
        h *= 0x85ebca6bu;
        h ^= h >> 13;
        return h | 1u;   // 0 marks a free slot
      }
    };
    static_assert(sizeof(PipelineKey) == 14 * sizeof(uint32_t), "no padding: compared bytewise");

    // Probes the hashes apart from the keys; the device keeps every pipeline.
    class PipelineTable {
    public:
      IBackendPipeline* find(const PipelineKey& key, uint32_t hash) const {
        if (m_hashes.empty())
          return nullptr;
        const size_t mask = m_hashes.size() - 1;
        for (size_t i = hash & mask; ; i = (i + 1) & mask) {
          const uint32_t slot = m_hashes[i];
          if (!slot)
            return nullptr;
          if (slot == hash && !std::memcmp(&m_entries[i].key, &key, sizeof(key)))
            return m_entries[i].pipeline;
        }
      }

      void insert(const PipelineKey& key, uint32_t hash, IBackendPipeline* pipeline) {
        // Objects get new numbers when games make them again: a table this
        // full starts over rather than growing without bound. The device
        // still has the pipelines.
        if (m_count >= kMaxEntries) {
          m_hashes.clear();
          m_entries.clear();
          m_count = 0;
        }
        if ((m_count + 1) * 2 > m_hashes.size())
          grow();
        place(key, hash, pipeline);
      }

    private:
      static constexpr size_t kMaxEntries = 16384;

      struct Entry {
        PipelineKey       key;
        IBackendPipeline* pipeline = nullptr;
      };

      void place(const PipelineKey& key, uint32_t hash, IBackendPipeline* pipeline) {
        const size_t mask = m_hashes.size() - 1;
        size_t i = hash & mask;
        while (m_hashes[i])
          i = (i + 1) & mask;
        m_hashes[i] = hash;
        m_entries[i].key = key;
        m_entries[i].pipeline = pipeline;
        m_count++;
      }

      void grow() {
        std::vector<uint32_t> hashes = std::move(m_hashes);
        std::vector<Entry> entries = std::move(m_entries);
        const size_t capacity = hashes.empty() ? 1024 : hashes.size() * 2;
        m_hashes.assign(capacity, 0);
        m_entries.assign(capacity, Entry{});
        m_count = 0;
        for (size_t i = 0; i < hashes.size(); i++)
          if (hashes[i])
            place(entries[i].key, hashes[i], entries[i].pipeline);
      }

      std::vector<uint32_t> m_hashes;
      std::vector<Entry>    m_entries;
      size_t                m_count = 0;
    };

    PrimitiveTopology convertTopology(D3D11_PRIMITIVE_TOPOLOGY topology) {
      switch (topology) {
        case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:     return PrimitiveTopology::PointList;
        case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:      return PrimitiveTopology::LineList;
        case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:     return PrimitiveTopology::LineStrip;
        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:  return PrimitiveTopology::TriangleList;
        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return PrimitiveTopology::TriangleStrip;
        default:                                     return PrimitiveTopology::Undefined;
      }
    }

    uint32_t formatElementBytes(DXGI_FORMAT format) {
      const uint32_t blockBytes = getFormatBlockBytes(format);
      return blockBytes ? blockBytes : getFormatBpp(format);
    }

    // D3D11 copies between formats of one family, and between block
    // compressed and uncompressed formats whose texels are as large as the
    // blocks; it drops other copies (native runtime).
    bool copyableFormats(DXGI_FORMAT dst, DXGI_FORMAT src) {
      const uint32_t dstElement = formatElementBytes(dst);
      const uint32_t srcElement = formatElementBytes(src);
      if (!dstElement || !srcElement)
        return true;
      if (dstElement != srcElement)
        return false;
      return getFormatFamily(dst) == getFormatFamily(src) || !getFormatBlockBytes(dst) != !getFormatBlockBytes(src);
    }

    uint64_t uploadSizeClass(uint64_t size) {
      if (size <= 65536)
        return size;
      uint64_t power = 1;
      while (power < size)
        power <<= 1;
      const uint64_t step = power / 8;
      return (size + step - 1) / step * step;
    }

    class DirectContext final : public ID3D11DeviceContext1 {
    public:
      DirectContext(DirectDevice* device, Rc<IBackendContext> context)
      : m_device(device), m_owned(std::move(context)), m_ctx(m_owned.get()),
        m_omRebinds(m_ctx->outputMergerStateCanNeedRebind()) {
        m_stages[0].stage = ShaderStage::Vertex;
        m_stages[1].stage = ShaderStage::Geometry;
        m_stages[2].stage = ShaderStage::Pixel;
      }

      ~DirectContext() {
        // The backend forgets its bindings first: dropping ours may destroy
        // views it would otherwise apply again.
        m_ctx->releaseBindings();
        dropBindings();
      }

      IBackendContext* backend() const { return m_ctx; }

      // ---- IUnknown ----

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object)
          return E_POINTER;
        *object = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11DeviceContext)
         || riid == __uuidof(ID3D11DeviceContext1)) {
          AddRef();
          *object = static_cast<ID3D11DeviceContext1*>(this);
          return S_OK;
        }
        return E_NOINTERFACE;
      }

      // As in D3D11, references to the immediate context are the device's.
      ULONG STDMETHODCALLTYPE AddRef() override;
      ULONG STDMETHODCALLTYPE Release() override;

      void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override;
      HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT* size, void*) override {
        if (size)
          *size = 0;
        return DXGI_ERROR_NOT_FOUND;
      }
      HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
      HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

      D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override { return D3D11_DEVICE_CONTEXT_IMMEDIATE; }
      UINT STDMETHODCALLTYPE GetContextFlags() override { return 0; }

      // ---- Input assembler ----

      void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout* layout) override {
        auto* value = static_cast<DirectInputLayout*>(layout);
        if (m_layout == value)
          return;
        DirectInputLayout* old = m_layout;
        if (value)
          value->noteBound();
        m_layout = value;
        m_key.layout = value ? value->id : 0;
        m_pipelineDirty = true;
        if (old)
          old->noteUnbound();
      }

      void STDMETHODCALLTYPE IASetVertexBuffers(UINT start, UINT count, ID3D11Buffer* const* buffers, const UINT* strides,
                                                const UINT* offsets) override {
        for (UINT i = 0; i < count; i++) {
          const UINT slot = start + i;
          if (slot >= kStreams)
            break;
          auto* buffer = static_cast<DirectBuffer*>(buffers ? buffers[i] : nullptr);
          const UINT stride = strides ? strides[i] : 0;
          const UINT offset = offsets ? offsets[i] : 0;
          DirectBuffer* old = m_vertexBuffers[slot];
          if (old == buffer && m_vertexStrides[slot] == stride && m_vertexOffsets[slot] == offset)
            continue;
          if (buffer)
            buffer->noteBound();
          m_vertexBuffers[slot] = buffer;
          m_vertexStrides[slot] = stride;
          m_vertexOffsets[slot] = offset;
          m_ctx->bindVertexBuffer(slot, buffer ? buffer->raw : nullptr, offset, stride);
          if (old)
            old->noteUnbound();
        }
      }

      void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer* indexBuffer, DXGI_FORMAT format, UINT offset) override {
        auto* buffer = static_cast<DirectBuffer*>(indexBuffer);
        DirectBuffer* old = m_indexBuffer;
        if (old == buffer && m_indexFormat == format && m_indexOffset == offset)
          return;
        if (buffer)
          buffer->noteBound();
        const bool formatChanged = m_indexFormat != format;
        m_indexBuffer = buffer;
        m_indexFormat = format;
        m_indexOffset = offset;
        m_ctx->bindIndexBuffer(buffer ? buffer->raw : nullptr, offset,
          format == DXGI_FORMAT_R32_UINT ? IndexType::UInt32 : IndexType::UInt16);
        if (old)
          old->noteUnbound();
        if (formatChanged)
          updateTopologyKey();
      }

      void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology) override {
        if (m_topology == topology)
          return;
        m_topology = topology;
        m_ctx->setPrimitiveTopology(convertTopology(topology));
        updateTopologyKey();
      }

      // ---- Shaders ----

      void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader* shader, ID3D11ClassInstance* const*, UINT) override {
        setShader(m_vs, static_cast<DirectVertexShader*>(shader), m_key.vs);
      }
      void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader* shader, ID3D11ClassInstance* const*, UINT) override {
        setShader(m_ps, static_cast<DirectPixelShader*>(shader), m_key.ps);
      }
      void STDMETHODCALLTYPE GSSetShader(ID3D11GeometryShader* shader, ID3D11ClassInstance* const*, UINT) override {
        setShader(m_gs, static_cast<DirectGeometryShader*>(shader), m_key.gs);
      }

      void STDMETHODCALLTYPE VSSetConstantBuffers(UINT start, UINT count, ID3D11Buffer* const* buffers) override {
        setConstantBuffers(m_stages[0], start, count, buffers, nullptr, nullptr);
      }
      void STDMETHODCALLTYPE GSSetConstantBuffers(UINT start, UINT count, ID3D11Buffer* const* buffers) override {
        setConstantBuffers(m_stages[1], start, count, buffers, nullptr, nullptr);
      }
      void STDMETHODCALLTYPE PSSetConstantBuffers(UINT start, UINT count, ID3D11Buffer* const* buffers) override {
        setConstantBuffers(m_stages[2], start, count, buffers, nullptr, nullptr);
      }
      void STDMETHODCALLTYPE VSSetConstantBuffers1(UINT start, UINT count, ID3D11Buffer* const* buffers, const UINT* first,
                                                   const UINT* constants) override {
        setConstantBuffers(m_stages[0], start, count, buffers, first, constants);
      }
      void STDMETHODCALLTYPE GSSetConstantBuffers1(UINT start, UINT count, ID3D11Buffer* const* buffers, const UINT* first,
                                                   const UINT* constants) override {
        setConstantBuffers(m_stages[1], start, count, buffers, first, constants);
      }
      void STDMETHODCALLTYPE PSSetConstantBuffers1(UINT start, UINT count, ID3D11Buffer* const* buffers, const UINT* first,
                                                   const UINT* constants) override {
        setConstantBuffers(m_stages[2], start, count, buffers, first, constants);
      }

      void STDMETHODCALLTYPE VSSetShaderResources(UINT start, UINT count, ID3D11ShaderResourceView* const* views) override {
        setShaderResources(m_stages[0], start, count, views);
      }
      void STDMETHODCALLTYPE GSSetShaderResources(UINT start, UINT count, ID3D11ShaderResourceView* const* views) override {
        setShaderResources(m_stages[1], start, count, views);
      }
      void STDMETHODCALLTYPE PSSetShaderResources(UINT start, UINT count, ID3D11ShaderResourceView* const* views) override {
        setShaderResources(m_stages[2], start, count, views);
      }

      void STDMETHODCALLTYPE VSSetSamplers(UINT start, UINT count, ID3D11SamplerState* const* samplers) override {
        setSamplers(m_stages[0], start, count, samplers);
      }
      void STDMETHODCALLTYPE GSSetSamplers(UINT start, UINT count, ID3D11SamplerState* const* samplers) override {
        setSamplers(m_stages[1], start, count, samplers);
      }
      void STDMETHODCALLTYPE PSSetSamplers(UINT start, UINT count, ID3D11SamplerState* const* samplers) override {
        setSamplers(m_stages[2], start, count, samplers);
      }

      // ---- Output merger ----

      void STDMETHODCALLTYPE OMSetRenderTargets(UINT count, ID3D11RenderTargetView* const* views,
                                                ID3D11DepthStencilView* depthView) override {
        DirectRtv* desired[kTargets] = {};
        count = (std::min)(count, kTargets);
        UINT active = 0;
        for (UINT i = 0; i < count; i++) {
          desired[i] = static_cast<DirectRtv*>(views ? views[i] : nullptr);
          if (desired[i])
            active = i + 1;
        }
        auto* dsv = static_cast<DirectDsv*>(depthView);
        if (m_dsv == dsv && m_targetCount == active && !std::memcmp(desired, m_rtvs, sizeof(desired)))
          return;

        DirectRtv* oldTargets[kTargets];
        std::memcpy(oldTargets, m_rtvs, sizeof(oldTargets));
        DirectDsv* oldDsv = m_dsv;
        for (DirectRtv* view : desired)
          if (view)
            view->noteBound();
        if (dsv)
          dsv->noteBound();
        std::memcpy(m_rtvs, desired, sizeof(m_rtvs));
        m_dsv = dsv;
        m_targetCount = active;

        updateTargets();
        rebindOutputMerger();
        for (DirectRtv* view : oldTargets)
          if (view)
            view->noteUnbound();
        if (oldDsv)
          oldDsv->noteUnbound();
      }

      void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState* state, const FLOAT factor[4], UINT sampleMask) override {
        auto* value = static_cast<DirectBlendState*>(state);
        DirectBlendState* old = m_blend;
        if (old != value) {
          if (value)
            value->noteBound();
          m_blend = value;
          m_key.blend = value ? value->id : 0;
          m_pipelineDirty = true;
          if (old)
            old->noteUnbound();
        }
        if (m_key.sampleMask != sampleMask) {
          m_key.sampleMask = sampleMask;
          m_pipelineDirty = true;
        }
        static const float kOnes[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        const float* used = factor ? factor : kOnes;
        if (std::memcmp(m_blendFactor, used, sizeof(m_blendFactor))) {
          std::memcpy(m_blendFactor, used, sizeof(m_blendFactor));
          m_ctx->setBlendFactor(used);
        }
      }

      void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState* state, UINT stencilRef) override {
        auto* value = static_cast<DirectDepthState*>(state);
        DirectDepthState* old = m_depth;
        if (old != value) {
          if (value)
            value->noteBound();
          m_depth = value;
          m_key.depth = value ? value->id : 0;
          m_pipelineDirty = true;
          if (old)
            old->noteUnbound();
        }
        if (m_stencilRef != stencilRef) {
          m_stencilRef = stencilRef;
          m_ctx->setStencilRef(stencilRef);
        }
      }

      // ---- Rasterizer ----

      void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState* state) override {
        auto* value = static_cast<DirectRasterState*>(state);
        DirectRasterState* old = m_raster;
        if (old == value)
          return;
        if (value)
          value->noteBound();
        m_raster = value;
        m_key.raster = value ? value->id : 0;
        m_pipelineDirty = true;
        applyScissor();
        if (old)
          old->noteUnbound();
      }

      void STDMETHODCALLTYPE RSSetViewports(UINT count, const D3D11_VIEWPORT* viewports) override {
        // D3D11 ignores the call for a viewport out of bounds or with a depth
        // range outside [0, 1] or inverted; D3D12 would draw with it.
        if (count > kViewports || (count && !viewports))
          return;
        if (m_viewportCount == count && (!count || !std::memcmp(m_viewports, viewports, sizeof(D3D11_VIEWPORT) * count)))
          return;
        for (UINT i = 0; i < count; i++) {
          const D3D11_VIEWPORT& v = viewports[i];
          if (!(v.MinDepth >= 0.0f && v.MaxDepth <= 1.0f && v.MinDepth <= v.MaxDepth
             && v.Width >= 0.0f && v.Height >= 0.0f
             && v.TopLeftX >= float(D3D11_VIEWPORT_BOUNDS_MIN) && v.TopLeftY >= float(D3D11_VIEWPORT_BOUNDS_MIN)
             && v.TopLeftX + v.Width <= float(D3D11_VIEWPORT_BOUNDS_MAX)
             && v.TopLeftY + v.Height <= float(D3D11_VIEWPORT_BOUNDS_MAX)))
            return;
        }
        m_viewportCount = count;
        if (count) {
          Viewport backend[kViewports];
          for (UINT i = 0; i < count; i++) {
            m_viewports[i] = viewports[i];
            backend[i] = { viewports[i].TopLeftX, viewports[i].TopLeftY, viewports[i].Width, viewports[i].Height,
                           viewports[i].MinDepth, viewports[i].MaxDepth };
          }
          m_ctx->setViewports(count, backend);
        } else {
          m_ctx->setViewports(0, nullptr);
        }
        if (!scissorEnabled())
          applyScissor();
      }

      void STDMETHODCALLTYPE RSSetScissorRects(UINT count, const D3D11_RECT* rects) override {
        if (count > kViewports || (count && !rects))
          return;
        if (m_scissorCount == count && (!count || !std::memcmp(m_scissors, rects, sizeof(D3D11_RECT) * count)))
          return;
        m_scissorCount = count;
        for (UINT i = 0; i < count; i++)
          m_scissors[i] = rects[i];
        if (scissorEnabled())
          applyScissor();
      }

      // ---- Draws ----

      void STDMETHODCALLTYPE Draw(UINT count, UINT start) override {
        if (prepareDraw())
          m_ctx->draw(count, 1, start, 0);
      }

      void STDMETHODCALLTYPE DrawIndexed(UINT count, UINT start, INT base) override {
        if (prepareDraw())
          m_ctx->drawIndexed(count, 1, start, base, 0);
      }

      void STDMETHODCALLTYPE DrawInstanced(UINT count, UINT instances, UINT start, UINT firstInstance) override {
        if (prepareDraw())
          m_ctx->draw(count, instances, start, firstInstance);
      }

      void STDMETHODCALLTYPE DrawIndexedInstanced(UINT count, UINT instances, UINT start, INT base, UINT firstInstance) override {
        if (prepareDraw())
          m_ctx->drawIndexed(count, instances, start, base, firstInstance);
      }

      // ---- Map ----

      HRESULT STDMETHODCALLTYPE Map(ID3D11Resource* resource, UINT subresource, D3D11_MAP type, UINT flags,
                                    D3D11_MAPPED_SUBRESOURCE* mapped) override {
        if (!resource || !mapped)
          return E_INVALIDARG;
        if (DirectBuffer* buffer = bufferOf(resource))
          return mapBuffer(buffer, type, flags, mapped);

        // The legacy core maps textures it made for the CPU only.
        IBackendImage* image = imageOf(resource);
        if (!image || !image->isCpuAccessible())
          return E_INVALIDARG;
        // An upload the GPU still copies from gets other memory with the
        // same contents instead of waiting, as D3D11 drivers rename it.
        const bool renamed = type != D3D11_MAP_READ && m_ctx->renameStagingImage(image);
        if (!renamed && !m_ctx->waitForGpuUse(image->gpuUse, (flags & D3D11_MAP_FLAG_DO_NOT_WAIT) != 0))
          return DXGI_ERROR_WAS_STILL_DRAWING;
        ImageSubresourceLayout layout;
        uint8_t* data = image->mapStaging(subresource, convertMapType(type), layout);
        if (!data)
          return E_INVALIDARG;
        mapped->pData = data;
        mapped->RowPitch = layout.rowPitch;
        mapped->DepthPitch = layout.depthPitch;
        return S_OK;
      }

      void STDMETHODCALLTYPE Unmap(ID3D11Resource* resource, UINT subresource) override {
        if (!resource)
          return;
        if (DirectBuffer* buffer = bufferOf(resource)) {
          m_ctx->unmapBuffer(buffer->raw);
        } else if (IBackendImage* image = imageOf(resource)) {
          if (image->isCpuAccessible())
            image->unmapStaging(subresource);
        }
      }

      void writeBuffer(DirectBuffer* buffer, UINT offset, const void* data, UINT size) {
        const uint64_t lineEnd = (std::min)((uint64_t(offset) + size + 63) & ~uint64_t(63), uint64_t(buffer->desc().ByteWidth));
        m_ctx->writeMappedBuffer(buffer->raw, offset, data, size, size_t(lineEnd - offset));
      }

      void writeBufferWith(DirectBuffer* buffer, UINT offset, UINT size, IBackendContext::WriteFn fn, void* user,
                           IBackendContext::PackFn pack, void* packUser, UINT dataSize) {
        m_ctx->writeMappedBufferWith(buffer ? buffer->raw : nullptr, offset, size, fn, user, pack, packUser, dataSize);
      }

      // ---- Updates and copies ----

      void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource* resource, UINT subresource, const D3D11_BOX* box, const void* data,
                                               UINT rowPitch, UINT depthPitch) override;
      void STDMETHODCALLTYPE CopyResource(ID3D11Resource* dst, ID3D11Resource* src) override;
      void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource* dst, UINT dstSubresource, UINT dstX, UINT dstY, UINT dstZ,
                                                   ID3D11Resource* src, UINT srcSubresource, const D3D11_BOX* srcBox) override;
      void STDMETHODCALLTYPE ResolveSubresource(ID3D11Resource* dst, UINT dstSubresource, ID3D11Resource* src, UINT srcSubresource,
                                                DXGI_FORMAT format) override;

      void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView* view) override {
        if (view)
          m_ctx->generateMips(static_cast<DirectSrv*>(view)->raw);
      }

      // ---- Clears ----

      void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView* view, const FLOAT color[4]) override {
        if (view && color)
          m_ctx->clearRenderTarget(static_cast<DirectRtv*>(view)->raw, color);
      }

      void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil) override {
        if (!view)
          return;
        auto* dsv = static_cast<DirectDsv*>(view);
        // Only planes the view writes are cleared (native runtime).
        flags &= ((dsv->range.planes & 1u) ? D3D11_CLEAR_DEPTH : 0u) | ((dsv->range.planes & 2u) ? D3D11_CLEAR_STENCIL : 0u);
        if (flags)
          m_ctx->clearDepthStencil(dsv->raw, depth, stencil, flags);
      }

      void STDMETHODCALLTYPE ClearState() override {
        m_ctx->clearState();
        dropBindings();
        m_key = PipelineKey{};
        m_pipelineDirty = true;
        m_currentPipeline = nullptr;
        m_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        m_indexFormat = DXGI_FORMAT_UNKNOWN;
        m_indexOffset = 0;
        std::memset(m_vertexStrides, 0, sizeof(m_vertexStrides));
        std::memset(m_vertexOffsets, 0, sizeof(m_vertexOffsets));
        m_viewportCount = 0;
        m_scissorCount = 0;
        m_targetCount = 0;
        m_writeCount = 0;
        m_stencilRef = 0;
        static const float kOnes[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        std::memcpy(m_blendFactor, kOnes, sizeof(m_blendFactor));
        // The command list keeps its dynamic state: back to the defaults.
        m_ctx->setBlendFactor(m_blendFactor);
        m_ctx->setStencilRef(0);
        m_ctx->setViewports(0, nullptr);
      }

      void STDMETHODCALLTYPE Flush() override { m_ctx->flush(); }

      // ---- Queries ----

      void STDMETHODCALLTYPE Begin(ID3D11Asynchronous* async) override {
        if (async)
          m_ctx->beginQuery(static_cast<DirectQuery*>(async)->raw);
      }

      void STDMETHODCALLTYPE End(ID3D11Asynchronous* async) override {
        if (!async)
          return;
        auto* query = static_cast<DirectQuery*>(async);
        query->resetFlushRequest();
        m_ctx->endQuery(query->raw);
      }

      HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous* async, void* data, UINT size, UINT flags) override {
        if (!async)
          return E_INVALIDARG;
        auto* query = static_cast<DirectQuery*>(async);
        if (size && (!data || size != query->GetDataSize()))
          return E_INVALIDARG;
        if (!size)
          data = nullptr;
        QueryData result;
        const QueryStatus status = query->raw->getData(result);
        if (status == QueryStatus::Invalid)
          return DXGI_ERROR_INVALID_CALL;
        if (status != QueryStatus::Ready) {
          // A flush the worker has yet to run submits this query too.
          if (status == QueryStatus::NotSubmitted && !(flags & D3D11_ASYNC_GETDATA_DONOTFLUSH) && !m_ctx->flushPending()
           && query->requestFlush())
            Flush();
          return S_FALSE;
        }
        if (!data)
          return S_OK;
        D3D11_QUERY_DESC desc;
        query->GetDesc(&desc);
        switch (desc.Query) {
          case D3D11_QUERY_EVENT:
            *static_cast<BOOL*>(data) = TRUE;
            break;
          case D3D11_QUERY_OCCLUSION:
            *static_cast<UINT64*>(data) = result.samples;
            break;
          case D3D11_QUERY_TIMESTAMP:
            *static_cast<UINT64*>(data) = result.timestamp;
            break;
          case D3D11_QUERY_TIMESTAMP_DISJOINT: {
            auto* disjoint = static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(data);
            disjoint->Frequency = result.frequency;
            disjoint->Disjoint = result.disjoint ? TRUE : FALSE;
            break;
          }
          default:
            return E_INVALIDARG;
        }
        return S_OK;
      }

      // ---- What the legacy core does not call ----

#define DXUP_UNUSED(name) unusedCall(#name)
      void STDMETHODCALLTYPE SetPredication(ID3D11Predicate*, BOOL) override { DXUP_UNUSED(SetPredication); }
      void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*) override { DXUP_UNUSED(OMSetRenderTargetsAndUnorderedAccessViews); }
      void STDMETHODCALLTYPE SOSetTargets(UINT, ID3D11Buffer* const*, const UINT*) override { DXUP_UNUSED(SOSetTargets); }
      void STDMETHODCALLTYPE DrawAuto() override { DXUP_UNUSED(DrawAuto); }
      void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(ID3D11Buffer*, UINT) override { DXUP_UNUSED(DrawIndexedInstancedIndirect); }
      void STDMETHODCALLTYPE DrawInstancedIndirect(ID3D11Buffer*, UINT) override { DXUP_UNUSED(DrawInstancedIndirect); }
      void STDMETHODCALLTYPE Dispatch(UINT, UINT, UINT) override { DXUP_UNUSED(Dispatch); }
      void STDMETHODCALLTYPE DispatchIndirect(ID3D11Buffer*, UINT) override { DXUP_UNUSED(DispatchIndirect); }
      void STDMETHODCALLTYPE CopyStructureCount(ID3D11Buffer*, UINT, ID3D11UnorderedAccessView*) override { DXUP_UNUSED(CopyStructureCount); }
      void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView*, const UINT[4]) override { DXUP_UNUSED(ClearUnorderedAccessViewUint); }
      void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView*, const FLOAT[4]) override { DXUP_UNUSED(ClearUnorderedAccessViewFloat); }
      void STDMETHODCALLTYPE SetResourceMinLOD(ID3D11Resource*, FLOAT) override { DXUP_UNUSED(SetResourceMinLOD); }
      FLOAT STDMETHODCALLTYPE GetResourceMinLOD(ID3D11Resource*) override { DXUP_UNUSED(GetResourceMinLOD); return 0.0f; }
      void STDMETHODCALLTYPE ExecuteCommandList(ID3D11CommandList*, BOOL) override { DXUP_UNUSED(ExecuteCommandList); }
      void STDMETHODCALLTYPE HSSetShaderResources(UINT, UINT, ID3D11ShaderResourceView* const*) override { DXUP_UNUSED(HSSetShaderResources); }
      void STDMETHODCALLTYPE HSSetShader(ID3D11HullShader*, ID3D11ClassInstance* const*, UINT) override { DXUP_UNUSED(HSSetShader); }
      void STDMETHODCALLTYPE HSSetSamplers(UINT, UINT, ID3D11SamplerState* const*) override { DXUP_UNUSED(HSSetSamplers); }
      void STDMETHODCALLTYPE HSSetConstantBuffers(UINT, UINT, ID3D11Buffer* const*) override { DXUP_UNUSED(HSSetConstantBuffers); }
      void STDMETHODCALLTYPE DSSetShaderResources(UINT, UINT, ID3D11ShaderResourceView* const*) override { DXUP_UNUSED(DSSetShaderResources); }
      void STDMETHODCALLTYPE DSSetShader(ID3D11DomainShader*, ID3D11ClassInstance* const*, UINT) override { DXUP_UNUSED(DSSetShader); }
      void STDMETHODCALLTYPE DSSetSamplers(UINT, UINT, ID3D11SamplerState* const*) override { DXUP_UNUSED(DSSetSamplers); }
      void STDMETHODCALLTYPE DSSetConstantBuffers(UINT, UINT, ID3D11Buffer* const*) override { DXUP_UNUSED(DSSetConstantBuffers); }
      void STDMETHODCALLTYPE CSSetShaderResources(UINT, UINT, ID3D11ShaderResourceView* const*) override { DXUP_UNUSED(CSSetShaderResources); }
      void STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*) override { DXUP_UNUSED(CSSetUnorderedAccessViews); }
      void STDMETHODCALLTYPE CSSetShader(ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT) override { DXUP_UNUSED(CSSetShader); }
      void STDMETHODCALLTYPE CSSetSamplers(UINT, UINT, ID3D11SamplerState* const*) override { DXUP_UNUSED(CSSetSamplers); }
      void STDMETHODCALLTYPE CSSetConstantBuffers(UINT, UINT, ID3D11Buffer* const*) override { DXUP_UNUSED(CSSetConstantBuffers); }
      void STDMETHODCALLTYPE VSGetConstantBuffers(UINT, UINT, ID3D11Buffer**) override { DXUP_UNUSED(VSGetConstantBuffers); }
      void STDMETHODCALLTYPE PSGetShaderResources(UINT, UINT, ID3D11ShaderResourceView**) override { DXUP_UNUSED(PSGetShaderResources); }
      void STDMETHODCALLTYPE PSGetShader(ID3D11PixelShader**, ID3D11ClassInstance**, UINT*) override { DXUP_UNUSED(PSGetShader); }
      void STDMETHODCALLTYPE PSGetSamplers(UINT, UINT, ID3D11SamplerState**) override { DXUP_UNUSED(PSGetSamplers); }
      void STDMETHODCALLTYPE VSGetShader(ID3D11VertexShader**, ID3D11ClassInstance**, UINT*) override { DXUP_UNUSED(VSGetShader); }
      void STDMETHODCALLTYPE PSGetConstantBuffers(UINT, UINT, ID3D11Buffer**) override { DXUP_UNUSED(PSGetConstantBuffers); }
      void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout**) override { DXUP_UNUSED(IAGetInputLayout); }
      void STDMETHODCALLTYPE IAGetVertexBuffers(UINT, UINT, ID3D11Buffer**, UINT*, UINT*) override { DXUP_UNUSED(IAGetVertexBuffers); }
      void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer**, DXGI_FORMAT*, UINT*) override { DXUP_UNUSED(IAGetIndexBuffer); }
      void STDMETHODCALLTYPE GSGetConstantBuffers(UINT, UINT, ID3D11Buffer**) override { DXUP_UNUSED(GSGetConstantBuffers); }
      void STDMETHODCALLTYPE GSGetShader(ID3D11GeometryShader**, ID3D11ClassInstance**, UINT*) override { DXUP_UNUSED(GSGetShader); }
      void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY*) override { DXUP_UNUSED(IAGetPrimitiveTopology); }
      void STDMETHODCALLTYPE VSGetShaderResources(UINT, UINT, ID3D11ShaderResourceView**) override { DXUP_UNUSED(VSGetShaderResources); }
      void STDMETHODCALLTYPE VSGetSamplers(UINT, UINT, ID3D11SamplerState**) override { DXUP_UNUSED(VSGetSamplers); }
      void STDMETHODCALLTYPE GetPredication(ID3D11Predicate**, BOOL*) override { DXUP_UNUSED(GetPredication); }
      void STDMETHODCALLTYPE GSGetShaderResources(UINT, UINT, ID3D11ShaderResourceView**) override { DXUP_UNUSED(GSGetShaderResources); }
      void STDMETHODCALLTYPE GSGetSamplers(UINT, UINT, ID3D11SamplerState**) override { DXUP_UNUSED(GSGetSamplers); }
      void STDMETHODCALLTYPE OMGetRenderTargets(UINT, ID3D11RenderTargetView**, ID3D11DepthStencilView**) override { DXUP_UNUSED(OMGetRenderTargets); }
      void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(UINT, ID3D11RenderTargetView**, ID3D11DepthStencilView**, UINT, UINT, ID3D11UnorderedAccessView**) override { DXUP_UNUSED(OMGetRenderTargetsAndUnorderedAccessViews); }
      void STDMETHODCALLTYPE OMGetBlendState(ID3D11BlendState**, FLOAT[4], UINT*) override { DXUP_UNUSED(OMGetBlendState); }
      void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D11DepthStencilState**, UINT*) override { DXUP_UNUSED(OMGetDepthStencilState); }
      void STDMETHODCALLTYPE SOGetTargets(UINT, ID3D11Buffer**) override { DXUP_UNUSED(SOGetTargets); }
      void STDMETHODCALLTYPE RSGetState(ID3D11RasterizerState**) override { DXUP_UNUSED(RSGetState); }
      void STDMETHODCALLTYPE RSGetViewports(UINT*, D3D11_VIEWPORT*) override { DXUP_UNUSED(RSGetViewports); }
      void STDMETHODCALLTYPE RSGetScissorRects(UINT*, D3D11_RECT*) override { DXUP_UNUSED(RSGetScissorRects); }
      void STDMETHODCALLTYPE HSGetShaderResources(UINT, UINT, ID3D11ShaderResourceView**) override { DXUP_UNUSED(HSGetShaderResources); }
      void STDMETHODCALLTYPE HSGetShader(ID3D11HullShader**, ID3D11ClassInstance**, UINT*) override { DXUP_UNUSED(HSGetShader); }
      void STDMETHODCALLTYPE HSGetSamplers(UINT, UINT, ID3D11SamplerState**) override { DXUP_UNUSED(HSGetSamplers); }
      void STDMETHODCALLTYPE HSGetConstantBuffers(UINT, UINT, ID3D11Buffer**) override { DXUP_UNUSED(HSGetConstantBuffers); }
      void STDMETHODCALLTYPE DSGetShaderResources(UINT, UINT, ID3D11ShaderResourceView**) override { DXUP_UNUSED(DSGetShaderResources); }
      void STDMETHODCALLTYPE DSGetShader(ID3D11DomainShader**, ID3D11ClassInstance**, UINT*) override { DXUP_UNUSED(DSGetShader); }
      void STDMETHODCALLTYPE DSGetSamplers(UINT, UINT, ID3D11SamplerState**) override { DXUP_UNUSED(DSGetSamplers); }
      void STDMETHODCALLTYPE DSGetConstantBuffers(UINT, UINT, ID3D11Buffer**) override { DXUP_UNUSED(DSGetConstantBuffers); }
      void STDMETHODCALLTYPE CSGetShaderResources(UINT, UINT, ID3D11ShaderResourceView**) override { DXUP_UNUSED(CSGetShaderResources); }
      void STDMETHODCALLTYPE CSGetUnorderedAccessViews(UINT, UINT, ID3D11UnorderedAccessView**) override { DXUP_UNUSED(CSGetUnorderedAccessViews); }
      void STDMETHODCALLTYPE CSGetShader(ID3D11ComputeShader**, ID3D11ClassInstance**, UINT*) override { DXUP_UNUSED(CSGetShader); }
      void STDMETHODCALLTYPE CSGetSamplers(UINT, UINT, ID3D11SamplerState**) override { DXUP_UNUSED(CSGetSamplers); }
      void STDMETHODCALLTYPE CSGetConstantBuffers(UINT, UINT, ID3D11Buffer**) override { DXUP_UNUSED(CSGetConstantBuffers); }
      HRESULT STDMETHODCALLTYPE FinishCommandList(BOOL, ID3D11CommandList**) override { DXUP_UNUSED(FinishCommandList); return DXGI_ERROR_INVALID_CALL; }
      void STDMETHODCALLTYPE CopySubresourceRegion1(ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*, UINT) override { DXUP_UNUSED(CopySubresourceRegion1); }
      void STDMETHODCALLTYPE UpdateSubresource1(ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT) override { DXUP_UNUSED(UpdateSubresource1); }
      void STDMETHODCALLTYPE DiscardResource(ID3D11Resource*) override { DXUP_UNUSED(DiscardResource); }
      void STDMETHODCALLTYPE DiscardView(ID3D11View*) override { DXUP_UNUSED(DiscardView); }
      void STDMETHODCALLTYPE HSSetConstantBuffers1(UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*) override { DXUP_UNUSED(HSSetConstantBuffers1); }
      void STDMETHODCALLTYPE DSSetConstantBuffers1(UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*) override { DXUP_UNUSED(DSSetConstantBuffers1); }
      void STDMETHODCALLTYPE CSSetConstantBuffers1(UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*) override { DXUP_UNUSED(CSSetConstantBuffers1); }
      void STDMETHODCALLTYPE VSGetConstantBuffers1(UINT, UINT, ID3D11Buffer**, UINT*, UINT*) override { DXUP_UNUSED(VSGetConstantBuffers1); }
      void STDMETHODCALLTYPE HSGetConstantBuffers1(UINT, UINT, ID3D11Buffer**, UINT*, UINT*) override { DXUP_UNUSED(HSGetConstantBuffers1); }
      void STDMETHODCALLTYPE DSGetConstantBuffers1(UINT, UINT, ID3D11Buffer**, UINT*, UINT*) override { DXUP_UNUSED(DSGetConstantBuffers1); }
      void STDMETHODCALLTYPE GSGetConstantBuffers1(UINT, UINT, ID3D11Buffer**, UINT*, UINT*) override { DXUP_UNUSED(GSGetConstantBuffers1); }
      void STDMETHODCALLTYPE PSGetConstantBuffers1(UINT, UINT, ID3D11Buffer**, UINT*, UINT*) override { DXUP_UNUSED(PSGetConstantBuffers1); }
      void STDMETHODCALLTYPE CSGetConstantBuffers1(UINT, UINT, ID3D11Buffer**, UINT*, UINT*) override { DXUP_UNUSED(CSGetConstantBuffers1); }
      void STDMETHODCALLTYPE SwapDeviceContextState(ID3DDeviceContextState*, ID3DDeviceContextState**) override { DXUP_UNUSED(SwapDeviceContextState); }
      void STDMETHODCALLTYPE ClearView(ID3D11View*, const FLOAT[4], const D3D11_RECT*, UINT) override { DXUP_UNUSED(ClearView); }
      void STDMETHODCALLTYPE DiscardView1(ID3D11View*, const D3D11_RECT*, UINT) override { DXUP_UNUSED(DiscardView1); }
#undef DXUP_UNUSED

    private:
      struct Stage {
        ShaderStage    stage = ShaderStage::Vertex;
        UINT           viewEnd = 0;
        DirectSrv*     views[kViewSlots] = {};
        DirectSampler* samplers[kSamplerSlots] = {};
        DirectBuffer*  constants[kConstantSlots] = {};
        UINT           constantFirst[kConstantSlots] = {};
        UINT           constantCount[kConstantSlots] = {};
      };

      template<typename T>
      void setShader(T*& slot, T* value, uint32_t& keyField) {
        T* old = slot;
        if (old == value)
          return;
        if (value)
          value->noteBound();
        slot = value;
        keyField = value ? value->id : 0;
        m_pipelineDirty = true;
        if (old)
          old->noteUnbound();
      }

      void setConstantBuffers(Stage& stage, UINT start, UINT count, ID3D11Buffer* const* buffers, const UINT* first,
                              const UINT* constants) {
        for (UINT i = 0; i < count; i++) {
          const UINT slot = start + i;
          if (slot >= kConstantSlots)
            break;
          auto* buffer = static_cast<DirectBuffer*>(buffers ? buffers[i] : nullptr);
          const UINT firstConstant = first ? first[i] : 0;
          const UINT constantCount = constants ? constants[i] : 0;
          DirectBuffer* old = stage.constants[slot];
          if (old == buffer && stage.constantFirst[slot] == firstConstant && stage.constantCount[slot] == constantCount)
            continue;
          if (buffer)
            buffer->noteBound();
          stage.constants[slot] = buffer;
          stage.constantFirst[slot] = firstConstant;
          stage.constantCount[slot] = constantCount;
          if (firstConstant || constantCount)
            m_ctx->bindConstantBufferRange(stage.stage, slot, buffer ? buffer->raw : nullptr, firstConstant, constantCount);
          else
            m_ctx->bindConstantBuffer(stage.stage, slot, buffer ? buffer->raw : nullptr);
          if (old)
            old->noteUnbound();
        }
      }

      // A view of something bound for writing binds as NULL (native runtime).
      DirectSrv* readable(DirectSrv* view) const {
        for (UINT i = 0; i < m_writeCount; i++)
          if (rangesOverlap(view->range, m_writes[i]))
            return nullptr;
        return view;
      }

      void setShaderResources(Stage& stage, UINT start, UINT count, ID3D11ShaderResourceView* const* views) {
        for (UINT i = 0; i < count; i++) {
          const UINT slot = start + i;
          if (slot >= kViewSlots)
            break;
          auto* requested = static_cast<DirectSrv*>(views ? views[i] : nullptr);
          DirectSrv* old = stage.views[slot];
          if (requested == old)
            continue;
          DirectSrv* view = requested ? readable(requested) : nullptr;
          if (view == old)
            continue;
          if (view) {
            view->noteBound();
            if (slot >= stage.viewEnd)
              stage.viewEnd = slot + 1;
          }
          stage.views[slot] = view;
          m_ctx->bindShaderResource(stage.stage, slot, view ? view->raw : static_cast<IBackendImageView*>(nullptr));
          if (old)
            old->noteUnbound();
        }
      }

      void setSamplers(Stage& stage, UINT start, UINT count, ID3D11SamplerState* const* samplers) {
        for (UINT i = 0; i < count; i++) {
          const UINT slot = start + i;
          if (slot >= kSamplerSlots)
            break;
          auto* sampler = static_cast<DirectSampler*>(samplers ? samplers[i] : nullptr);
          DirectSampler* old = stage.samplers[slot];
          if (old == sampler)
            continue;
          if (sampler)
            sampler->noteBound();
          stage.samplers[slot] = sampler;
          m_ctx->bindSampler(stage.stage, slot, sampler ? sampler->raw : nullptr);
          if (old)
            old->noteUnbound();
        }
      }

      // What the bound targets mean for pipelines and reads: the formats and
      // the sample count, and what views may no longer read.
      void updateTargets() {
        uint32_t formats[2] = {};
        uint32_t samples = 1, quality = 0;
        bool sampled = false;
        m_writeCount = 0;
        for (UINT i = 0; i < m_targetCount; i++) {
          DirectRtv* view = m_rtvs[i];
          if (!view)
            continue;
          formats[i / 4] |= uint32_t(uint8_t(view->pipelineFormat)) << (i % 4 * 8);
          if (!sampled) {
            samples = view->sampleCount;
            quality = view->sampleQuality;
            sampled = true;
          }
          m_writes[m_writeCount++] = view->range;
        }
        if (m_dsv) {
          if (!sampled || samples == 1) {
            samples = m_dsv->sampleCount;
            quality = m_dsv->sampleQuality;
          }
          if (m_dsv->range.planes)
            m_writes[m_writeCount++] = m_dsv->range;
        }
        m_key.formats[0] = formats[0];
        m_key.formats[1] = formats[1];
        m_key.dsvFormat = m_dsv ? uint32_t(m_dsv->pipelineFormat) : 0u;
        m_key.samples = samples | m_targetCount << 16;
        m_key.sampleQuality = quality;
        m_pipelineDirty = true;
        // Binding for writing unbinds the reads of the same parts.
        for (UINT w = 0; w < m_writeCount; w++)
          for (Stage& stage : m_stages)
            for (UINT slot = 0; slot < stage.viewEnd; slot++) {
              DirectSrv* view = stage.views[slot];
              if (view && rangesOverlap(view->range, m_writes[w])) {
                stage.views[slot] = nullptr;
                m_ctx->bindShaderResource(stage.stage, slot, static_cast<IBackendImageView*>(nullptr));
                view->noteUnbound();
              }
            }
      }

      void rebindOutputMerger() {
        IBackendImageView* targets[kTargets] = {};
        for (UINT i = 0; i < m_targetCount; i++)
          targets[i] = m_rtvs[i] ? m_rtvs[i]->raw : nullptr;
        m_ctx->bindRenderTargets(m_targetCount, targets, m_dsv ? m_dsv->raw : nullptr);
      }

      void updateTopologyKey() {
        const PrimitiveTopology topology = convertTopology(m_topology);
        const bool strip = topology == PrimitiveTopology::LineStrip || topology == PrimitiveTopology::TriangleStrip;
        const bool wide = strip && m_indexFormat == DXGI_FORMAT_R32_UINT;
        const uint32_t key = uint32_t(topology) | (wide ? 1u << 8 : 0u);
        if (m_key.topology != key) {
          m_key.topology = key;
          m_pipelineDirty = true;
        }
      }

      bool scissorEnabled() const { return m_raster && m_raster->state.scissorEnable; }

      void applyScissor() {
        dxup::Rect rects[kViewports] = {};
        if (scissorEnabled() && m_scissorCount) {
          for (UINT i = 0; i < m_scissorCount; i++)
            rects[i] = { m_scissors[i].left, m_scissors[i].top, m_scissors[i].right, m_scissors[i].bottom };
          m_ctx->setScissorRects(m_scissorCount, rects);
        } else if (scissorEnabled()) {
          // Scissoring without rects: nothing is drawn (native runtime).
          m_ctx->setScissorRects((std::max)(1u, m_viewportCount), rects);
        } else if (m_viewportCount) {
          for (UINT i = 0; i < m_viewportCount; i++) {
            const D3D11_VIEWPORT& v = m_viewports[i];
            rects[i] = { int32_t(v.TopLeftX), int32_t(v.TopLeftY), int32_t(v.TopLeftX + v.Width), int32_t(v.TopLeftY + v.Height) };
          }
          m_ctx->setScissorRects(m_viewportCount, rects);
        }
      }

      __forceinline bool prepareDraw() {
        if (m_pipelineDirty && !preparePipeline())
          return false;
        // Present and copies took the targets out of their state.
        if (m_omRebinds && m_ctx->outputMergerStateNeedsRebind() && (m_targetCount || m_dsv))
          rebindOutputMerger();
        return true;
      }

      bool preparePipeline() {
        // Without a topology the native runtime draws nothing.
        if (m_topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
          return false;
        const uint32_t hash = m_key.hash();
        IBackendPipeline* pipeline = m_pipelines.find(m_key, hash);
        if (!pipeline) {
          pipeline = createPipeline();
          if (!pipeline)
            return false;
          m_pipelines.insert(m_key, hash, pipeline);
        }
        if (pipeline != m_currentPipeline) {
          m_currentPipeline = pipeline;
          m_ctx->bindGraphicsPipeline(pipeline);
        }
        m_pipelineDirty = false;
        return true;
      }

      IBackendPipeline* createPipeline();

      HRESULT mapBuffer(DirectBuffer* buffer, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped);
      void rebindRenamedBuffer(DirectBuffer* buffer);

      void dropBindings() {
        for (Stage& stage : m_stages) {
          for (DirectSrv*& view : stage.views)
            if (DirectSrv* old = std::exchange(view, nullptr))
              old->noteUnbound();
          stage.viewEnd = 0;
          for (DirectSampler*& sampler : stage.samplers)
            if (DirectSampler* old = std::exchange(sampler, nullptr))
              old->noteUnbound();
          for (UINT slot = 0; slot < kConstantSlots; slot++) {
            stage.constantFirst[slot] = 0;
            stage.constantCount[slot] = 0;
            if (DirectBuffer* old = std::exchange(stage.constants[slot], nullptr))
              old->noteUnbound();
          }
        }
        for (DirectBuffer*& buffer : m_vertexBuffers)
          if (DirectBuffer* old = std::exchange(buffer, nullptr))
            old->noteUnbound();
        if (DirectBuffer* old = std::exchange(m_indexBuffer, nullptr))
          old->noteUnbound();
        for (DirectRtv*& view : m_rtvs)
          if (DirectRtv* old = std::exchange(view, nullptr))
            old->noteUnbound();
        if (DirectDsv* old = std::exchange(m_dsv, nullptr))
          old->noteUnbound();
        if (auto* old = std::exchange(m_vs, nullptr)) old->noteUnbound();
        if (auto* old = std::exchange(m_ps, nullptr)) old->noteUnbound();
        if (auto* old = std::exchange(m_gs, nullptr)) old->noteUnbound();
        if (auto* old = std::exchange(m_layout, nullptr)) old->noteUnbound();
        if (auto* old = std::exchange(m_blend, nullptr)) old->noteUnbound();
        if (auto* old = std::exchange(m_depth, nullptr)) old->noteUnbound();
        if (auto* old = std::exchange(m_raster, nullptr)) old->noteUnbound();
      }

      DirectDevice*        m_device;
      Rc<IBackendContext>  m_owned;
      IBackendContext*     m_ctx;
      bool                 m_omRebinds;
      bool                 m_pipelineDirty = true;
      IBackendPipeline*    m_currentPipeline = nullptr;
      PipelineKey          m_key;
      PipelineTable        m_pipelines;

      UINT                 m_writeCount = 0;
      ViewRange            m_writes[kTargets + 1];
      UINT                 m_targetCount = 0;
      DirectRtv*           m_rtvs[kTargets] = {};
      DirectDsv*           m_dsv = nullptr;

      DirectVertexShader*  m_vs = nullptr;
      DirectPixelShader*   m_ps = nullptr;
      DirectGeometryShader* m_gs = nullptr;
      DirectInputLayout*   m_layout = nullptr;
      DirectBlendState*    m_blend = nullptr;
      DirectDepthState*    m_depth = nullptr;
      DirectRasterState*   m_raster = nullptr;
      float                m_blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
      UINT                 m_stencilRef = 0;
      D3D11_PRIMITIVE_TOPOLOGY m_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

      Stage                m_stages[3];

      DirectBuffer*        m_vertexBuffers[kStreams] = {};
      UINT                 m_vertexStrides[kStreams] = {};
      UINT                 m_vertexOffsets[kStreams] = {};
      DirectBuffer*        m_indexBuffer = nullptr;
      DXGI_FORMAT          m_indexFormat = DXGI_FORMAT_UNKNOWN;
      UINT                 m_indexOffset = 0;

      UINT                 m_viewportCount = 0;
      UINT                 m_scissorCount = 0;
      D3D11_VIEWPORT       m_viewports[kViewports] = {};
      D3D11_RECT           m_scissors[kViewports] = {};
    };

    class DirectDevice final : public ID3D11Device {
    public:
      DirectDevice() : m_backendDevice(BackendType::D3D12, nullptr) { }

      void createContext() {
        // With the Direct3D 9 worker thread that thread records too: no
        // second worker behind it. Not "externally synchronized": without
        // the CS thread's retiring, the context keeps what its lists use.
        Rc<IBackendContext> context = UserOverrides::get().d3d9WorkerThread
          ? m_backendDevice.createImmediateContext() : m_backendDevice.createThreadedImmediateContext();
        if (!context)
          throw std::runtime_error("no immediate context");
        m_context = new DirectContext(this, std::move(context));
      }

      ~DirectDevice() {
        // Its destructor drops the bindings; the backend is still alive for them.
        delete m_context;
        m_context = nullptr;
      }

      d3d12::D3D12BackendDevice* backend() const { return static_cast<d3d12::D3D12BackendDevice*>(m_backendDevice.getBackend()); }
      DirectContext* context() const { return m_context; }
      D3D11BackendDevice& backendDevice() { return m_backendDevice; }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object)
          return E_POINTER;
        *object = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11Device)) {
          AddRef();
          *object = static_cast<ID3D11Device*>(this);
          return S_OK;
        }
        return E_NOINTERFACE;
      }

      ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.fetch_add(1, std::memory_order_relaxed) + 1; }

      ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!refs && !processExiting())
          delete this;
        return refs;
      }

      // ---- Resources ----

      HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* initial,
                                             ID3D11Buffer** buffer) override {
        if (buffer)
          *buffer = nullptr;
        if (!desc)
          return E_INVALIDARG;
        if (HRESULT hr = D3D11Device::validateBufferDesc(*desc, initial && initial->pSysMem); FAILED(hr))
          return hr;
        if (!buffer)
          return S_FALSE;
        return guarded([&] {
          auto backend = m_backendDevice.createBuffer(desc, initial);
          if (!backend)
            return HRESULT(E_FAIL);
          *buffer = new DirectBuffer(this, *desc, std::move(backend));
          return HRESULT(S_OK);
        });
      }

      HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* initial,
                                                ID3D11Texture2D** texture) override {
        if (texture)
          *texture = nullptr;
        if (!desc)
          return E_INVALIDARG;
        if (HRESULT hr = validateTextureDesc(D3D11_RESOURCE_DIMENSION_TEXTURE2D, desc->Width, desc->Height, 1, desc->MipLevels,
              desc->ArraySize, desc->Format, desc->SampleDesc, desc->Usage, desc->BindFlags, desc->CPUAccessFlags, desc->MiscFlags,
              initial != nullptr); FAILED(hr))
          return hr;
        if (!texture)
          return S_FALSE;
        return guarded([&] {
          auto image = m_backendDevice.createTexture2D(desc, initial);
          if (!image)
            return HRESULT(E_FAIL);
          D3D11_TEXTURE2D_DESC effective = *desc;
          if (!effective.MipLevels)
            effective.MipLevels = computeFullMipLevelCount(effective.Width, effective.Height, 1u);
          *texture = new DirectTexture2D(this, effective, std::move(image));
          return HRESULT(S_OK);
        });
      }

      HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC* desc, const D3D11_SUBRESOURCE_DATA* initial,
                                                ID3D11Texture3D** texture) override {
        if (texture)
          *texture = nullptr;
        if (!desc)
          return E_INVALIDARG;
        if (HRESULT hr = validateTextureDesc(D3D11_RESOURCE_DIMENSION_TEXTURE3D, desc->Width, desc->Height, desc->Depth, desc->MipLevels,
              1, desc->Format, DXGI_SAMPLE_DESC{ 1, 0 }, desc->Usage, desc->BindFlags, desc->CPUAccessFlags, desc->MiscFlags,
              initial != nullptr); FAILED(hr))
          return hr;
        if (!texture)
          return S_FALSE;
        return guarded([&] {
          auto image = m_backendDevice.createTexture3D(desc, initial);
          if (!image)
            return HRESULT(E_FAIL);
          D3D11_TEXTURE3D_DESC effective = *desc;
          if (!effective.MipLevels)
            effective.MipLevels = computeFullMipLevelCount(effective.Width, effective.Height, effective.Depth);
          *texture = new DirectTexture3D(this, effective, std::move(image));
          return HRESULT(S_OK);
        });
      }

      // ---- Views ----

      HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                                         ID3D11ShaderResourceView** view) override {
        return createMappedView(resource, desc, 0, view);
      }

      HRESULT createMappedView(ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, UINT mapping,
                               ID3D11ShaderResourceView** view) {
        if (view)
          *view = nullptr;
        if (!resource)
          return E_INVALIDARG;
        D3D11_SHADER_RESOURCE_VIEW_DESC resolved = {};
        if (HRESULT hr = resolveSrvDesc(resource, desc, formatQuery(), resolved); FAILED(hr))
          return hr;
        if (!view)
          return S_FALSE;
        IBackendImage* image = imageOf(resource);
        if (!image)
          return E_INVALIDARG;
        const D3D11_SHADER_RESOURCE_VIEW_DESC backendDesc = getBackendSrvDesc(resource, resolved);
        const ImageDesc& imageDesc = image->getDesc();
        ImageViewDesc viewDesc = getDefaultImageViewDesc(imageDesc, ImageViewUsage::ShaderResource);
        if (!initImageViewDescFromSrv(backendDesc, imageDesc, viewDesc))
          return E_INVALIDARG;
        viewDesc.componentMapping = mapping;
        return guarded([&] {
          auto backend = m_backendDevice.getBackend()->createImageView(image, viewDesc);
          if (!backend)
            return HRESULT(E_FAIL);
          ViewRange range = rangeOf(resource, image, viewDesc);
          range.planes = resolved.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || resolved.Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT ? 2u : 1u;
          *view = new DirectSrv(this, resource, std::move(backend), range, resolved);
          return HRESULT(S_OK);
        });
      }

      HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource* resource, const D3D11_RENDER_TARGET_VIEW_DESC* desc,
                                                       ID3D11RenderTargetView** view) override {
        if (view)
          *view = nullptr;
        if (!resource)
          return E_INVALIDARG;
        D3D11_RENDER_TARGET_VIEW_DESC resolved = {};
        if (HRESULT hr = resolveRtvDesc(resource, desc, formatQuery(), resolved); FAILED(hr))
          return hr;
        if (!view)
          return S_FALSE;
        IBackendImage* image = imageOf(resource);
        if (!image)
          return E_INVALIDARG;
        const ImageDesc& imageDesc = image->getDesc();
        ImageViewDesc viewDesc = getDefaultImageViewDesc(imageDesc, ImageViewUsage::RenderTarget);
        if (!initImageViewDescFromRtv(resolved, imageDesc, viewDesc))
          return E_INVALIDARG;
        return guarded([&] {
          auto backend = m_backendDevice.getBackend()->createImageView(image, viewDesc);
          if (!backend)
            return HRESULT(E_FAIL);
          const DXGI_FORMAT format = validateImageViewFormat(resolved.Format, imageDesc.format,
            (imageDesc.miscFlags & kResourceMiscCastable) != 0);
          *view = new DirectRtv(this, resource, std::move(backend), rangeOf(resource, image, viewDesc), resolved, format,
            imageDesc.sampleCount ? imageDesc.sampleCount : 1u, imageDesc.sampleQuality);
          return HRESULT(S_OK);
        });
      }

      HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource* resource, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc,
                                                       ID3D11DepthStencilView** view) override {
        if (view)
          *view = nullptr;
        if (!resource)
          return E_INVALIDARG;
        D3D11_DEPTH_STENCIL_VIEW_DESC resolved = {};
        if (HRESULT hr = resolveDsvDesc(resource, desc, formatQuery(), resolved); FAILED(hr))
          return hr;
        if (!view)
          return S_FALSE;
        IBackendImage* image = imageOf(resource);
        if (!image)
          return E_INVALIDARG;
        const ImageDesc& imageDesc = image->getDesc();
        ImageViewDesc viewDesc = getDefaultImageViewDesc(imageDesc, ImageViewUsage::DepthStencil);
        if (!initImageViewDescFromDsv(resolved, imageDesc, viewDesc))
          return E_INVALIDARG;
        return guarded([&] {
          auto backend = m_backendDevice.getBackend()->createImageView(image, viewDesc);
          if (!backend)
            return HRESULT(E_FAIL);
          // Planes it writes; a read-only one is no write.
          ViewRange range = rangeOf(resource, image, viewDesc);
          range.planes = 0;
          if (!(resolved.Flags & D3D11_DSV_READ_ONLY_DEPTH))
            range.planes |= 1u;
          const bool stencil = resolved.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || resolved.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
          if (stencil && !(resolved.Flags & D3D11_DSV_READ_ONLY_STENCIL))
            range.planes |= 2u;
          const DXGI_FORMAT format = resolveDepthStencilFormat(validateImageViewFormat(resolved.Format, imageDesc.format));
          *view = new DirectDsv(this, resource, std::move(backend), range, resolved, format,
            imageDesc.sampleCount ? imageDesc.sampleCount : 1u, imageDesc.sampleQuality);
          return HRESULT(S_OK);
        });
      }

      // ---- Shaders and states ----

      HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* elements, UINT count, const void* bytecode, SIZE_T size,
                                                  ID3D11InputLayout** layout) override {
        if (layout)
          *layout = nullptr;
        if (!elements)
          return E_INVALIDARG;
        if (HRESULT hr = validateInputLayout(elements, count, bytecode, size, formatQuery()); FAILED(hr))
          return hr;
        if (!layout)
          return S_FALSE;
        *layout = new DirectInputLayout(this, elements, count);
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage*, ID3D11VertexShader** shader) override {
        return createShader<DirectVertexShader>(bytecode, size, DxbcProgramType::Vertex, ShaderStage::Vertex, shader);
      }
      HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage*, ID3D11PixelShader** shader) override {
        return createShader<DirectPixelShader>(bytecode, size, DxbcProgramType::Pixel, ShaderStage::Pixel, shader);
      }
      HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void* bytecode, SIZE_T size, ID3D11ClassLinkage*, ID3D11GeometryShader** shader) override {
        return createShader<DirectGeometryShader>(bytecode, size, DxbcProgramType::Geometry, ShaderStage::Geometry, shader);
      }

      HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC* desc, ID3D11BlendState** state) override {
        if (state)
          *state = nullptr;
        if (!desc)
          return E_INVALIDARG;
        if (HRESULT hr = D3D11Device::validateBlendDesc(*desc); FAILED(hr))
          return hr;
        if (!state)
          return S_FALSE;
        *state = new DirectBlendState(this, *desc);
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* desc, ID3D11DepthStencilState** state) override {
        if (state)
          *state = nullptr;
        if (!desc)
          return E_INVALIDARG;
        if (HRESULT hr = D3D11Device::validateDepthStencilDesc(*desc); FAILED(hr))
          return hr;
        if (!state)
          return S_FALSE;
        *state = new DirectDepthState(this, *desc);
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC* desc, ID3D11RasterizerState** state) override {
        if (state)
          *state = nullptr;
        if (!desc)
          return E_INVALIDARG;
        if (HRESULT hr = D3D11Device::validateRasterizerDesc(*desc); FAILED(hr))
          return hr;
        if (!state)
          return S_FALSE;
        *state = new DirectRasterState(this, *desc);
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** sampler) override {
        return createSampler(desc, sampler, false);
      }

      // pointMipAnisotropic: also D3D12's anisotropic filter with point mips.
      HRESULT createSampler(const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** sampler, bool pointMipAnisotropic) {
        if (sampler)
          *sampler = nullptr;
        if (!desc)
          return E_INVALIDARG;
        if (HRESULT hr = D3D11Device::validateSamplerDesc(*desc, pointMipAnisotropic); FAILED(hr))
          return hr;
        if (!sampler)
          return S_FALSE;
        return guarded([&] {
          auto backend = m_backendDevice.createSampler(desc);
          if (!backend)
            return HRESULT(E_FAIL);
          *sampler = new DirectSampler(this, *desc, std::move(backend));
          return HRESULT(S_OK);
        });
      }

      HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC* desc, ID3D11Query** query) override {
        if (query)
          *query = nullptr;
        if (!desc)
          return E_INVALIDARG;
        QueryDesc backendDesc;
        if (!D3D11Device::translateQueryDesc(*desc, backendDesc) || (desc->MiscFlags & ~UINT(D3D11_QUERY_MISC_PREDICATEHINT)))
          return E_INVALIDARG;
        if (!query)
          return S_FALSE;
        return guarded([&] {
          auto backend = m_backendDevice.getBackend()->createQuery(backendDesc);
          if (!backend)
            return HRESULT(E_FAIL);
          *query = new DirectQuery(this, *desc, std::move(backend));
          return HRESULT(S_OK);
        });
      }

      // ---- Queries of the device ----

      HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT format, UINT* support) override {
        if (!support)
          return E_INVALIDARG;
        *support = formatSupport(format);
        return *support ? S_OK : E_FAIL;
      }

      HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT format, UINT samples, UINT* levels) override {
        if (!levels)
          return E_INVALIDARG;
        *levels = m_backendDevice.getBackend()->checkMultisampleQualityLevels(format, samples);
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
        HRESULT hr = backend()->getDevice()->GetDeviceRemovedReason();
        // Work DXUP could not submit counts too (IBackendDevice::lostReason).
        if (SUCCEEDED(hr)) {
          const HRESULT lost = backend()->lostReason();
          if (FAILED(lost))
            hr = lost;
        }
        return hr;
      }

      void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** context) override {
        AddRef();
        *context = m_context;
      }

      D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override { return D3D_FEATURE_LEVEL_11_0; }
      UINT STDMETHODCALLTYPE GetCreationFlags() override { return D3D11_CREATE_DEVICE_BGRA_SUPPORT; }

      HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT* size, void*) override {
        if (size)
          *size = 0;
        return DXGI_ERROR_NOT_FOUND;
      }
      HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
      HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
      HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT) override { return S_OK; }
      UINT STDMETHODCALLTYPE GetExceptionMode() override { return 0; }

#define DXUP_UNUSED(name) unusedCall(#name); return E_NOTIMPL
      HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**) override { DXUP_UNUSED(CreateTexture1D); }
      HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**) override { DXUP_UNUSED(CreateUnorderedAccessView); }
      HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void*, SIZE_T, const D3D11_SO_DECLARATION_ENTRY*, UINT, const UINT*, UINT, UINT, ID3D11ClassLinkage*, ID3D11GeometryShader**) override { DXUP_UNUSED(CreateGeometryShaderWithStreamOutput); }
      HRESULT STDMETHODCALLTYPE CreateHullShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11HullShader**) override { DXUP_UNUSED(CreateHullShader); }
      HRESULT STDMETHODCALLTYPE CreateDomainShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DomainShader**) override { DXUP_UNUSED(CreateDomainShader); }
      HRESULT STDMETHODCALLTYPE CreateComputeShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**) override { DXUP_UNUSED(CreateComputeShader); }
      HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage**) override { DXUP_UNUSED(CreateClassLinkage); }
      HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC*, ID3D11Predicate**) override { DXUP_UNUSED(CreatePredicate); }
      HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC*, ID3D11Counter**) override { DXUP_UNUSED(CreateCounter); }
      HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT, ID3D11DeviceContext**) override { DXUP_UNUSED(CreateDeferredContext); }
      HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE, REFIID, void**) override { DXUP_UNUSED(OpenSharedResource); }
      void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO*) override { unusedCall("CheckCounterInfo"); }
      HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC*, D3D11_COUNTER_TYPE*, UINT*, LPSTR, UINT*, LPSTR, UINT*, LPSTR, UINT*) override { DXUP_UNUSED(CheckCounter); }
      HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE, void*, UINT) override { DXUP_UNUSED(CheckFeatureSupport); }
#undef DXUP_UNUSED

    private:
      template<typename Fn>
      static HRESULT guarded(Fn&& fn) {
        try {
          return fn();
        } catch (const std::exception& e) {
          logMessage(LogLevel::Error, "d3d9.backend = d3d12: creation failed: %s", e.what());
        } catch (...) {
          logMessage(LogLevel::Error, "d3d9.backend = d3d12: creation failed");
        }
        return E_FAIL;
      }

      template<typename Object, typename Interface>
      HRESULT createShader(const void* bytecode, SIZE_T size, DxbcProgramType type, ShaderStage stage, Interface** shader) {
        if (shader)
          *shader = nullptr;
        if (!bytecode || !size)
          return E_INVALIDARG;
        if (HRESULT hr = validateShaderBytecode(bytecode, size, { type }); FAILED(hr))
          return hr;
        if (!shader)
          return S_FALSE;
        return guarded([&] {
          ShaderDesc desc;
          desc.stage = stage;
          desc.bytecode = bytecode;
          desc.bytecodeSize = size;
          auto backend = m_backendDevice.getBackend()->createShader(desc);
          if (!backend)
            return HRESULT(E_FAIL);
          *shader = new Object(this, std::move(backend));
          return HRESULT(S_OK);
        });
      }

      FormatSupportQuery formatQuery() {
        return [this](DXGI_FORMAT format) { return formatSupport(format); };
      }

      UINT formatSupport(DXGI_FORMAT format) {
        const UINT index = UINT(format);
        if (index < m_formatSupport.size()) {
          const uint64_t cached = m_formatSupport[index].load(std::memory_order_relaxed);
          if (cached >> 32)
            return UINT(cached);
        }
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { format };
        UINT result = 0;
        if (SUCCEEDED(backend()->getDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) {
          result = support.Support1;
          // Bits D3D12 does not report, as in DXUP's D3D11.
          if (result)
            result |= D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;
          const UINT autogen = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_MIP;
          if ((result & autogen) == autogen)
            result |= D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
        }
        if (index < m_formatSupport.size())
          m_formatSupport[index].store((uint64_t(1) << 32) | result, std::memory_order_relaxed);
        return result;
      }

      // The checks D3D11 applies to texture descriptions the legacy core can
      // get wrong (formats it tries, sample counts).
      HRESULT validateTextureDesc(D3D11_RESOURCE_DIMENSION dimension, UINT width, UINT height, UINT depth, UINT mipLevels, UINT arraySize,
                                  DXGI_FORMAT format, DXGI_SAMPLE_DESC sampleDesc, D3D11_USAGE usage, UINT bindFlags, UINT cpuAccessFlags,
                                  UINT miscFlags, bool initialData) {
        const bool is3D = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
        const UINT maxExtent = is3D ? D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION : D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        if (format == DXGI_FORMAT_UNKNOWN || !width || !height || !depth || !arraySize
         || width > maxExtent || height > maxExtent || depth > maxExtent
         || arraySize > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION
         || mipLevels > computeFullMipLevelCount(width, height, depth))
          return E_INVALIDARG;
        if ((bindFlags & D3D11_BIND_RENDER_TARGET) && (bindFlags & D3D11_BIND_DEPTH_STENCIL))
          return E_INVALIDARG;
        const UINT support = formatSupport(format);
        if (!support || !(support & (is3D ? D3D11_FORMAT_SUPPORT_TEXTURE3D : D3D11_FORMAT_SUPPORT_TEXTURE2D)))
          return E_INVALIDARG;
        if (!isTypelessFormat(format)) {
          if (((bindFlags & D3D11_BIND_RENDER_TARGET) && !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
           || ((bindFlags & D3D11_BIND_DEPTH_STENCIL) && !(support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL))
           || ((bindFlags & D3D11_BIND_SHADER_RESOURCE) && !(support & D3D11_FORMAT_SUPPORT_SHADER_LOAD)))
            return E_INVALIDARG;
        }
        if (isBlockCompressedFormat(format) && ((width & 3) || (height & 3)))
          return E_INVALIDARG;
        if ((miscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS)
         && (bindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) != (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
          return E_INVALIDARG;
        if ((miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) && (is3D || width != height || arraySize < 6))
          return E_INVALIDARG;
        if (!sampleDesc.Count)
          return E_INVALIDARG;
        if (sampleDesc.Count > 1) {
          UINT levels = 0;
          if (initialData || is3D || mipLevels != 1 || FAILED(CheckMultisampleQualityLevels(format, sampleDesc.Count, &levels)) || !levels
           || (sampleDesc.Quality >= levels && sampleDesc.Quality != D3D11_STANDARD_MULTISAMPLE_PATTERN
            && sampleDesc.Quality != D3D11_CENTER_MULTISAMPLE_PATTERN))
            return E_INVALIDARG;
        }
        const UINT gpuWrites = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
        switch (usage) {
          case D3D11_USAGE_DEFAULT:
            return cpuAccessFlags ? E_INVALIDARG : S_OK;
          case D3D11_USAGE_IMMUTABLE:
            return initialData && !cpuAccessFlags && !(bindFlags & gpuWrites) ? S_OK : E_INVALIDARG;
          case D3D11_USAGE_DYNAMIC:
            return cpuAccessFlags == D3D11_CPU_ACCESS_WRITE && mipLevels == 1 && arraySize == 1
                && bindFlags && !(bindFlags & gpuWrites) && !(miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
              ? S_OK : E_INVALIDARG;
          case D3D11_USAGE_STAGING:
            return !bindFlags && cpuAccessFlags && sampleDesc.Count == 1 ? S_OK : E_INVALIDARG;
          default:
            return E_INVALIDARG;
        }
      }

      std::atomic<ULONG>  m_refs{1};
      D3D11BackendDevice  m_backendDevice;
      DirectContext*      m_context = nullptr;
      std::array<std::atomic<uint64_t>, 256> m_formatSupport = {};
    };

    void retainDevice(DirectDevice* device) { device->AddRef(); }
    void releaseDevice(DirectDevice* device) { device->Release(); }

    template<typename Base>
    void STDMETHODCALLTYPE Child<Base>::GetDevice(ID3D11Device** device) {
      m_device->AddRef();
      *device = m_device;
    }

    ULONG STDMETHODCALLTYPE DirectContext::AddRef() { return m_device->AddRef(); }
    ULONG STDMETHODCALLTYPE DirectContext::Release() { return m_device->Release(); }

    void STDMETHODCALLTYPE DirectContext::GetDevice(ID3D11Device** device) {
      m_device->AddRef();
      *device = m_device;
    }

    IBackendPipeline* DirectContext::createPipeline() {
      if (!m_vs)
        return nullptr;
      GraphicsPipelineDesc desc = {};
      static const uint64_t kBlendHash = state_hash::blend(BlendState{});
      static const uint64_t kRasterHash = state_hash::rasterizer(RasterizerState{});
      static const uint64_t kDepthHash = state_hash::depthStencil(DepthStencilState{});
      desc.blendStateHash = kBlendHash;
      desc.rasterizerStateHash = kRasterHash;
      desc.depthStencilStateHash = kDepthHash;

      desc.vsBytecode = m_vs->raw->getBytecode();
      desc.vsBytecodeSize = m_vs->raw->getBytecodeSize();
      desc.vsBytecodeHash = m_vs->raw->getBytecodeHash();
      if (m_ps) {
        desc.psBytecode = m_ps->raw->getBytecode();
        desc.psBytecodeSize = m_ps->raw->getBytecodeSize();
        desc.psBytecodeHash = m_ps->raw->getBytecodeHash();
      }
      if (m_gs) {
        desc.gsBytecode = m_gs->raw->getBytecode();
        desc.gsBytecodeSize = m_gs->raw->getBytecodeSize();
        desc.gsBytecodeHash = m_gs->raw->getBytecodeHash();
      }
      if (m_layout) {
        desc.inputLayout.elements = m_layout->elements().data();
        desc.inputLayout.numElements = uint32_t(m_layout->elements().size());
        desc.inputLayoutHash = m_layout->hash;
      }
      if (m_blend) {
        desc.blendState = m_blend->state;
        desc.blendStateHash = m_blend->hash;
      }
      if (m_raster) {
        desc.rasterizerState = m_raster->state;
        desc.rasterizerStateHash = m_raster->hash;
      }
      if (m_depth) {
        desc.depthStencilState = m_depth->state;
        desc.depthStencilStateHash = m_depth->hash;
      }
      desc.sampleMask = m_key.sampleMask;
      desc.topology = convertTopology(m_topology);
      desc.stripCutIndexType = (m_key.topology >> 8) & 1u ? IndexType::UInt32 : IndexType::UInt16;
      desc.numRenderTargets = m_targetCount;
      for (UINT i = 0; i < m_targetCount; i++)
        if (m_rtvs[i])
          desc.rtvFormats[i] = m_rtvs[i]->pipelineFormat;
      if (m_dsv) {
        desc.dsvFormat = m_dsv->pipelineFormat;
      } else {
        desc.depthStencilState.depthEnable = false;
        desc.depthStencilState.depthWriteEnable = false;
        desc.depthStencilState.stencilEnable = false;
      }
      desc.sampleCount = m_key.samples & 0xffffu;
      desc.sampleQuality = m_key.sampleQuality;

      IBackendDevice* backend = m_device->backendDevice().getBackend();
      // With asyncShaders a pipeline that is not compiled yet compiles in the
      // background and its draws are skipped meanwhile.
      Rc<IBackendPipeline> pipeline;
      bool pending = false;
      if (backend->asyncPipelines())
        pipeline = backend->requestGraphicsPipeline(desc, pending);
      else
        pipeline = backend->createGraphicsPipelineDeferred(desc);
      // The device keeps every pipeline.
      return pipeline.get();
    }

    void DirectContext::rebindRenamedBuffer(DirectBuffer* buffer) {
      IBackendBuffer* raw = buffer->raw;
      for (Stage& stage : m_stages)
        for (UINT slot = 0; slot < kConstantSlots; slot++) {
          if (stage.constants[slot] != buffer)
            continue;
          if (stage.constantFirst[slot] || stage.constantCount[slot])
            m_ctx->bindConstantBufferRange(stage.stage, slot, raw, stage.constantFirst[slot], stage.constantCount[slot]);
          else
            m_ctx->bindConstantBuffer(stage.stage, slot, raw);
        }
      for (UINT slot = 0; slot < kStreams; slot++)
        if (m_vertexBuffers[slot] == buffer)
          m_ctx->bindVertexBuffer(slot, raw, m_vertexOffsets[slot], m_vertexStrides[slot]);
      if (m_indexBuffer == buffer)
        m_ctx->bindIndexBuffer(raw, m_indexOffset, m_indexFormat == DXGI_FORMAT_R32_UINT ? IndexType::UInt32 : IndexType::UInt16);
    }

    HRESULT DirectContext::mapBuffer(DirectBuffer* buffer, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
      const MapMode mode = convertMapType(type);
      const D3D11_BUFFER_DESC& desc = buffer->desc();
      bool renamed = false;
      void* discarded = nullptr;
      // WRITE_DISCARD gives the buffer new memory: in place when the backend
      // can, else a new buffer.
      if (desc.Usage == D3D11_USAGE_DYNAMIC && mode == MapMode::WriteDiscard) {
        discarded = m_ctx->discardBuffer(buffer->raw);
        if (discarded) {
          // The backend binds constant buffers again at their new memory.
          if (desc.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER))
            rebindRenamedBuffer(buffer);
          renamed = true;
        } else if (auto fresh = m_device->backendDevice().createBuffer(&desc, nullptr)) {
          if (auto old = buffer->replace(std::move(fresh), m_ctx))
            m_ctx->retainTemporaryBuffer(std::move(old));
          rebindRenamedBuffer(buffer);
          renamed = true;
        }
      }
      if (desc.Usage == D3D11_USAGE_STAGING) {
        if (!m_ctx->waitForGpuUse(buffer->raw->gpuUse, (flags & D3D11_MAP_FLAG_DO_NOT_WAIT) != 0))
          return DXGI_ERROR_WAS_STILL_DRAWING;
      } else if (mode == MapMode::Read || mode == MapMode::ReadWrite) {
        m_ctx->synchronize();
      }
      void* data = discarded ? discarded : m_ctx->mapBuffer(buffer->raw, renamed ? MapMode::Write : mode);
      if (!data)
        return (flags & D3D11_MAP_FLAG_DO_NOT_WAIT) ? DXGI_ERROR_WAS_STILL_DRAWING : E_FAIL;
      mapped->pData = data;
      mapped->RowPitch = desc.ByteWidth;
      mapped->DepthPitch = 0;
      return S_OK;
    }

    void STDMETHODCALLTYPE DirectContext::UpdateSubresource(ID3D11Resource* resource, UINT subresource, const D3D11_BOX* box, const void* data,
                                                            UINT rowPitch, UINT depthPitch) {
      if (!resource || !data)
        return;
      if (DirectBuffer* buffer = bufferOf(resource)) {
        const UINT size = buffer->desc().ByteWidth;
        // Empty boxes and ranges past the buffer do nothing, as in D3D11.
        if (box && (box->right <= box->left || box->bottom <= box->top || box->back <= box->front || box->right > size))
          return;
        m_ctx->updateBuffer(buffer->raw, box ? box->left : 0, box ? box->right - box->left : size, data);
        return;
      }
      IBackendImage* image = imageOf(resource);
      if (!image)
        return;
      const ImageDesc& desc = image->getDesc();
      const uint32_t mip = subresource % desc.mipLevels;
      const uint32_t layer = subresource / desc.mipLevels;
      if (layer >= (desc.type == ImageType::Tex3D ? 1u : (std::max)(desc.arraySize, 1u)))
        return;
      uint32_t width = (std::max)(desc.width >> mip, 1u);
      uint32_t height = (std::max)(desc.height >> mip, 1u);
      uint32_t depth = (std::max)(desc.depth >> mip, 1u);
      if (box) {
        // Block formats address whole blocks: a mip smaller than a block takes one.
        const uint32_t block = getFormatBlockSize(desc.format);
        if (box->right <= box->left || box->bottom <= box->top || box->back <= box->front
         || box->right > (width + block - 1) / block * block || box->bottom > (height + block - 1) / block * block || box->back > depth)
          return;
        width = box->right - box->left;
        height = box->bottom - box->top;
        depth = box->back - box->front;
      }
      IBackendDevice* backend = m_device->backendDevice().getBackend();
      const UploadLayout layout = getImageUploadLayout(BackendType::D3D12, backend, image, subresource,
        computeRowPitch(width, desc.format), computeNumRows(height, desc.format), depth, box != nullptr);
      const uint64_t slicePitch = uint64_t(layout.rowPitch) * layout.numRows;
      if (layout.numRows <= 1 && rowPitch < layout.copyRowBytes)
        rowPitch = layout.copyRowBytes;
      const uint64_t srcDepthPitch = depthPitch && layout.depth > 1 ? depthPitch : uint64_t(rowPitch) * layout.numRows;

      BufferDesc uploadDesc = {};
      uploadDesc.size = uploadSizeClass(layout.totalSize);
      uploadDesc.usage = ResourceUsage::Dynamic;
      uploadDesc.cpuAccess = CpuAccess::Write;
      auto upload = backend->createBuffer(uploadDesc);
      if (!upload)
        return;
      void* mapped = m_ctx->mapBuffer(upload.get(), MapMode::Write);
      if (!mapped)
        return;
      for (uint32_t z = 0; z < layout.depth; z++)
        copyUploadRows(static_cast<uint8_t*>(mapped) + z * slicePitch, layout.rowPitch, static_cast<const uint8_t*>(data) + z * srcDepthPitch,
          rowPitch, layout.copyRowBytes, layout.numRows, desc.format);
      m_ctx->unmapBuffer(upload.get());
      ImageSubresource target = {};
      target.mipLevel = mip;
      target.arrayLayer = layer;
      if (box) {
        ImageCopyRegion region = {};
        region.x = box->left;
        region.y = box->top;
        region.z = box->front;
        region.width = width;
        region.height = height;
        region.depth = depth;
        m_ctx->copyBufferToImage(image, target, upload.get(), 0, &region);
      } else {
        m_ctx->copyBufferToImage(image, target, upload.get(), 0);
      }
      m_ctx->retainTemporaryBuffer(std::move(upload));
    }

    void STDMETHODCALLTYPE DirectContext::CopyResource(ID3D11Resource* dst, ID3D11Resource* src) {
      if (!dst || !src || dst == src)
        return;
      DirectBuffer* dstBuffer = bufferOf(dst);
      DirectBuffer* srcBuffer = bufferOf(src);
      if (dstBuffer || srcBuffer) {
        // D3D11 copies only buffers of the same size and drops the call otherwise.
        if (dstBuffer && srcBuffer && dstBuffer->desc().ByteWidth == srcBuffer->desc().ByteWidth)
          m_ctx->copyBuffer(dstBuffer->raw, 0, srcBuffer->raw, 0, srcBuffer->desc().ByteWidth);
        return;
      }
      IBackendImage* dstImage = imageOf(dst);
      IBackendImage* srcImage = imageOf(src);
      if (!dstImage || !srcImage)
        return;
      const ImageDesc& d = dstImage->getDesc();
      const ImageDesc& s = srcImage->getDesc();
      // D3D11 copies only textures of the same layout in compatible formats;
      // D3D12 would reject the command list.
      if (d.width != s.width || d.height != s.height || d.depth != s.depth || d.mipLevels != s.mipLevels || d.arraySize != s.arraySize
       || d.sampleCount != s.sampleCount || getFormatBlockSize(d.format) != getFormatBlockSize(s.format)
       || !copyableFormats(d.format, s.format))
        return;
      const uint32_t mips = (std::max)(s.mipLevels, 1u);
      const uint32_t layers = s.type == ImageType::Tex3D ? 1u : (std::max)(s.arraySize, 1u);
      for (uint32_t layer = 0; layer < layers; layer++)
        for (uint32_t mip = 0; mip < mips; mip++) {
          ImageSubresource sub = {};
          sub.mipLevel = mip;
          sub.arrayLayer = layer;
          m_ctx->copyImage(dstImage, sub, srcImage, sub);
        }
    }

    void STDMETHODCALLTYPE DirectContext::CopySubresourceRegion(ID3D11Resource* dst, UINT dstSubresource, UINT dstX, UINT dstY, UINT dstZ,
                                                                ID3D11Resource* src, UINT srcSubresource, const D3D11_BOX* srcBox) {
      if (!dst || !src)
        return;
      DirectBuffer* dstBuffer = bufferOf(dst);
      DirectBuffer* srcBuffer = bufferOf(src);
      if (dstBuffer && srcBuffer) {
        // Ranges outside either buffer are cut off, as D3D11 does.
        const uint64_t srcSize = srcBuffer->desc().ByteWidth;
        const uint64_t dstSize = dstBuffer->desc().ByteWidth;
        const uint64_t srcOffset = srcBox ? srcBox->left : 0;
        const uint64_t srcEnd = srcBox ? (std::min)(uint64_t(srcBox->right), srcSize) : srcSize;
        if (srcOffset >= srcEnd || dstX >= dstSize || (srcBox && (srcBox->bottom <= srcBox->top || srcBox->back <= srcBox->front)))
          return;
        const uint64_t size = (std::min)(srcEnd - srcOffset, dstSize - dstX);
        if (dstBuffer != srcBuffer) {
          m_ctx->copyBuffer(dstBuffer->raw, dstX, srcBuffer->raw, srcOffset, size);
        } else if (dstX != srcOffset) {
          BufferDesc temporaryDesc = {};
          temporaryDesc.size = size;
          temporaryDesc.usage = ResourceUsage::Default;
          temporaryDesc.bindFlags = BindFlags::UnorderedAccess;
          if (auto temporary = m_device->backendDevice().getBackend()->createBuffer(temporaryDesc)) {
            m_ctx->copyBuffer(temporary.get(), 0, srcBuffer->raw, srcOffset, size);
            m_ctx->copyBuffer(dstBuffer->raw, dstX, temporary.get(), 0, size);
            m_ctx->retainTemporaryBuffer(std::move(temporary));
          }
        }
        return;
      }
      IBackendImage* dstImage = imageOf(dst);
      IBackendImage* srcImage = imageOf(src);
      if (!dstImage || !srcImage)
        return;
      const ImageDesc& dstDesc = dstImage->getDesc();
      const ImageDesc& srcDesc = srcImage->getDesc();
      ImageSubresource dstSub = {};
      const uint32_t dstMips = (std::max)(dstDesc.mipLevels, 1u);
      dstSub.mipLevel = dstSubresource % dstMips;
      dstSub.arrayLayer = dstSubresource / dstMips;
      ImageSubresource srcSub = {};
      const uint32_t srcMips = (std::max)(srcDesc.mipLevels, 1u);
      srcSub.mipLevel = srcSubresource % srcMips;
      srcSub.arrayLayer = srcSubresource / srcMips;
      if (dstSub.arrayLayer >= (dstDesc.type == ImageType::Tex3D ? 1u : (std::max)(dstDesc.arraySize, 1u))
       || srcSub.arrayLayer >= (srcDesc.type == ImageType::Tex3D ? 1u : (std::max)(srcDesc.arraySize, 1u)))
        return;

      ImageCopyRegion region = {};
      ImageCopyRegion* regionPtr = nullptr;
      if (srcBox) {
        if (srcBox->right <= srcBox->left || srcBox->bottom <= srcBox->top || srcBox->back <= srcBox->front)
          return;
        region.x = srcBox->left;
        region.y = srcBox->top;
        region.z = srcBox->front;
        region.width = srcBox->right - srcBox->left;
        region.height = srcBox->bottom - srcBox->top;
        region.depth = srcBox->back - srcBox->front;
        regionPtr = &region;
      }
      if (!copyableFormats(dstDesc.format, srcDesc.format))
        return;
      auto extent = [](uint32_t size, uint32_t mip) { return (std::max)(size >> mip, 1u); };
      // Regions outside either subresource are cut off, as D3D11 does: in
      // elements (a block of a compressed format for a texel of another).
      if (formatElementBytes(dstDesc.format) == formatElementBytes(srcDesc.format)) {
        auto elements = [](uint32_t texels, uint32_t block) { return (texels + block - 1) / block; };
        const uint32_t srcBlock = getFormatBlockSize(srcDesc.format);
        const uint32_t dstBlock = getFormatBlockSize(dstDesc.format);
        const uint32_t srcW = extent(srcDesc.width, srcSub.mipLevel);
        const uint32_t srcH = extent(srcDesc.height, srcSub.mipLevel);
        const uint32_t srcD = srcDesc.type == ImageType::Tex3D ? extent(srcDesc.depth, srcSub.mipLevel) : 1u;
        const uint32_t dstW = extent(dstDesc.width, dstSub.mipLevel);
        const uint32_t dstH = extent(dstDesc.height, dstSub.mipLevel);
        const uint32_t dstD = dstDesc.type == ImageType::Tex3D ? extent(dstDesc.depth, dstSub.mipLevel) : 1u;
        if (!regionPtr) {
          region = { 0, 0, 0, srcW, srcH, srcD };
          regionPtr = &region;
        }
        const uint32_t srcX = region.x / srcBlock, srcY = region.y / srcBlock;
        const uint32_t dstEX = dstX / dstBlock, dstEY = dstY / dstBlock;
        if (srcX >= elements(srcW, srcBlock) || srcY >= elements(srcH, srcBlock) || region.z >= srcD
         || dstEX >= elements(dstW, dstBlock) || dstEY >= elements(dstH, dstBlock) || dstZ >= dstD)
          return;
        const uint32_t width = (std::min)({ elements(region.x + region.width, srcBlock) - srcX,
          elements(srcW, srcBlock) - srcX, elements(dstW, dstBlock) - dstEX });
        const uint32_t height = (std::min)({ elements(region.y + region.height, srcBlock) - srcY,
          elements(srcH, srcBlock) - srcY, elements(dstH, dstBlock) - dstEY });
        region.width = (std::min)(width * srcBlock, srcW - region.x);
        region.height = (std::min)(height * srcBlock, srcH - region.y);
        region.depth = (std::min)({ region.depth, srcD - region.z, dstD - dstZ });
        // Whole-subresource copies keep the fast path.
        if (!srcBox && region.width == srcW && region.height == srcH && region.depth == srcD
         && !dstX && !dstY && !dstZ && srcBlock == dstBlock)
          regionPtr = nullptr;
      }

      // A subresource copied onto itself goes through a temporary image:
      // D3D12 cannot have it in the copy source and destination states at
      // once, and D3D11 copies overlapping regions as if through one.
      const bool same = dstImage == srcImage && dstSub.mipLevel == srcSub.mipLevel && dstSub.arrayLayer == srcSub.arrayLayer;
      if (same && (!regionPtr || (region.x == dstX && region.y == dstY && region.z == dstZ)))
        return;
      if (same || (dstImage == srcImage && srcImage->isCpuAccessible())) {
        if (srcDesc.sampleCount > 1)
          return;
        if (!regionPtr)
          region = { 0, 0, 0, extent(srcDesc.width, srcSub.mipLevel), extent(srcDesc.height, srcSub.mipLevel),
            srcDesc.type == ImageType::Tex3D ? extent(srcDesc.depth, srcSub.mipLevel) : 1u };
        const uint32_t block = getFormatBlockSize(srcDesc.format);
        ImageDesc temporaryDesc = {};
        temporaryDesc.type = srcDesc.type;
        temporaryDesc.width = (region.width + block - 1) / block * block;
        temporaryDesc.height = (region.height + block - 1) / block * block;
        temporaryDesc.depth = srcDesc.type == ImageType::Tex3D ? region.depth : 1u;
        temporaryDesc.format = srcDesc.format;
        temporaryDesc.usage = ResourceUsage::Default;
        switch (srcDesc.format) {
          case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_D32_FLOAT:
          case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R32G8X24_TYPELESS:
            temporaryDesc.bindFlags = BindFlags::DepthStencil;
            break;
          default:
            break;
        }
        auto temporary = m_device->backendDevice().getBackend()->createImage(temporaryDesc);
        if (!temporary)
          return;
        const ImageSubresource temporarySub = {};
        const ImageCopyRegion temporaryRegion = { 0, 0, 0, region.width, region.height, region.depth };
        m_ctx->copyImageRegion(temporary.get(), temporarySub, 0, 0, 0, srcImage, srcSub, &region);
        m_ctx->copyImageRegion(dstImage, dstSub, dstX, dstY, dstZ, temporary.get(), temporarySub, &temporaryRegion);
        // It lives until the GPU has run the copies.
        std::vector<std::shared_ptr<void>> retired;
        retired.push_back(std::move(temporary));
        m_ctx->retireAfterGpu(retired);
        return;
      }
      m_ctx->copyImageRegion(dstImage, dstSub, dstX, dstY, dstZ, srcImage, srcSub, regionPtr);
    }

    void STDMETHODCALLTYPE DirectContext::ResolveSubresource(ID3D11Resource* dst, UINT dstSubresource, ID3D11Resource* src, UINT srcSubresource,
                                                             DXGI_FORMAT format) {
      if (!dst || !src)
        return;
      IBackendImage* dstImage = imageOf(dst);
      IBackendImage* srcImage = imageOf(src);
      if (!dstImage || !srcImage)
        return;
      // D3D11 resolves a multisampled subresource into a single-sampled one
      // of its size in a format both can take, and drops other calls. A
      // single-sampled source is copied, as the drivers do.
      const ImageDesc& dstDesc = dstImage->getDesc();
      const ImageDesc& srcDesc = srcImage->getDesc();
      const uint32_t dstMips = (std::max)(dstDesc.mipLevels, 1u);
      const uint32_t srcMips = (std::max)(srcDesc.mipLevels, 1u);
      const uint32_t dstMip = dstSubresource % dstMips, srcMip = srcSubresource % srcMips;
      auto extent = [](uint32_t size, uint32_t mip) { return (std::max)(size >> mip, 1u); };
      auto viewable = [&](DXGI_FORMAT resourceFormat) { return format == resourceFormat || getFormatFamily(format) == resourceFormat; };
      if (dstDesc.sampleCount != 1
       || dstSubresource / dstMips >= (std::max)(dstDesc.arraySize, 1u)
       || srcSubresource / srcMips >= (std::max)(srcDesc.arraySize, 1u)
       || extent(dstDesc.width, dstMip) != extent(srcDesc.width, srcMip)
       || extent(dstDesc.height, dstMip) != extent(srcDesc.height, srcMip)
       || isFamilyOnlyFormat(format) || !viewable(dstDesc.format) || !viewable(srcDesc.format))
        return;
      if (srcDesc.sampleCount <= 1) {
        if (dst == src)
          return;
        ImageSubresource dstSub = {};
        dstSub.mipLevel = dstMip;
        dstSub.arrayLayer = dstSubresource / dstMips;
        ImageSubresource srcSub = {};
        srcSub.mipLevel = srcMip;
        srcSub.arrayLayer = srcSubresource / srcMips;
        m_ctx->copyImage(dstImage, dstSub, srcImage, srcSub);
        return;
      }
      m_ctx->resolveSubresource(dstImage, dstSubresource, srcImage, srcSubresource, format);
    }

    // Borderless full screen: the window's client area covers its monitor.
    bool windowCoversMonitor(HWND window) {
      if (!window || !IsWindowVisible(window) || IsIconic(window))
        return false;
      RECT client = {};
      if (!GetClientRect(window, &client))
        return false;
      MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
      MONITORINFO monitor = { sizeof(monitor) };
      HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
      return handle && GetMonitorInfoW(handle, &monitor)
        && client.left <= monitor.rcMonitor.left && client.top <= monitor.rcMonitor.top
        && client.right >= monitor.rcMonitor.right && client.bottom >= monitor.rcMonitor.bottom;
    }

    class DirectChain final : public DirectSwapChain {
    public:
      DirectChain(DirectDevice* device, ComPtr<IDXGISwapChain3> real, HWND window, UINT flags)
      : m_device(device), m_real(std::move(real)), m_window(window), m_flags(flags),
        m_tearing((flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0) {
        m_device->AddRef();
        ComPtr<IDXGISwapChain2> swapChain2;
        if ((flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) && SUCCEEDED(m_real.As(&swapChain2)))
          m_waitable = swapChain2->GetFrameLatencyWaitableObject();
        noteSize();
      }

      ~DirectChain() override {
        IBackendContext* context = m_device->context()->backend();
        context->waitForQueue();
        releaseBuffer();
        if (m_waitable)
          CloseHandle(m_waitable);
        m_real.Reset();
        m_device->Release();
      }

      HRESULT getBuffer(ID3D11Texture2D** texture) override {
        *texture = nullptr;
        if (!m_texture && !createBuffer())
          return E_FAIL;
        m_texture->AddRef();
        *texture = m_texture.Get();
        return S_OK;
      }

      HRESULT resize(UINT width, UINT height) override {
        // Presents queued on the worker still use the buffers.
        m_device->context()->backend()->waitForQueue();
        releaseBuffer();
        const HRESULT hr = m_real->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, m_flags);
        if (SUCCEEDED(hr))
          noteSize();
        return hr;
      }

      HRESULT present(UINT syncInterval) override {
        HitchScope hitch("present", syncInterval);
        DirectContext* directContext = m_device->context();
        IBackendContext* context = directContext->backend();
        // A device DXUP lost without the system knowing: the application
        // recreates it as after a GPU fault.
        if (FAILED(m_device->backend()->lostReason()))
          return DXGI_ERROR_DEVICE_REMOVED;
        const UserOverrides& overrides = UserOverrides::get();
        if (overrides.syncInterval >= 0)
          syncInterval = UINT(overrides.syncInterval);
        const UINT flags = !syncInterval && m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        if (!(++m_presents & 127))
          noteWindow();
        if (!m_texture && !createBuffer())
          return E_FAIL;
        if (m_saveFrames)
          saveFrames();
        // The worker copies, submits and presents the frame while the
        // application goes on with the next.
        context->executeOnBackend([this, syncInterval, flags](IBackendContext* backend) {
          waitForBackBuffer(backend);
          retarget(backend);
          backend->presentImage(m_image);
          backend->waitForQueue();
          const int64_t start = hitchTraceEnabled() ? hitchTimestamp() : 0;
          const HRESULT hr = m_real->Present(syncInterval, flags);
          if (FAILED(hr))
            logMessage(LogLevel::Error, "d3d9.backend = d3d12: Present failed: hr=0x%08x", unsigned(hr));
          if (start)
            notePresentTime(start);
          m_lastPresentFailed.store(FAILED(hr), std::memory_order_release);
          retarget(backend);
          m_lastResult.store(hr, std::memory_order_release);
        });
        context->endFrame();
        // Like a native Present, block while too many frames are queued.
        const UINT latency = overrides.lowLatency ? 1u : overrides.maxFrameLatency > 0 ? UINT(overrides.maxFrameLatency) : 3u;
        context->waitForFrameLatency(latency);
        return m_lastResult.load(std::memory_order_acquire);
      }

    private:
      bool createBuffer() {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        if (FAILED(m_real->GetDesc1(&desc)) || desc.BufferCount < 2)
          return false;
        IBackendContext* context = m_device->context()->backend();
        m_buffers.clear();
        for (UINT i = 0; i < desc.BufferCount; i++) {
          ComPtr<ID3D12Resource> buffer;
          if (FAILED(m_real->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(buffer.GetAddressOf()))) || !buffer) {
            m_buffers.clear();
            return false;
          }
          ComPtr<IUnknown> held = buffer.Get();
          context->executeOnBackend([held](IBackendContext* backend) { backend->trackSwapChainBuffer(held.Get()); });
          m_buffers.push_back(std::move(buffer));
        }
        const D3D12_RESOURCE_DESC real = m_buffers[0]->GetDesc();
        D3D11_TEXTURE2D_DESC texture = {};
        texture.Width = UINT(real.Width);
        texture.Height = real.Height;
        texture.MipLevels = 1;
        texture.ArraySize = 1;
        texture.Format = real.Format;
        texture.SampleDesc = { 1, 0 };
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (!(real.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
          texture.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        ImageDesc imageDesc = {};
        imageDesc.type = ImageType::Tex2D;
        imageDesc.width = texture.Width;
        imageDesc.height = texture.Height;
        imageDesc.format = texture.Format;
        imageDesc.bindFlags = (texture.BindFlags & D3D11_BIND_SHADER_RESOURCE)
          ? BindFlags::RenderTarget | BindFlags::ShaderResource : BindFlags::RenderTarget;
        // Swap chain buffers take sRGB views of their UNORM format.
        imageDesc.miscFlags = kResourceMiscCastable;
        const UINT current = m_real->GetCurrentBackBufferIndex() % desc.BufferCount;
        auto image = std::make_shared<d3d12::D3D12BackendImage>(m_buffers[current], imageDesc);
        image->setCurrentState(D3D12_RESOURCE_STATE_PRESENT);
        image->setRetargetable();
        m_image = image.get();
        m_texture.Attach(new DirectTexture2D(m_device, texture, std::move(image)));
        return true;
      }

      // ResizeBuffers fails while the GPU or a backend image still references
      // a buffer.
      void releaseBuffer() {
        IBackendContext* context = m_device->context()->backend();
        if (m_image) {
          IBackendImage* image = m_image;
          context->executeOnBackend([image](IBackendContext* backend) { backend->retargetImage(image, nullptr); });
          context->waitForWorker();
        }
        m_image = nullptr;
        m_buffers.clear();
        context->synchronize();
        m_texture.Reset();
        context->synchronize();
      }

      // After Present: the buffer is the real buffer Present shows next.
      void retarget(IBackendContext* backend) {
        const UINT index = m_real->GetCurrentBackBufferIndex();
        if (m_image && index < m_buffers.size())
          backend->retargetImage(m_image, m_buffers[index].Get());
      }

      // Before the worker writes the next back buffer. What the frame
      // recorded so far goes to the GPU first, so that it does not run dry
      // while the worker waits. A present that failed queued no frame, which
      // would give back what its wait took: the next present does not wait.
      void waitForBackBuffer(IBackendContext* backend) {
        if (m_lastPresentFailed.exchange(false, std::memory_order_acq_rel))
          return;
        if (!m_waitable || WaitForSingleObjectEx(m_waitable, 0, TRUE) != WAIT_TIMEOUT)
          return;
        backend->flush();
        if (WaitForSingleObjectEx(m_waitable, 1000, TRUE) == WAIT_TIMEOUT)
          logMessage(LogLevel::Warn, "d3d9.backend = d3d12: no back buffer after 1 s");
      }

      void noteSize() {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        ComPtr<IDXGISwapChain2> swapChain2;
        if (m_waitable && SUCCEEDED(m_real->GetDesc1(&desc)) && SUCCEEDED(m_real.As(&swapChain2)))
          swapChain2->SetMaximumFrameLatency((std::max)(desc.BufferCount, 2u) - 1);
        noteWindow();
      }

      // Windows replaces a window whose thread has handled no input for 5 s
      // with a "not responding" ghost: as in DXUP's DXGI, not for a borderless
      // full screen window.
      void noteWindow() {
        static std::atomic<bool> off{false};
        if (!off.load(std::memory_order_relaxed) && windowCoversMonitor(m_window) && !off.exchange(true))
          DisableProcessWindowsGhosting();
      }

      // dxup_frame_dump.txt: presents 60, 300 and 301, then one every 15
      // seconds, of which the last four stay, as with DXUP's DXGI.
      void saveFrames() {
        const uint32_t frame = ++m_savedPresents;
        const uint64_t now = GetTickCount64();
        if (frame == 60 || frame == 300 || frame == 301) {
          saveFrame(frame);
          m_lastSave = now;
        } else if (frame > 301 && now - m_lastSave >= 15000) {
          saveFrame(frame);
          m_lastSave = now;
          m_periodicSaves.push_back(frame);
          if (m_periodicSaves.size() > 4) {
            char old[64];
            sprintf_s(old, "dxup_frame_%u.bmp", m_periodicSaves.front());
            dxupDeleteSibling(old);
            m_periodicSaves.erase(m_periodicSaves.begin());
          }
        }
      }

      void saveFrame(uint32_t frame) {
        D3D11_TEXTURE2D_DESC desc = {};
        m_texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())))
          return;
        DirectContext* context = m_device->context();
        context->CopyResource(staging.Get(), m_texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
          return;
        char name[64];
        sprintf_s(name, "dxup_frame_%u.bmp", frame);
        if (FILE* file = dxupOpenSibling(name, "wb")) {
          const uint32_t imageSize = desc.Width * desc.Height * 4;
          BITMAPFILEHEADER fileHeader = {};
          BITMAPINFOHEADER infoHeader = {};
          fileHeader.bfType = 0x4D42;
          fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
          fileHeader.bfSize = fileHeader.bfOffBits + imageSize;
          infoHeader.biSize = sizeof(infoHeader);
          infoHeader.biWidth = LONG(desc.Width);
          infoHeader.biHeight = -LONG(desc.Height);
          infoHeader.biPlanes = 1;
          infoHeader.biBitCount = 32;
          infoHeader.biSizeImage = imageSize;
          fwrite(&fileHeader, sizeof(fileHeader), 1, file);
          fwrite(&infoHeader, sizeof(infoHeader), 1, file);
          // The swap chain's buffers are BGRA, as a bitmap's rows.
          for (UINT y = 0; y < desc.Height; y++)
            fwrite(static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(desc.Width) * 4, 1, file);
          fclose(file);
          logMessage(LogLevel::Warn, "frame dump: wrote %s", name);
        }
        context->Unmap(staging.Get(), 0);
      }

      // Hitch trace: how long the real Present takes, every 1000 presents.
      void notePresentTime(int64_t start) {
        static const LARGE_INTEGER frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        const double ms = double(hitchTimestamp() - start) * 1000.0 / double(frequency.QuadPart);
        m_presentMs += ms;
        m_presentMax = (std::max)(m_presentMax, ms);
        if (++m_presentCount < 1000)
          return;
        logMessage(LogLevel::Warn, "present stats: %u presents, %.3f ms average in Present, max %.2f ms",
          m_presentCount, m_presentMs / m_presentCount, m_presentMax);
        m_presentCount = 0;
        m_presentMs = 0.0;
        m_presentMax = 0.0;
      }

      DirectDevice*           m_device;
      ComPtr<IDXGISwapChain3> m_real;
      HWND                    m_window;
      UINT                    m_flags;
      bool                    m_tearing;
      HANDLE                  m_waitable = nullptr;
      std::vector<ComPtr<ID3D12Resource>> m_buffers;
      ComPtr<DirectTexture2D> m_texture;
      IBackendImage*          m_image = nullptr;
      std::atomic<HRESULT>    m_lastResult{S_OK};
      std::atomic<bool>       m_lastPresentFailed{false};
      uint32_t                m_presents = 0;
      const bool              m_saveFrames = dxupSiblingExists("dxup_frame_dump.txt");
      uint32_t                m_savedPresents = 0;
      uint64_t                m_lastSave = 0;
      std::vector<uint32_t>   m_periodicSaves;
      uint32_t                m_presentCount = 0;
      double                  m_presentMs = 0.0, m_presentMax = 0.0;
    };

    DirectDevice* directDevice(IUnknown* device) {
      return static_cast<DirectDevice*>(static_cast<ID3D11Device*>(device));
    }

  }

  namespace {
    std::atomic<LegacyBackend> g_legacyBackend{LegacyBackend::D3D11};
  }

  void setLegacyBackend(LegacyBackend backend) {
    g_legacyBackend.store(backend, std::memory_order_relaxed);
  }

  LegacyBackend legacyBackend() {
    char value[16] = {};
    const DWORD length = GetEnvironmentVariableA("DXUP_D3D9_BACKEND", value, sizeof(value));
    if (length && length < sizeof(value)) {
      if (!_stricmp(value, "d3d12"))
        return LegacyBackend::D3D12;
      if (!_stricmp(value, "d3d12only"))
        return LegacyBackend::D3D12Only;
      if (!_stricmp(value, "d3d11"))
        return LegacyBackend::D3D11;
    }
    return g_legacyBackend.load(std::memory_order_relaxed);
  }

  const char* directBackendObstacle() {
    const UserOverrides& overrides = UserOverrides::get();
    if (overrides.msaaSamples > 1 || overrides.msaaForce || overrides.sgssaa != SgssaaMode::Off)
      return "forced multisampling (d3d11.msaaSamples, msaaForce, sgssaa)";
    if (overrides.upscaler || overrides.upscalerAuto || overrides.upscalerInject)
      return "the temporal upscalers (d3d11.upscaler)";
    if (overrides.spatialUpscale || overrides.renderScale < 1.0f)
      return "scaling at Present (dxgi.upscale, dxgi.renderScale)";
    if (dxupSiblingExists("dxup_show_fps_overlay.txt"))
      return "the frame rate overlay (dxup_show_fps_overlay.txt)";
    return nullptr;
  }

  HRESULT createDirectDevice(ID3D11Device** device, ID3D11DeviceContext** context, uint32_t* vendorId) {
    *device = nullptr;
    *context = nullptr;
    try {
      auto* created = new DirectDevice();
      try {
        created->createContext();
      } catch (...) {
        created->Release();
        throw;
      }
      *vendorId = created->backend()->getVendorId();
      created->GetImmediateContext(context);
      *device = created;
      return S_OK;
    } catch (const std::exception& e) {
      logMessage(LogLevel::Error, "d3d9.backend = d3d12: no device: %s", e.what());
    } catch (...) {
      logMessage(LogLevel::Error, "d3d9.backend = d3d12: no device");
    }
    return E_FAIL;
  }

  HRESULT WINAPI directCreateMappedView(IUnknown* device, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                        UINT mapping, ID3D11ShaderResourceView** view) {
    return directDevice(device)->createMappedView(resource, desc, mapping, view);
  }

  HRESULT WINAPI directCreateSampler(IUnknown* device, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** sampler) {
    return directDevice(device)->createSampler(desc, sampler, true);
  }

  HRESULT WINAPI directWriteBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, UINT offset, const void* data, UINT size) {
    if (!buffer)
      return S_OK;
    static_cast<DirectContext*>(context)->writeBuffer(static_cast<DirectBuffer*>(buffer), offset, data, size);
    return S_OK;
  }

  HRESULT WINAPI directWriteBufferWith(ID3D11DeviceContext* context, ID3D11Buffer* buffer, UINT offset, UINT size,
                                       void (*fn)(void*, uint8_t*, size_t, const void*, size_t), void* user,
                                       void (*pack)(void*, uint8_t*), void* packUser, UINT dataSize) {
    static_cast<DirectContext*>(context)->writeBufferWith(static_cast<DirectBuffer*>(buffer), offset, size, fn, user, pack, packUser, dataSize);
    return S_OK;
  }

  HRESULT createDirectSwapChain(ID3D11Device* device, HWND window, UINT width, UINT height,
                                std::unique_ptr<DirectSwapChain>& swapChain) {
    DirectDevice* direct = directDevice(device);
    auto* backend = direct->backend();
    IDXGIFactory4* factory = backend->getDxgiFactory();
    auto* queue = static_cast<ID3D12CommandQueue*>(backend->getNativeCommandQueue());
    if (!factory || !queue)
      return E_FAIL;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // Presents without VSync (Direct3D 9's IMMEDIATE) tear rather than wait
    // for the display: flip model swap chains otherwise run at its refresh
    // rate. DXUP waits for buffers itself (waitForBackBuffer).
    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))
     && SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing)
      desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (!dxupSiblingExists("dxup_blocking_present.txt"))
      desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> created;
    HRESULT hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, created.GetAddressOf());
    if (FAILED(hr))
      return hr;
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    ComPtr<IDXGISwapChain3> real;
    hr = created.As(&real);
    if (FAILED(hr))
      return hr;
    swapChain = std::make_unique<DirectChain>(direct, std::move(real), window, desc.Flags);
    return S_OK;
  }

}
