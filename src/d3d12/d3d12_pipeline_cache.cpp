// Pipeline state cache: the descriptions of the graphics pipelines an
// application used, kept in dxup_pso_cache\pipelines.bin. When a later run
// creates every shader of a stored pipeline again, a compiler thread builds
// the pipeline in the background, so the first draw that needs it finds it
// compiled instead of stalling the frame (D3D11 drivers compile shaders when
// they are created; D3D12 pipelines also depend on state known at the draw).

#include "d3d12_backend_device.h"
#include "../util/util_config.h"
#include "../util/util_types.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace dxup::d3d12 {

#ifdef _WIN32
  namespace {
    constexpr char     kStateCacheMagic[8] = { 'D', 'X', 'U', 'P', 'P', 'S', 'C', '1' };
    constexpr uint32_t kStateCacheVersion  = 2;   // 2: sample quality
    // A record larger than this is not a pipeline description: the file
    // is damaged from there on.
    constexpr uint32_t kMaxRecordSize      = 64u << 10;

    uint64_t shaderId(uint64_t hash, uint64_t size) {
      return hash ^ (size * 0x9e3779b97f4a7c15ull);
    }

    class RecordWriter {
    public:
      template<typename T>
      void put(T value) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(T));
      }
      void putString(const char* text) {
        const uint16_t length = text ? static_cast<uint16_t>((std::min)(std::strlen(text), size_t(0xFFFF))) : 0;
        put(length);
        if (length)
          data.insert(data.end(), text, text + length);
      }
      std::vector<uint8_t> data;
    };

    class RecordReader {
    public:
      RecordReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}
      template<typename T>
      T get() {
        T value = {};
        if (m_offset + sizeof(T) > m_size) {
          m_failed = true;
          return value;
        }
        std::memcpy(&value, m_data + m_offset, sizeof(T));
        m_offset += sizeof(T);
        return value;
      }
      std::string getString() {
        const uint16_t length = get<uint16_t>();
        if (m_failed || m_offset + length > m_size) {
          m_failed = true;
          return {};
        }
        std::string text(reinterpret_cast<const char*>(m_data + m_offset), length);
        m_offset += length;
        return text;
      }
      bool ok() const { return !m_failed && m_offset == m_size; }
    private:
      const uint8_t* m_data;
      size_t         m_size;
      size_t         m_offset = 0;
      bool           m_failed = false;
    };

    void writeStencilOp(RecordWriter& w, const StencilOpDesc& op) {
      w.put(uint8_t(op.stencilFailOp));
      w.put(uint8_t(op.stencilDepthFailOp));
      w.put(uint8_t(op.stencilPassOp));
      w.put(uint8_t(op.stencilFunc));
    }

    StencilOpDesc readStencilOp(RecordReader& r) {
      StencilOpDesc op;
      op.stencilFailOp      = StencilOp(r.get<uint8_t>());
      op.stencilDepthFailOp = StencilOp(r.get<uint8_t>());
      op.stencilPassOp      = StencilOp(r.get<uint8_t>());
      op.stencilFunc        = ComparisonFunc(r.get<uint8_t>());
      return op;
    }

    std::vector<uint8_t> encodePipeline(const GraphicsPipelineDesc& desc) {
      RecordWriter w;
      const uint64_t hashes[5] = { desc.vsBytecodeHash, desc.psBytecodeHash, desc.gsBytecodeHash,
                                   desc.hsBytecodeHash, desc.dsBytecodeHash };
      const uint64_t sizes[5]  = { desc.vsBytecodeSize, desc.psBytecodeSize, desc.gsBytecodeSize,
                                   desc.hsBytecodeSize, desc.dsBytecodeSize };
      for (uint32_t i = 0; i < 5; i++) {
        w.put(hashes[i]);
        w.put(sizes[i]);
      }

      w.put(desc.inputLayoutHash);
      const uint32_t elements = desc.inputLayout.elements ? desc.inputLayout.numElements : 0;
      w.put(elements);
      for (uint32_t i = 0; i < elements; i++) {
        const auto& e = desc.inputLayout.elements[i];
        w.putString(e.semanticName);
        w.put(e.semanticIndex);
        w.put(uint32_t(e.format));
        w.put(e.inputSlot);
        w.put(e.byteOffset);
        w.put(uint8_t(e.inputClass));
        w.put(e.instanceDataStepRate);
      }

      const BlendState& b = desc.blendState;
      w.put(uint8_t(b.alphaToCoverageEnable));
      w.put(uint8_t(b.independentBlendEnable));
      for (const auto& rt : b.renderTargets) {
        w.put(uint8_t(rt.blendEnable));
        w.put(uint8_t(rt.srcBlend));
        w.put(uint8_t(rt.dstBlend));
        w.put(uint8_t(rt.blendOp));
        w.put(uint8_t(rt.srcBlendAlpha));
        w.put(uint8_t(rt.dstBlendAlpha));
        w.put(uint8_t(rt.blendOpAlpha));
        w.put(rt.writeMask);
        w.put(uint8_t(rt.logicOpEnable));
        w.put(rt.logicOp);
      }
      w.put(desc.blendStateHash);

      const RasterizerState& rs = desc.rasterizerState;
      w.put(uint8_t(rs.fillMode));
      w.put(uint8_t(rs.cullMode));
      w.put(uint8_t(rs.frontCounterClockwise));
      w.put(rs.depthBias);
      w.put(rs.depthBiasClamp);
      w.put(rs.slopeScaledDepthBias);
      w.put(uint8_t(rs.depthClipEnable));
      w.put(uint8_t(rs.scissorEnable));
      w.put(uint8_t(rs.multisampleEnable));
      w.put(uint8_t(rs.antialiasedLineEnable));
      w.put(rs.forcedSampleCount);
      w.put(uint8_t(rs.conservativeRaster));
      w.put(desc.rasterizerStateHash);

      const DepthStencilState& ds = desc.depthStencilState;
      w.put(uint8_t(ds.depthEnable));
      w.put(uint8_t(ds.depthWriteEnable));
      w.put(uint8_t(ds.depthFunc));
      w.put(uint8_t(ds.stencilEnable));
      w.put(ds.stencilReadMask);
      w.put(ds.stencilWriteMask);
      writeStencilOp(w, ds.frontFace);
      writeStencilOp(w, ds.backFace);
      w.put(desc.depthStencilStateHash);

      w.put(uint32_t(desc.topology));
      w.put(uint32_t(desc.stripCutIndexType));
      w.put(desc.numRenderTargets);
      for (DXGI_FORMAT format : desc.rtvFormats)
        w.put(uint32_t(format));
      w.put(uint32_t(desc.dsvFormat));
      w.put(desc.sampleCount);
      w.put(desc.sampleMask);
      w.put(uint8_t(desc.sampleShading));
      w.put(desc.sampleQuality);

      const uint32_t soEntries = desc.soEntries ? desc.numSOEntries : 0;
      w.put(soEntries);
      for (uint32_t i = 0; i < soEntries; i++) {
        const auto& e = desc.soEntries[i];
        w.put(e.stream);
        w.putString(e.semanticName);
        w.put(e.semanticIndex);
        w.put(e.startComponent);
        w.put(e.componentCount);
        w.put(e.outputSlot);
      }
      const uint32_t soStrides = desc.soBufferStrides ? desc.numSOStrides : 0;
      w.put(soStrides);
      for (uint32_t i = 0; i < soStrides; i++)
        w.put(desc.soBufferStrides[i]);
      w.put(desc.rasterizedStream);
      return std::move(w.data);
    }
  }

  // A stored pipeline description with the storage its pointers refer to.
  struct D3D12BackendDevice::DecodedPipeline {
    GraphicsPipelineDesc desc;
    std::vector<InputElementDesc> elements;
    std::vector<GraphicsPipelineDesc::SOEntry> soEntries;
    std::vector<uint32_t> soStrides;
    std::deque<std::string> names;
    // The shaders, alive while the pipeline compiles.
    std::shared_ptr<D3D12BackendShader> shaders[5];

    uint64_t shaderHash(uint32_t stage) const {
      const uint64_t hashes[5] = { desc.vsBytecodeHash, desc.psBytecodeHash, desc.gsBytecodeHash,
                                   desc.hsBytecodeHash, desc.dsBytecodeHash };
      return hashes[stage];
    }
    uint64_t shaderSize(uint32_t stage) const {
      const uint64_t sizes[5] = { desc.vsBytecodeSize, desc.psBytecodeSize, desc.gsBytecodeSize,
                                  desc.hsBytecodeSize, desc.dsBytecodeSize };
      return sizes[stage];
    }
    void setBytecode(uint32_t stage, const void* code) {
      const void** targets[5] = { &desc.vsBytecode, &desc.psBytecode, &desc.gsBytecode,
                                  &desc.hsBytecode, &desc.dsBytecode };
      *targets[stage] = code;
    }

    bool decode(const std::vector<uint8_t>& record) {
      RecordReader r(record.data(), record.size());
      uint64_t* hashes[5] = { &desc.vsBytecodeHash, &desc.psBytecodeHash, &desc.gsBytecodeHash,
                              &desc.hsBytecodeHash, &desc.dsBytecodeHash };
      size_t* sizes[5] = { &desc.vsBytecodeSize, &desc.psBytecodeSize, &desc.gsBytecodeSize,
                           &desc.hsBytecodeSize, &desc.dsBytecodeSize };
      for (uint32_t i = 0; i < 5; i++) {
        *hashes[i] = r.get<uint64_t>();
        *sizes[i] = static_cast<size_t>(r.get<uint64_t>());
      }

      desc.inputLayoutHash = r.get<uint64_t>();
      const uint32_t elementCount = r.get<uint32_t>();
      if (elementCount > 32)
        return false;
      elements.resize(elementCount);
      for (auto& e : elements) {
        names.push_back(r.getString());
        e.semanticName = names.back().c_str();
        e.semanticIndex = r.get<uint32_t>();
        e.format = DXGI_FORMAT(r.get<uint32_t>());
        e.inputSlot = r.get<uint32_t>();
        e.byteOffset = r.get<uint32_t>();
        e.inputClass = InputClassification(r.get<uint8_t>());
        e.instanceDataStepRate = r.get<uint32_t>();
      }
      desc.inputLayout.elements = elements.empty() ? nullptr : elements.data();
      desc.inputLayout.numElements = elementCount;

      BlendState& b = desc.blendState;
      b.alphaToCoverageEnable = r.get<uint8_t>() != 0;
      b.independentBlendEnable = r.get<uint8_t>() != 0;
      for (auto& rt : b.renderTargets) {
        rt.blendEnable = r.get<uint8_t>() != 0;
        rt.srcBlend = BlendFactor(r.get<uint8_t>());
        rt.dstBlend = BlendFactor(r.get<uint8_t>());
        rt.blendOp = BlendOp(r.get<uint8_t>());
        rt.srcBlendAlpha = BlendFactor(r.get<uint8_t>());
        rt.dstBlendAlpha = BlendFactor(r.get<uint8_t>());
        rt.blendOpAlpha = BlendOp(r.get<uint8_t>());
        rt.writeMask = r.get<uint8_t>();
        rt.logicOpEnable = r.get<uint8_t>() != 0;
        rt.logicOp = r.get<uint8_t>();
      }
      desc.blendStateHash = r.get<uint64_t>();

      RasterizerState& rs = desc.rasterizerState;
      rs.fillMode = FillMode(r.get<uint8_t>());
      rs.cullMode = CullMode(r.get<uint8_t>());
      rs.frontCounterClockwise = r.get<uint8_t>() != 0;
      rs.depthBias = r.get<int32_t>();
      rs.depthBiasClamp = r.get<float>();
      rs.slopeScaledDepthBias = r.get<float>();
      rs.depthClipEnable = r.get<uint8_t>() != 0;
      rs.scissorEnable = r.get<uint8_t>() != 0;
      rs.multisampleEnable = r.get<uint8_t>() != 0;
      rs.antialiasedLineEnable = r.get<uint8_t>() != 0;
      rs.forcedSampleCount = r.get<uint32_t>();
      rs.conservativeRaster = r.get<uint8_t>() != 0;
      desc.rasterizerStateHash = r.get<uint64_t>();

      DepthStencilState& ds = desc.depthStencilState;
      ds.depthEnable = r.get<uint8_t>() != 0;
      ds.depthWriteEnable = r.get<uint8_t>() != 0;
      ds.depthFunc = ComparisonFunc(r.get<uint8_t>());
      ds.stencilEnable = r.get<uint8_t>() != 0;
      ds.stencilReadMask = r.get<uint8_t>();
      ds.stencilWriteMask = r.get<uint8_t>();
      ds.frontFace = readStencilOp(r);
      ds.backFace = readStencilOp(r);
      desc.depthStencilStateHash = r.get<uint64_t>();

      desc.topology = PrimitiveTopology(r.get<uint32_t>());
      desc.stripCutIndexType = IndexType(r.get<uint32_t>());
      desc.numRenderTargets = r.get<uint32_t>();
      if (desc.numRenderTargets > 8)
        return false;
      for (DXGI_FORMAT& format : desc.rtvFormats)
        format = DXGI_FORMAT(r.get<uint32_t>());
      desc.dsvFormat = DXGI_FORMAT(r.get<uint32_t>());
      desc.sampleCount = r.get<uint32_t>();
      desc.sampleMask = r.get<uint32_t>();
      desc.sampleShading = r.get<uint8_t>() != 0;
      desc.sampleQuality = r.get<uint32_t>();

      const uint32_t soCount = r.get<uint32_t>();
      if (soCount > 64)
        return false;
      soEntries.resize(soCount);
      for (auto& e : soEntries) {
        e.stream = r.get<uint32_t>();
        names.push_back(r.getString());
        e.semanticName = names.back().empty() ? nullptr : names.back().c_str();
        e.semanticIndex = r.get<uint32_t>();
        e.startComponent = r.get<uint8_t>();
        e.componentCount = r.get<uint8_t>();
        e.outputSlot = r.get<uint8_t>();
      }
      desc.soEntries = soEntries.empty() ? nullptr : soEntries.data();
      desc.numSOEntries = soCount;
      const uint32_t strideCount = r.get<uint32_t>();
      if (strideCount > 4)
        return false;
      soStrides.resize(strideCount);
      for (auto& stride : soStrides)
        stride = r.get<uint32_t>();
      desc.soBufferStrides = soStrides.empty() ? nullptr : soStrides.data();
      desc.numSOStrides = strideCount;
      desc.rasterizedStream = r.get<uint32_t>();
      // Every pipeline has a vertex shader.
      return r.ok() && desc.vsBytecodeSize;
    }
  };

  void D3D12BackendDevice::loadPipelineStateCache() {
    if (m_psoDiskCacheDir.empty() || !UserOverrides::get().shaderPrecompile)
      return;
    m_stateCachePath = m_psoDiskCacheDir + "\\pipelines.bin";
    // Shared: another device of this process or another process may append
    // meanwhile.
    HANDLE file = CreateFileA(m_stateCachePath.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return;
    std::vector<uint8_t> contents;
    LARGE_INTEGER fileSize = {};
    if (GetFileSizeEx(file, &fileSize) && fileSize.QuadPart > 0 && fileSize.QuadPart < (LONGLONG(1) << 31)) {
      contents.resize(size_t(fileSize.QuadPart));
      DWORD read = 0;
      if (!ReadFile(file, contents.data(), DWORD(contents.size()), &read, nullptr) || read != contents.size())
        contents.clear();
    }
    CloseHandle(file);
    m_stateCacheLoadedSize = contents.size();

    const size_t headerSize = sizeof(kStateCacheMagic) + sizeof(uint32_t);
    uint32_t version = 0;
    if (contents.size() >= headerSize)
      std::memcpy(&version, contents.data() + sizeof(kStateCacheMagic), sizeof(version));
    if (contents.size() < headerSize || std::memcmp(contents.data(), kStateCacheMagic, sizeof(kStateCacheMagic))
     || version != kStateCacheVersion) {
      logMessage(LogLevel::Warn, "Pipeline cache: '%s' has another format, starting over", m_stateCachePath.c_str());
      return;
    }

    size_t offset = headerSize;
    while (offset + sizeof(uint32_t) <= contents.size()) {
      uint32_t size = 0;
      std::memcpy(&size, contents.data() + offset, sizeof(size));
      if (!size || size > kMaxRecordSize || offset + sizeof(uint32_t) + size > contents.size())
        break;
      std::vector<uint8_t> record(contents.begin() + ptrdiff_t(offset + sizeof(uint32_t)),
                                  contents.begin() + ptrdiff_t(offset + sizeof(uint32_t) + size));
      DecodedPipeline decoded;
      if (!decoded.decode(record))
        break;
      offset += sizeof(uint32_t) + size;
      if (!m_storedPipelineKeys.insert(graphicsPipelineKey(decoded.desc)).second)
        continue;

      if (decoded.shaderSize(1)) {
        (decoded.desc.sampleShading ? m_storedSampleShadingShaders : m_storedPixelRateShaders)
          .insert(shaderId(decoded.shaderHash(1), decoded.shaderSize(1)));
      }
      StoredPipeline stored;
      stored.record = std::move(record);
      const uint32_t index = static_cast<uint32_t>(m_storedPipelines.size());
      for (uint32_t stage = 0; stage < 5; stage++) {
        if (!decoded.shaderSize(stage))
          continue;
        const uint64_t id = shaderId(decoded.shaderHash(stage), decoded.shaderSize(stage));
        auto& users = m_storedPipelinesByShader[id];
        // A shader in two stages of one pipeline counts once.
        if (users.empty() || users.back() != index) {
          users.push_back(index);
          stored.missing++;
        }
      }
      m_storedPipelines.push_back(std::move(stored));
    }
    m_stateCacheValidSize = offset;
    logMessage(LogLevel::Info, "Pipeline cache: %zu pipelines, %zu shaders from '%s'",
      m_storedPipelines.size(), m_storedPipelinesByShader.size(), m_stateCachePath.c_str());
  }

  bool D3D12BackendDevice::noteShaderCreated(const std::shared_ptr<D3D12BackendShader>& shader, bool& perPixelMissing,
                                             bool& perSampleMissing) {
    const uint64_t id = shaderId(shader->getBytecodeHash(), shader->getBytecodeSize());
    const bool pixel = shader->getStage() == ShaderStage::Pixel;
    perPixelMissing = pixel && !m_storedPixelRateShaders.count(id);
    perSampleMissing = pixel && !m_storedSampleShadingShaders.count(id);
    std::vector<uint32_t> ready;
    {
      std::lock_guard<std::mutex> lock(m_stateCacheMutex);
      auto users = m_storedPipelinesByShader.find(id);
      if (users == m_storedPipelinesByShader.end())
        return false;
      auto [live, first] = m_stateCacheShaders.try_emplace(id, shader);
      if (!first) {
        // Created again: later pipelines compile with this one.
        live->second = shader;
        return true;
      }
      for (uint32_t index : users->second) {
        StoredPipeline& stored = m_storedPipelines[index];
        if (stored.missing && --stored.missing == 0)
          ready.push_back(index);
      }
    }
    for (uint32_t index : ready)
      queuePrecompile(index);
    return true;
  }

  void D3D12BackendDevice::queuePrecompile(uint32_t index) {
    if (m_compileThreads.empty())
      return;
    auto work = std::make_shared<DecodedPipeline>();
    {
      std::lock_guard<std::mutex> lock(m_stateCacheMutex);
      if (!work->decode(m_storedPipelines[index].record))
        return;
      for (uint32_t stage = 0; stage < 5; stage++) {
        if (!work->shaderSize(stage))
          continue;
        auto live = m_stateCacheShaders.find(shaderId(work->shaderHash(stage), work->shaderSize(stage)));
        if (live == m_stateCacheShaders.end() || !(work->shaders[stage] = live->second.lock()))
          return;
        work->setBytecode(stage, work->shaders[stage]->getBytecode());
      }
    }

    const PsoCacheKey key = graphicsPipelineKey(work->desc);
    auto job = std::make_shared<CompileJob>();
    job->run = [this, work, key] {
      if (compileGraphicsPipeline(work->desc, key))
        m_psoStats.precompiled.fetch_add(1, std::memory_order_relaxed);
    };
    {
      std::lock_guard<std::mutex> lock(m_psoCacheMutex);
      if (m_psoCache.count(key) || m_compilingPsos.count(key))
        return;
      m_compilingPsos.insert(key);
      m_queuedPsoJobs.emplace(key, job);
    }
    // Not taken only while the device goes away, which drops the jobs.
    enqueueCompile(job, false);
  }

  void D3D12BackendDevice::storePipelineRecord(const GraphicsPipelineDesc& desc, const PsoCacheKey& key) {
    if (m_stateCachePath.empty() || !m_psoDiskWriter.joinable())
      return;
    {
      std::lock_guard<std::mutex> lock(m_stateCacheMutex);
      if (!m_storedPipelineKeys.insert(key).second)
        return;
    }
    DeferredPsoDiskWrite write;
    write.record = encodePipeline(desc);
    {
      std::lock_guard<std::mutex> lock(m_psoDiskWriteMutex);
      m_deferredPsoDiskWrites.push_back(std::move(write));
    }
    m_psoDiskWriteCv.notify_one();
  }

  void D3D12BackendDevice::appendPipelineRecord(const std::vector<uint8_t>& record) {
    // Runs on the disk writer thread only. The file stays shared: other
    // devices of the process (Trine 4 has three) and other processes append
    // too, each record in one write at the end of the file.
    if (m_stateCacheHandle == INVALID_HANDLE_VALUE) {
      HANDLE handle = CreateFileA(m_stateCachePath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        m_stateCachePath.clear();
        return;
      }
      LARGE_INTEGER size = {};
      GetFileSizeEx(handle, &size);
      // Unchanged since it was read: a partial record a run left behind (or
      // another format) goes, and an empty file gets the header.
      if (size_t(size.QuadPart) == m_stateCacheLoadedSize && m_stateCacheValidSize < m_stateCacheLoadedSize) {
        LARGE_INTEGER end;
        end.QuadPart = LONGLONG(m_stateCacheValidSize);
        SetFilePointerEx(handle, end, nullptr, FILE_BEGIN);
        SetEndOfFile(handle);
        size = end;
      }
      if (!size.QuadPart) {
        uint8_t header[sizeof(kStateCacheMagic) + sizeof(kStateCacheVersion)];
        std::memcpy(header, kStateCacheMagic, sizeof(kStateCacheMagic));
        std::memcpy(header + sizeof(kStateCacheMagic), &kStateCacheVersion, sizeof(kStateCacheVersion));
        DWORD written = 0;
        WriteFile(handle, header, DWORD(sizeof(header)), &written, nullptr);
      }
      CloseHandle(handle);
      // Append-only access: every write lands at the end of the file as it
      // is then, also with other writers.
      m_stateCacheHandle = CreateFileA(m_stateCachePath.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (m_stateCacheHandle == INVALID_HANDLE_VALUE) {
        m_stateCachePath.clear();
        return;
      }
    }
    std::vector<uint8_t> data(sizeof(uint32_t) + record.size());
    const uint32_t size = static_cast<uint32_t>(record.size());
    std::memcpy(data.data(), &size, sizeof(size));
    std::memcpy(data.data() + sizeof(size), record.data(), record.size());
    DWORD written = 0;
    WriteFile(m_stateCacheHandle, data.data(), DWORD(data.size()), &written, nullptr);
  }

  void D3D12BackendDevice::closePipelineStateCache() {
    if (m_stateCacheHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(m_stateCacheHandle);
      m_stateCacheHandle = INVALID_HANDLE_VALUE;
    }
  }
#endif

}
