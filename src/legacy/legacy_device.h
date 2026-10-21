#pragma once

// The legacy core: what Direct3D 7 to 9 have in common (surfaces that lock,
// the fixed-function pipeline, render states, flexible vertex formats, draws
// from application memory, triangle fans), on Direct3D 11 - DXUP's own when
// its d3d11.dll is next to the front end's DLL, so the work ends in D3D12.

#include "legacy_types.h"
#include <atomic>
#include "d3d9_shader.h"
#include "../util/util_types.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxup::legacy {

  // The device's self-test draws (legacy_selftest.cpp): not recorded as the
  // game's shader variants.
  extern std::atomic<bool> g_selfTesting;

  // D3DCompile of d3dcompiler_47.dll (loaded once), null without it.
  using CompileFunction = HRESULT (WINAPI*)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*,
    const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
  CompileFunction compiler();

  class Device;

  template<typename T>
  class Ref {
  public:
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() { reset(); }
    void reset() { if (m_ptr) { m_ptr->Release(); m_ptr = nullptr; } }
    T* get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    T** put() { reset(); return &m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }
  private:
    T* m_ptr = nullptr;
  };

  struct TextureDesc {
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t depth  = 1;            // volume textures: slices
    uint32_t levels = 1;
    Format   format = FormatUnknown;
    bool     cube = false;
    bool     volume = false;
    bool     renderTarget = false;
    bool     depthStencil = false;
    bool     systemMemory = false;  // application memory only, no GPU copy
    bool     sampledDepth = false;  // depth textures shaders read
    bool     srgb = false;          // sRGB views as well as linear ones
    bool     autoMips = false;      // mips generated from level 0
    uint32_t samples = 1;           // multisampled targets
    bool     backBuffer = false;    // a swap chain's buffer (forced multisampling covers its size)
    bool     colorKeyed = false;    // DirectDraw textures: alpha for colour-keyed texels
  };

  struct Box { uint32_t left, top, front, right, bottom, back; };

  // A texture or surface: a D3D11 texture, and for lockable ones the data of
  // each subresource in the application's format.
  class Texture {
  public:
    Texture(Device* device, const TextureDesc& desc);
    ~Texture();

    bool valid() const { return m_valid; }
    const TextureDesc& desc() const { return m_desc; }
    uint32_t subresourceCount() const { return m_desc.levels * (m_desc.cube ? 6u : 1u); }
    uint32_t width(uint32_t subresource) const;
    uint32_t height(uint32_t subresource) const;
    uint32_t depth(uint32_t subresource) const;
    uint32_t pitch(uint32_t subresource) const;
    uint32_t slicePitch(uint32_t subresource) const;

    // Application memory of a subresource (rect or box: or null for all).
    // Discarding locks need not see what the GPU drew.
    bool lock(uint32_t subresource, const Rect* rect, bool readOnly, void** data, uint32_t* pitch, bool discard = false);
    bool lockBox(uint32_t subresource, const Box* box, bool readOnly, void** data, uint32_t* rowPitch, uint32_t* slicePitch,
                 bool discard = false);
    void unlock(uint32_t subresource);
    // Uploads changed subresources (unlocked since the last draw), and
    // generates mips the texture generates when mips (it is sampled, not
    // drawn into).
    void flush(bool mips = true);

    void setPalette(const uint32_t* entries);
    // Texels of the key color become transparent (color-keyed textures).
    void setColorKey(bool enabled, uint32_t key);

    ID3D11Texture2D*          resource() const { return m_texture.get(); }
    ID3D11Resource*           d3dResource() const;
    ID3D11ShaderResourceView* srv() const { return m_srv.get(); }
    ID3D11ShaderResourceView* srv(bool srgb);
    bool ownsView(ID3D11ShaderResourceView* view) const { return view == m_srv.get() || view == m_srvSrgb.get(); }
    // A view of a copy of the texture as it is now: shaders reading a
    // texture they also draw into (null when no copy can be made).
    ID3D11ShaderResourceView* copySrv(bool srgb);
    ID3D11RenderTargetView*   rtv(uint32_t subresource, bool srgb = false);
    ID3D11DepthStencilView*   dsv() const { return m_dsv.get(); }
    DXGI_FORMAT dxgiFormat() const { return m_dxgiFormat; }
    bool nullTarget() const { return m_desc.format == FormatNULL; }
    // Depth textures sampled with a comparison.
    bool shadow() const { return m_shadow; }
    // The shader's texture type: 0 none, 1 2D, 2 cube, 3 volume.
    uint32_t type() const { return !m_srv ? 0u : m_desc.volume ? 3u : m_desc.cube ? 2u : 1u; }

    // GPU contents changed (rendered to): locks read them back first.
    void markGpuWritten();
    // The application's copy of a subresource (empty when never locked).
    const std::vector<uint8_t>& levelData(uint32_t subresource) const { return m_levels[subresource].data; }
    // Application writes or mipmaps wait for the next draw.
    bool pending() const { return m_anyDirty || m_mipsDirty; }

  private:
    struct Level {
      std::vector<uint8_t> data;   // application format
      bool dirty = false;
      bool locked = false;
      bool lockReadOnly = false;
    };
    void upload(uint32_t subresource);
    void readBack(uint32_t subresource);
    void markPending();
    // CPU-visible copy of a mip level's size, kept: render targets locked
    // every frame (video frames written into the back buffer) neither
    // create one per lock nor pass whole frames through UpdateSubresource.
    ID3D11Texture2D* staging(uint32_t subresource);
    void write(uint32_t subresource, const D3D11_BOX& box, const void* data, uint32_t pitch, uint32_t depthPitch);
    size_t levelSize(uint32_t subresource) const;

    Device*                m_device;
    TextureDesc            m_desc;
    bool                   m_valid = false;
    DXGI_FORMAT            m_dxgiFormat = DXGI_FORMAT_UNKNOWN;   // views
    DXGI_FORMAT            m_srgbFormat = DXGI_FORMAT_UNKNOWN;
    bool                   m_convert = false;
    bool                   m_shadow = false;
    Ref<ID3D11Texture2D>   m_texture;
    Ref<ID3D11Texture3D>   m_volume;
    Ref<ID3D11Texture2D>   m_resolved;      // multisampled targets read back
    std::unique_ptr<Texture> m_copy;        // copySrv
    bool                   m_copyFailed = false;
    Ref<ID3D11ShaderResourceView> m_srv;
    Ref<ID3D11ShaderResourceView> m_srvSrgb;
    Ref<ID3D11DepthStencilView>   m_dsv;
    std::vector<ID3D11RenderTargetView*> m_rtvs;
    std::vector<ID3D11RenderTargetView*> m_rtvsSrgb;
    std::vector<ID3D11Texture2D*> m_staging;
    std::vector<Level>     m_levels;
    std::array<uint32_t, 256> m_palette = {};
    bool                   m_colorKeyEnabled = false;
    uint32_t               m_colorKey = 0;
    bool                   m_gpuWritten = false;
    bool                   m_anyDirty = false;
    bool                   m_mipsDirty = false;
  };

  // A vertex or index buffer, kept in application memory. A draw uploads
  // the part it reads, if written since: buffers locked whole and filled
  // piece by piece (no-overwrite appends) upload each piece once.
  class Buffer {
  public:
    // Dynamic buffers (rewritten between draws) stream the range each draw
    // reads through the device's rings instead of updating GPU memory.
    Buffer(Device* device, uint32_t size, bool index, bool dynamic = false);
    bool dynamic() const { return m_dynamic; }
    // A draw streamed the buffer: buffers drawn often between writes go
    // back to GPU memory (unless created dynamic).
    void noteStreamedDraw() {
      if (!m_declaredDynamic && ++m_drawsSinceWrite > 64) {
        m_dynamic = false;
        m_settled = true;
      }
    }
    ~Buffer();
    bool valid() const { return m_buffer.get() != nullptr; }
    uint32_t size() const { return uint32_t(m_data.size()); }
    uint8_t* data() { return m_data.data(); }
    // The application may have written [offset, offset + size) (size 0: all).
    void invalidate(uint32_t offset = 0, uint32_t size = 0);
    // Uploads what of [offset, offset + size) changed since its last upload.
    void prepare(uint32_t offset, uint32_t size);
    // All of it uploaded: draws need not know which part they read.
    bool uploaded() const {
      return m_clean.size() == 1 && m_clean[0].first == 0 && m_clean[0].second >= m_data.size();
    }
    ID3D11Buffer* resource() const { return m_buffer.get(); }
    // The smallest and largest of `count` indices from index `start`, kept
    // until the buffer is written again; false past the buffer's end.
    bool indexRange(uint32_t start, uint32_t count, bool index32, uint32_t& lo, uint32_t& hi);
  private:
    struct IndexRange { uint32_t lo, hi; };
    std::unordered_map<uint64_t, IndexRange> m_indexRanges;   // of m_indexRangesVersion
    uint32_t           m_indexRangesVersion = 0;
    uint32_t           m_version = 0;   // counts writes
    Device*            m_device;
    std::vector<uint8_t> m_data;
    Ref<ID3D11Buffer>  m_buffer;
    bool               m_dynamic = false;
    bool               m_declaredDynamic = false;
    bool               m_settled = false;     // static for good
    uint32_t           m_writes = 0;
    uint32_t           m_drawsSinceWrite = 0;
    std::vector<std::pair<uint32_t, uint32_t>> m_clean;  // sorted, disjoint [begin, end)
    std::vector<std::pair<uint32_t, uint32_t>> m_scratch;
  };

  // A vertex declaration (Direct3D 9) or flexible vertex format.
  class VertexDecl {
  public:
    VertexDecl(const VertexElement* elements, uint32_t count, uint32_t fvf = 0);
    const std::vector<VertexElement>& elements() const { return m_elements; }
    uint64_t hash() const { return m_hash; }
    uint32_t streamMask() const { return m_streamMask; }
    // The flexible vertex format it was made from, or 0.
    uint32_t fvf() const { return m_fvf; }
    // What the fixed function sees of it, as a flexible vertex format:
    // pre-transformed positions, blend weights, colors, coordinate sizes.
    uint32_t ffpFvf() const { return m_ffpFvf; }
    // Elements of a flexible vertex format.
    static std::vector<VertexElement> elementsOf(uint32_t fvf);
  private:
    std::vector<VertexElement> m_elements;
    uint64_t m_hash = 0;
    uint32_t m_streamMask = 0;
    uint32_t m_fvf = 0;
    uint32_t m_ffpFvf = 0;
  };

  // A Direct3D 8/9 shader, translated to shader model 5 per variant: vertex
  // shaders by varying count and integer inputs, pixel shaders by the
  // texture types of shader model 1 and the samplers comparing depth.
  class Shader : public std::enable_shared_from_this<Shader> {
  public:
    struct Variant {
      Ref<ID3D11VertexShader> vs;
      Ref<ID3D11PixelShader>  ps;
      std::vector<uint8_t>    bytecode;   // vertex shaders: for input layouts
      ShaderInfo9             info;
      uint32_t                id = 0;
      uint8_t                 clipPlanes = 0;   // vertex: clip distances written
    };
    static std::shared_ptr<Shader> create(Device* device, const uint32_t* tokens, size_t tokenCount, std::string& error);
    // A shader of the tokens (count of them, hash as create forms it), no
    // variant compiled yet.
    static std::shared_ptr<Shader> make(ID3D11Device* d3d11, const uint32_t* tokens, size_t count, uint64_t hash,
                                        std::string& error);
    const ShaderInfo9& info() const { return m_info; }
    const std::vector<uint32_t>& tokens() const { return m_tokens; }
    bool vertex() const { return m_info.stage == ShaderStage9::Vertex; }
    uint64_t hash() const { return m_hash; }
    // Null when the variant does not compile. Waits for a compile of the
    // same variant on a worker. Variants of draws are recorded and compiled
    // ahead in later runs.
    const Variant* variant(const TranslateOptions9& options, bool draw = true);
    // Compiles a variant on a worker thread (loading screens create many).
    void prepare(const TranslateOptions9& options, bool first = false);
  private:
    friend class Device;
    struct Slot {
      std::once_flag once;
      Variant        variant;
      bool           ok = false;
      bool           drawn = false;   // under m_mutex
    };
    bool compile(const TranslateOptions9& options, uint64_t key, Variant& v);
    bool createVariant(const TranslateOptions9& options, Variant& v);
    Ref<ID3D11Device>       m_d3d11;
    std::vector<uint32_t>   m_tokens;
    ShaderInfo9             m_info;
    uint64_t                m_hash = 0;
    bool                    m_ahead = false;   // compiled ahead (Device::takeWarmShader)
    std::mutex              m_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<Slot>> m_variants;
    // The variant the last draw used (draws only, on the device's thread):
    // most draws use the one before, without the lock and the map.
    uint64_t                m_drawnKey = 0;
    const Variant*          m_drawnVariant = nullptr;
  };

  class Device {
  public:
    static std::unique_ptr<Device> create(HWND window, std::string& error);
    ~Device();

    ID3D11Device*        d3d11() const { return m_device.get(); }
    // On DXUP's D3D11: targets of this size are a swap chain's (dxup.conf
    // d3d11.msaaForce covers them). Before the back buffer is made.
    void noteBackBufferSize(uint32_t width, uint32_t height);
    // A texture view whose components come as the D3D12 mapping says (0 the
    // default; on DXUP's D3D11 only).
    HRESULT createShaderResourceView(ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, UINT mapping,
                                     ID3D11ShaderResourceView** view);
    // A texture has writes for the GPU: draws flush their textures.
    void noteTexturePending() { m_texturesPending = true; }
    // Vertex shaders have drawn with the fixed-function pixel stage, which
    // reads all its varyings: their variants for it are compiled ahead.
    bool fixedPixelsWithShaders() const { return m_fixedPixelsWithShaders; }
    // A shader drawn in earlier runs that was compiled ahead (the game's
    // first creation of it takes it), or null.
    std::shared_ptr<Shader> takeWarmShader(uint64_t hash, const uint32_t* tokens, size_t count);
    static constexpr uint32_t kFixedPixelVaryings = 11;   // fog, two colors, eight coordinates
    ID3D11DeviceContext* context() const { return m_context.get(); }
    // A frame's draws go into the log (dxup_trace9_go.txt).
    bool tracing() const { return m_trace9; }
    // Bytes into a dynamic buffer (whole 64-byte lines), new memory first
    // for WRITE_DISCARD: written by DXUP's worker thread where it has one.
    bool writeBuffer(ID3D11Buffer* buffer, uint32_t offset, const void* data, uint32_t size, bool discard);
    // Whether textures of a format can be created directly (else converted).
    bool supports(DXGI_FORMAT format, UINT usage) const;

    // ---- Presentation ----
    // Shows a surface in the window (scaled to the window's client area).
    // gammaRamp: 3 x 256 entries (red, green, blue) or null.
    HRESULT present(Texture* source, const Rect* sourceRect, HWND window, uint32_t syncInterval,
                    const uint16_t* gammaRamp = nullptr);

    // ---- Render targets ----
    void setRenderTarget(Texture* target, uint32_t subresource) { setRenderTarget(0, target, subresource); }
    void setRenderTarget(uint32_t index, Texture* target, uint32_t subresource);
    // Viewport and scissor of the whole first target.
    void resetViewport();
    void setDepthStencil(Texture* depth);
    Texture* renderTarget(uint32_t index = 0) const { return index < kMaxRenderTargets ? m_renderTargets[index] : nullptr; }
    uint32_t renderTargetSubresource(uint32_t index = 0) const { return index < kMaxRenderTargets ? m_renderTargetSubresources[index] : 0; }
    Texture* depthStencil() const { return m_depthStencil; }

    // ---- States ----
    void     setRenderState(uint32_t state, uint32_t value);
    uint32_t renderState(uint32_t state) const { return state < RsCount ? m_renderStates[state] : 0; }
    void     setTextureStageState(uint32_t stage, uint32_t type, uint32_t value);
    uint32_t textureStageState(uint32_t stage, uint32_t type) const;
    // Samplers 0-15, and 16-19 of vertex shaders.
    void     setSamplerState(uint32_t sampler, uint32_t type, uint32_t value);
    uint32_t samplerState(uint32_t sampler, uint32_t type) const;
    void     setTexture(uint32_t stage, Texture* texture);
    Texture* texture(uint32_t stage) const { return stage < kSamplerCount ? m_textures[stage] : nullptr; }
    void     setTransform(uint32_t state, const float* matrix);
    void     getTransform(uint32_t state, float* matrix) const;
    void     setMaterial(const Material& material);
    const Material& material() const { return m_material; }
    // Light indices up to kMaxLights (Direct3D grows its light array to the
    // index; larger ones fail as it runs out of memory).
    static constexpr uint32_t kMaxLights = 1u << 16;
    // The largest vertex index of the device caps.
    static constexpr uint32_t kMaxVertexIndex = 0xffffff;
    bool     setLight(uint32_t index, const Light& light);
    bool     getLight(uint32_t index, Light* light) const;
    bool     lightEnable(uint32_t index, bool enable);
    bool     lightEnabled(uint32_t index) const;
    void     setViewport(const Viewport& viewport);
    const Viewport& viewport() const { return m_viewport; }
    void     setScissor(const Rect& rect);
    const Rect& scissor() const { return m_scissor; }
    // Software vertex processing (ProcessVertices): the fixed-function
    // transform, and lighting when asked, of count vertices on the CPU into
    // transformed ones (window x, y, z, 1/w). copyData: colors and texture
    // coordinates not computed are copied.
    void     processVertices(const uint8_t* src, uint32_t srcFvf, uint32_t srcStride, uint8_t* dst, uint32_t dstFvf,
                             uint32_t dstStride, uint32_t count, bool light, bool copyData) const;

    // ---- Shaders, vertex streams (Direct3D 8/9) ----
    void setVertexShader(Shader* shader);
    void setPixelShader(Shader* shader);
    Shader* vertexShader() const { return m_vertexShader; }
    Shader* pixelShader() const { return m_pixelShader; }
    void setShaderConstantF(bool pixel, uint32_t start, const float* data, uint32_t count);
    void setShaderConstantI(bool pixel, uint32_t start, const int32_t* data, uint32_t count);
    void setShaderConstantB(bool pixel, uint32_t start, const uint32_t* data, uint32_t count);
    void getShaderConstantF(bool pixel, uint32_t start, float* data, uint32_t count) const;
    void getShaderConstantI(bool pixel, uint32_t start, int32_t* data, uint32_t count) const;
    void getShaderConstantB(bool pixel, uint32_t start, uint32_t* data, uint32_t count) const;
    void setVertexDeclaration(VertexDecl* decl);
    VertexDecl* vertexDeclaration() const { return m_decl; }
    // The declaration of a flexible vertex format, kept by the device.
    VertexDecl* fvfDeclaration(uint32_t fvf);
    void setStreamSource(uint32_t stream, Buffer* buffer, uint32_t offset, uint32_t stride);
    void setStreamFrequency(uint32_t stream, uint32_t value);
    void setIndices(Buffer* buffer, bool index32);
    void setClipPlane(uint32_t index, const float* plane);

    // ---- Drawing ----
    void clear(uint32_t count, const Rect* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
    // Vertices (and 16- or 32-bit indices) from application memory.
    void drawUP(PrimitiveType type, uint32_t fvf, const void* vertices, uint32_t stride, uint32_t vertexCount,
                const void* indices, bool index32, uint32_t indexCount);
    // From buffers; indices relative to baseVertex.
    void draw(PrimitiveType type, uint32_t fvf, Buffer* vertices, uint32_t stride, uint32_t startVertex,
              uint32_t vertexCount, Buffer* indices, bool index32, uint32_t startIndex, uint32_t indexCount,
              int32_t baseVertex);
    // From a vertex buffer with indices from application memory.
    void drawIndexedVB(PrimitiveType type, uint32_t fvf, Buffer* vertices, uint32_t stride, int32_t baseVertex,
                       uint32_t vertexCount, const void* indices, bool index32, uint32_t indexCount);
    // With the declaration and streams set (Direct3D 8/9).
    void drawPrimitive(PrimitiveType type, uint32_t startVertex, uint32_t vertexCount);
    void drawIndexedPrimitive(PrimitiveType type, int32_t baseVertex, uint32_t minIndex, uint32_t numVertices,
                              uint32_t startIndex, uint32_t indexCount);
    // Stream 0 from application memory (and indices).
    void drawPrimitiveUP(PrimitiveType type, uint32_t vertexCount, const void* vertices, uint32_t stride);
    void drawIndexedPrimitiveUP(PrimitiveType type, uint32_t minIndex, uint32_t numVertices, uint32_t indexCount,
                                const void* indices, bool index32, const void* vertices, uint32_t stride);
    // Copies a rectangle between surfaces, scaling with point or linear
    // filtering, leaving out texels of the source color key.
    void blit(Texture* dst, uint32_t dstSubresource, const Rect* dstRect, Texture* src, uint32_t srcSubresource,
              const Rect* srcRect, bool linear, bool colorKey, uint32_t keyColor);
    // Fills a rectangle of a surface with a color in its own format.
    void fill(Texture* dst, uint32_t dstSubresource, const Rect* dstRect, uint32_t color);
    void fillTexel(Texture* dst, uint32_t dstSubresource, const Rect* dstRect, const uint8_t* texel);
    // Fills with a color given as ARGB (Direct3D 9 ColorFill).
    void fillArgb(Texture* dst, uint32_t dstSubresource, const Rect* dstRect, uint32_t argb);

    void flush() { m_context->Flush(); }
    // Everything bound on D3D11 is unknown (after foreign D3D11 calls).
    void invalidateBindings();
    // With diagnostics on: every texture format and vertex element type read
    // back as shaders read it, against what Direct3D 9 gives
    // (legacy_selftest.cpp).
    void selfTestFormats();
    void selfTestVertexTypes();
    // Direct3D 9's default states, nothing bound (Reset).
    void resetState();
    // A dxup_trace9_go.txt frame is being traced (queries log into it).
    bool tracing9() const { return m_trace9; }
    // An occlusion query's result the game read (glitch reports: a game
    // that sees nothing hides what it would draw).
    void noteOcclusionResult(uint64_t samples) {
      m_occlusion.results++;
      m_occlusion.zeros += samples == 0;
    }
    // Traced frames: the bound depth buffer's values (statistics, the probe
    // pixel, an image next to the DLL), once per frame.
    void traceDepth9(const char* when);

  private:
    Device() = default;
    bool init(HWND window, std::string& error);

    struct TransformConstants;
    struct VertexConstants;
    struct PixelConstants;
    struct ShaderFixedConstants;
    struct DrawArgs;
    struct ShaderWarmup;

    // Compiles the shaders draws used in earlier runs on workers, from the
    // device's creation on (the game creates them later).
    void startShaderWarmup();
    void stopShaderWarmup();
    std::shared_ptr<ShaderWarmup> m_shaderWarmup;

    void applyState();
    void selectShaders();
    ID3D11InputLayout* inputLayout();
    void updateVertexConstants();
    void updateTransformConstants();
    void updatePixelConstants();
    void updateShaderConstants(bool pixel);
    void bindShaderConstants(bool pixel);
    void updateShaderInts(bool pixel);
    void updateShaderFixedConstants();
    ID3D11BlendState*        blendState();
    D3D11_BLEND_DESC         blendDesc() const;
    ID3D11DepthStencilState* depthStencilState();
    ID3D11RasterizerState*   rasterizerState();
    ID3D11SamplerState*      d3dSampler(uint32_t sampler, bool compare);
    void bindTargets();
    void bindViewport();
    void bindTextures();
    void drawWith(const DrawArgs& args);
    void widenVertexRange(DrawArgs& a);
    // Uploads application data into the ring buffers; returns offsets.
    uint32_t uploadVertices(const void* data, uint32_t size);
    uint32_t uploadIndices(const void* data, uint32_t size);
    void drawQuad(ID3D11PixelShader* shader, const float rect[4], const float texRect[4], const float color[4],
                  float depth, float keyEnabled);
    void markTargetsWritten();
    // Points drawn as quads (point sprites, sizes above one pixel): a
    // geometry shader per vertex output layout.
    bool expandPoints() const;
    bool fixedPointSizes() const;
    ID3D11GeometryShader* pointShader(uint32_t slots, bool clip);
    void updatePointConstants();

    // ---- D3D11 objects ----
    HMODULE                         m_d3d11Module = nullptr;
    void (WINAPI* m_noteBackBufferSize)(IUnknown*, UINT, UINT) = nullptr;   // DXUP's d3d11.dll only
    HRESULT (WINAPI* m_createMappedView)(IUnknown*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, UINT,
                                         ID3D11ShaderResourceView**) = nullptr;   // DXUP's d3d11.dll only
    HRESULT (WINAPI* m_createSampler)(IUnknown*, const D3D11_SAMPLER_DESC*, ID3D11SamplerState**) = nullptr;
    // DXUP's D3D11 (DXUPWriteBuffer): its worker thread writes dynamic buffers.
    HRESULT (WINAPI* m_writeBuffer)(ID3D11DeviceContext*, ID3D11Buffer*, UINT, const void*, UINT) = nullptr;
    // DXUPWriteBufferWith: the worker thread keeps a copy of each stage's
    // float constants (WorkerConstants, its own after creation) and writes
    // the windows from it; uploads pass the constants changed since the last.
    using WriteWith = void (*)(void* user, uint8_t* target, size_t size, const void* data, size_t dataSize);
    using PackWith = void (*)(void* packUser, uint8_t* data);
    HRESULT (WINAPI* m_writeBufferWith)(ID3D11DeviceContext*, ID3D11Buffer*, UINT, UINT, WriteWith, void*, PackWith, void*,
                                        UINT) = nullptr;
    struct WorkerConstants;
    struct WorkerPack;
    static void writeWorkerConstants(void* user, uint8_t* target, size_t size, const void* data, size_t dataSize);
    static void freeWorkerConstants(void* user, uint8_t* target, size_t size, const void* data, size_t dataSize);
    static void packWorkerConstants(void* packUser, uint8_t* data);
    WorkerConstants* m_workerConstants[2] = {};
    uint64_t m_changedConstants[2] = { ~0ull, ~0ull };   // groups of 4 floats changed since the last upload
    std::vector<uint8_t> m_uploadScratch;   // a constant window with def constants
    Ref<ID3D11Device>               m_device;
    Ref<ID3D11DeviceContext>        m_context;
    Ref<IDXGIFactory2>              m_factory;
    struct SwapChain {
      HWND window = nullptr;
      Ref<IDXGISwapChain1> swapChain;
      // Buffer 0's view (flip model: always the buffer drawn next), and the
      // buffers' size; both until a resize.
      Ref<ID3D11RenderTargetView> rtv;
      UINT bufferWidth = 0, bufferHeight = 0;
      UINT width = 0, height = 0;
      UINT flags = 0;
    };
    int m_tearing = -1;   // whether presents without VSync may tear (unknown: -1)
    std::vector<uint32_t> m_fanIndices;   // triangle fans drawn as lists
    std::vector<std::unique_ptr<SwapChain>> m_swapChains;
    Ref<ID3D11VertexShader>         m_ffpVs;
    Ref<ID3D11PixelShader>          m_ffpPs;
    Ref<ID3D11VertexShader>         m_quadVs;
    Ref<ID3D11PixelShader>          m_clearPs;
    Ref<ID3D11PixelShader>          m_blitPs;
    // Application memory blitted onto GPU targets goes through this one.
    std::shared_ptr<Texture>        m_blitSource;
    Ref<ID3D11PixelShader>          m_gammaPs;
    Ref<ID3D11Texture2D>            m_gammaTexture;
    Ref<ID3D11ShaderResourceView>   m_gammaView;
    uint64_t                        m_gammaHash = 0;
    Ref<ID3D11Buffer>               m_vertexConstants;
    Ref<ID3D11Buffer>               m_transformConstants;
    Ref<ID3D11Buffer>               m_pixelConstants;
    Ref<ID3D11Buffer>               m_quadConstants;
    // Shader floats: windows of a ring per stage (vertex, pixel), bound with
    // offsets; integers and booleans in a buffer each.
    static constexpr uint32_t kConstantRingSize = 65536;
    Ref<ID3D11DeviceContext1>       m_context1;
    Ref<ID3D11Buffer>               m_constantRings[2];
    uint32_t                        m_constantRingOffsets[2] = { kConstantRingSize, kConstantRingSize };
    // The window last written per stage: bound again for another shader
    // that reads no more (the ring has not been discarded since).
    struct ConstantWindow { UINT first = 0, count = 0, floats = 0; bool valid = false; };
    ConstantWindow                  m_constantWindows[2];
    Ref<ID3D11Buffer>               m_shaderIntConstants[2];
    Ref<ID3D11Buffer>               m_shaderFixedConstants;
    Ref<ID3D11Buffer>               m_clipPlaneConstants;   // shader vertex stage, b3
    Ref<ID3D11Buffer>               m_pointConstants;
    std::unordered_map<uint32_t, ID3D11GeometryShader*> m_pointShaders;
    ID3D11GeometryShader*           m_boundGs = nullptr;
    Ref<ID3D11Buffer>               m_defaultVertex;   // missing vertex elements, stride 0
    Ref<ID3D11Buffer>               m_vertexRing;
    Ref<ID3D11Buffer>               m_indexRing;
    uint32_t                        m_vertexRingSize = 0, m_vertexRingOffset = 0;
    uint32_t                        m_indexRingSize = 0, m_indexRingOffset = 0;
    Ref<ID3D11SamplerState>         m_pointSampler, m_linearSampler;
    Ref<ID3D11BlendState>           m_opaqueBlend;
    Ref<ID3D11DepthStencilState>    m_noDepth, m_depthAlways;
    Ref<ID3D11RasterizerState>      m_quadRaster;
    std::unordered_map<uint64_t, ID3D11InputLayout*>        m_inputLayouts;
    std::unordered_map<uint64_t, ID3D11BlendState*>         m_blendStates;   // by the states they come from
    // The states of recent draws by key, before the maps (games switch
    // among a few states draw by draw): 32 sets of two, the recent first.
    template<typename T>
    struct StateCache {
      struct Entry { uint64_t key = 0; T* state = nullptr; };
      Entry entries[64];
      Entry* set(uint64_t key) { return &entries[((uint32_t(key) ^ uint32_t(key >> 32)) * 0x9e3779b9u >> 27) * 2]; }
      T* find(uint64_t key) {
        Entry* e = set(key);
        if (e[0].state && e[0].key == key)
          return e[0].state;
        if (!e[1].state || e[1].key != key)
          return nullptr;
        std::swap(e[0], e[1]);
        return e[0].state;
      }
      void insert(uint64_t key, T* state) {
        Entry* e = set(key);
        e[1] = e[0];
        e[0] = { key, state };
      }
    };
    StateCache<ID3D11BlendState>                            m_blendCache;
    StateCache<ID3D11DepthStencilState>                     m_depthCache;
    StateCache<ID3D11RasterizerState>                       m_rasterCache;
    StateCache<ID3D11InputLayout>                           m_layoutCache;
    StateCache<ID3D11SamplerState>                          m_samplerCache;
    std::unordered_map<uint64_t, ID3D11DepthStencilState*>  m_depthStates;
    std::unordered_map<uint64_t, ID3D11RasterizerState*>    m_rasterStates;
    std::unordered_map<uint64_t, ID3D11SamplerState*>       m_samplerStates;
    std::unordered_map<uint32_t, std::unique_ptr<VertexDecl>> m_fvfDecls;
    uint32_t                        m_lastFvf = 0;
    VertexDecl*                     m_lastFvfDecl = nullptr;

    // ---- Legacy state ----
    std::array<uint32_t, RsCount> m_renderStates = {};
    std::array<std::array<uint32_t, TssCount>, kFixedStages> m_stageStates = {};
    std::array<std::array<uint32_t, SampCount>, kSamplerCount> m_samplerStatesValues = {};
    std::array<Texture*, kSamplerCount> m_textures = {};
    std::array<std::array<float, 16>, 4>  m_world = {};
    std::array<float, 16>    m_view = {};
    std::array<float, 16>    m_projection = {};
    std::array<std::array<float, 16>, 8>  m_textureTransforms = {};
    Material                 m_material = {};
    std::vector<Light>       m_lights;
    std::vector<bool>        m_lightEnabled;
    Viewport                 m_viewport = {};
    Rect                     m_scissor = {};
    std::array<Texture*, kMaxRenderTargets> m_renderTargets = {};
    std::array<uint32_t, kMaxRenderTargets> m_renderTargetSubresources = {};
    Texture*                 m_depthStencil = nullptr;
    std::array<std::array<float, 4>, 6> m_clipPlanes = {};

    // Direct3D 8/9
    struct Stream {
      Buffer*  buffer = nullptr;
      uint32_t offset = 0;
      uint32_t stride = 0;
      uint32_t frequency = 1;
    };
    std::array<Stream, kMaxStreams> m_streams = {};
    Buffer*      m_indices = nullptr;
    bool         m_index32 = false;
    VertexDecl*  m_decl = nullptr;
    Shader*      m_vertexShader = nullptr;
    Shader*      m_pixelShader = nullptr;
    struct ConstantStore {
      float    f[256][4];
      int32_t  i[16][4];
      uint32_t b[16];
      uint32_t limits[4];   // the last float constant, vertex input modes (dxup_inputModes, dxup_inputWOne)
    };
    ConstantStore m_constants[2] = {};      // vertex, pixel
    // The input modes of the last vertex shader and declaration (by hash).
    uint64_t m_inputModesShader = 0, m_inputModesDecl = 0;
    uint32_t m_inputModes[2] = {};
    // Float constants the application set: those below this, ever.
    uint32_t m_constantsWritten[2] = {};
    // The float constants of a draw's shader: relative reads find no more
    // than the application set and one zero constant past them (DXVK too
    // uploads no more than the highest constant set).
    uint32_t shaderFloats(bool pixel, const ShaderInfo9& info) const;
    bool uploadConstants(bool pixel, ID3D11Buffer* ring, uint32_t offset, uint32_t copied, const ShaderInfo9& info,
                         bool discard);
    bool uploadWorkerConstants(bool pixel, ID3D11Buffer* ring, uint32_t offset, uint32_t copied, const ShaderInfo9& info,
                               bool discard);
    bool m_shaderConstantsDirty[2] = { true, true };   // the floats changed
    bool m_shaderConstantsUnbound[2] = { true, true };  // b0 lost its window
    bool m_shaderIntsDirty[2] = { true, true };

    // The pipeline the bound state selects.
    const Shader::Variant* m_vsVariant = nullptr;
    const Shader::Variant* m_psVariant = nullptr;
    // Glitch reports (dxup_frame_dump.txt): shader constants that are NaN or
    // infinite turn what a shader draws black (checkConstants).
    struct NonFiniteConstants {
      uint32_t uploads = 0;        // this frame
      ReportThrottle frames;
      uint32_t presents = 0;
      bool     pixel = false;      // the first of this frame
      uint32_t reg = 0, component = 0;
      float    value = 0.0f;
      uint64_t shader = 0;
    };
    NonFiniteConstants m_nonFinite;
    void checkConstants(bool pixel, uint32_t floats);
    void logNonFiniteConstants();
    // Draws DXUP could not draw: each cause logged once with what the draw
    // lacked, and with saved frames on how many per frame (missing objects
    // leave no other trace).
    enum DropReason : uint32_t { DropDeclaration, DropVertexShader, DropPixelShader, DropInputLayout, DropUpload,
                                 DropIndices, DropReasons };
    struct DroppedDraws {
      uint32_t counts[DropReasons] = {};   // this frame
      ReportThrottle frames;
      std::vector<uint64_t> causes;        // logged once
    };
    DroppedDraws m_dropped;
    void dropDraw(DropReason reason, uint32_t bytes = 0);
    void logDroppedDraws();
    struct {
      uint32_t results = 0, zeros = 0;   // this frame
      uint32_t blindFrames = 0;          // frames in a row with nearly all zero
      bool logged = false;
    } m_occlusion;
    void logOcclusionResults();
    // dxup_trace9_go.txt next to the DLL: the draws of the next frame with
    // their shaders, declaration, streams (and first vertices), indices and
    // constants (checked every 64 presents, once per file).
    bool m_trace9 = false, m_trace9Done = false;
    uint32_t m_trace9Draw = 0;
    uint64_t m_trace9LastCheck = 0;   // GetTickCount64
    int32_t m_trace9X = -1, m_trace9Y = -1;   // probe pixel of target 0 after each draw
    Ref<ID3D11Texture2D> m_trace9Staging;
    DXGI_FORMAT m_trace9Format = DXGI_FORMAT_UNKNOWN;
    void traceDraw9(const DrawArgs& a);
    void traceProbe9();
    bool m_trace9DepthDone = false;
    void traceDepthMean9();
    // The bound depth buffer copied to m_trace9DepthStaging and mapped (the
    // caller unmaps it): depth 0-1 of any single-sampled format.
    struct DepthReadback {
      const uint8_t* data = nullptr;
      uint32_t pitch = 0, width = 0, height = 0;
      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
      double at(uint32_t x, uint32_t y) const;
    };
    bool readDepth9(DepthReadback& depth);
    Ref<ID3D11Texture2D> m_trace9DepthStaging;
    bool growRing(Ref<ID3D11Buffer>& ring, uint32_t& ringSize, uint32_t& ringOffset, uint32_t size, UINT bind);
    uint32_t m_rangeReports = 0;
    uint32_t m_ffpFvf = 0;                  // what the fixed function sees
    uint32_t m_psTextureTypes = 0;          // 2 bits per sampler: the bound textures' types
    uint32_t m_shadowMask = 0;              // samplers with depth textures compared

    // What changed since the last draw: draws set only that on D3D11.
    enum Dirty : uint32_t {
      DirtyBlend      = 1u << 0,
      DirtyDepth      = 1u << 1,
      DirtyRaster     = 1u << 2,
      DirtyStencilRef = 1u << 3,
      DirtyShaders    = 1u << 4,   // shader selection, input layout, constant buffers
      DirtyLayout     = 1u << 5,
      DirtyAll        = 0x3fu,
    };
    uint32_t m_dirty = DirtyAll;
    uint32_t m_textureDirtyMask = 0xfffff;
    uint32_t m_samplerDirtyMask = 0xfffff;
    // Samplers whose texture is a bound target: bound again each draw
    // (bindTextures copies the targets the shaders read).
    uint32_t m_feedbackMask = 0;
    bool m_vertexConstantsDirty = true;
    bool m_transformConstantsDirty = true;
    bool m_pixelConstantsDirty = true;
    bool m_shaderFixedDirty = true;
    bool m_clipPlanesDirty = true;
    bool m_targetsDirty = true;
    bool m_viewportDirty = true;   // viewport and scissor only
    ID3D11ShaderResourceView* m_boundViews[24] = {};
    ID3D11RenderTargetView*   m_boundRtvs[kMaxRenderTargets] = {};   // valid with m_targetsBound
    ID3D11DepthStencilView*   m_boundDsv = nullptr;
    bool                      m_targetsBound = false;
    ID3D11ShaderResourceView* m_boundVertexViews[4] = {};
    ID3D11SamplerState*       m_boundSamplers[kSamplerCount] = {};
    ID3D11Buffer*             m_boundStreams[kMaxStreams] = {};
    UINT                      m_boundStreamOffsets[kMaxStreams] = {};
    UINT                      m_boundStreamStrides[kMaxStreams] = {};
    ID3D11InputLayout*        m_boundLayout = nullptr;
    bool                      m_boundFfpPixel = true;
    bool                      m_defaultStreamBound = false;
    bool                      m_fixedPixelsWithShaders = false;
    bool                      m_texturesPending = true;
  };

}
