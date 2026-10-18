#include "util_config.h"
#include "util_types.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dxup {

  namespace {

    std::string trim(const std::string& text) {
      size_t begin = 0;
      size_t end = text.size();
      while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        begin++;
      while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        end--;
      return text.substr(begin, end - begin);
    }

    // Where the settings came from, for the log.
    std::string& configSource() {
      static std::string source = "none";
      return source;
    }

    std::string lower(std::string text) {
      std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    }

#ifdef _WIN32
    std::wstring moduleFileName(HMODULE module) {
      std::wstring path(MAX_PATH, L'\0');
      for (;;) {
        const DWORD length = GetModuleFileNameW(module, &path[0], static_cast<DWORD>(path.size()));
        if (!length)
          return {};
        if (length < path.size()) {
          path.resize(length);
          return path;
        }
        path.resize(path.size() * 2);
      }
    }

    // The folder of a path, without the separator.
    std::wstring folderOf(const std::wstring& path) {
      const size_t slash = path.find_last_of(L"\\/");
      return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    }

    std::wstring fileNameOf(const std::wstring& path) {
      const size_t slash = path.find_last_of(L"\\/");
      return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    std::wstring widen(const std::string& text) {
      if (text.empty())
        return {};
      const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
      std::wstring wide(static_cast<size_t>(length > 0 ? length : 0), L'\0');
      if (length > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &wide[0], length);
      return wide;
    }

    std::string narrow(const std::wstring& text) {
      if (text.empty())
        return {};
      const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
      std::string utf8(static_cast<size_t>(length > 0 ? length : 0), '\0');
      if (length > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &utf8[0], length, nullptr, nullptr);
      return utf8;
    }

    // Folders compare as Windows does: case-insensitive, either separator,
    // with or without a trailing one.
    bool sameFolder(std::wstring a, std::wstring b) {
      for (auto* path : { &a, &b }) {
        for (auto& c : *path) {
          if (c == L'/')
            c = L'\\';
        }
        while (!path->empty() && path->back() == L'\\')
          path->pop_back();
      }
      return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    }

    bool readFile(const std::wstring& path, std::string& text) {
      HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE)
        return false;
      LARGE_INTEGER size = {};
      bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (16 << 20);
      if (ok) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = text.empty() || (ReadFile(file, &text[0], static_cast<DWORD>(text.size()), &read, nullptr) && read == text.size());
      }
      CloseHandle(file);
      // A UTF-8 byte order mark is not part of the first key.
      if (ok && text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        text.erase(0, 3);
      return ok;
    }

    // Where DXUP is installed: DXUP_INSTALL_DIR, else what the setup
    // registered.
    std::wstring installFolder() {
      wchar_t buffer[MAX_PATH * 4] = {};
      const DWORD length = GetEnvironmentVariableW(L"DXUP_INSTALL_DIR", buffer, static_cast<DWORD>(std::size(buffer)));
      if (length && length < std::size(buffer))
        return buffer;
      DWORD size = sizeof(buffer);
      if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\DXUP", L"InstallDir", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
            nullptr, buffer, &size) == ERROR_SUCCESS)
        return buffer;
      return {};
    }

    // The settings of games.cfg for the folder this DLL was copied to. One
    // block per game: "[game]", then "dir =" (the folder of the game's
    // executable, where DXUP's DLLs are), "exe =" and the settings. '#'
    // starts a comment only at the start of a line, so paths may hold it.
    std::string gameSettings(const std::string& text, const std::wstring& dllFolder, const std::wstring& exeName,
                             std::string& matched) {
      struct Block { std::string dir, exe, settings; };
      std::vector<Block> blocks;
      std::istringstream lines(text);
      std::string line;
      while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
          continue;
        if (line[0] == '[') {
          if (lower(line) == "[game]")
            blocks.emplace_back();
          continue;
        }
        const size_t equals = line.find('=');
        if (blocks.empty() || equals == std::string::npos)
          continue;
        const std::string key = lower(trim(line.substr(0, equals)));
        const std::string value = trim(line.substr(equals + 1));
        if (key == "dir")
          blocks.back().dir = value;
        else if (key == "exe")
          blocks.back().exe = value;
        else
          blocks.back().settings += key + " = " + value + "\n";
      }
      const Block* best = nullptr;
      for (const auto& block : blocks) {
        if (!sameFolder(widen(block.dir), dllFolder))
          continue;
        const std::wstring exe = widen(block.exe);
        if (!exe.empty() && CompareStringOrdinal(exe.data(), static_cast<int>(exe.size()), exeName.data(),
              static_cast<int>(exeName.size()), TRUE) == CSTR_EQUAL) {
          best = &block;
          break;
        }
        if (!best)
          best = &block;
      }
      if (!best)
        return {};
      matched = best->dir + (best->exe.empty() ? std::string() : "\\" + best->exe);
      return best->settings;
    }
#endif

    // Settings for this process: dxup.conf next to the DLL, else the game's
    // block in games.cfg of the DXUP installation.
    std::string readConfigFile() {
#ifdef _WIN32
      HMODULE module = nullptr;
      GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&readConfigFile), &module);
      const std::wstring dllFolder = folderOf(moduleFileName(module));
      if (dllFolder.empty())
        return {};
      std::string text;
      if (readFile(dllFolder + L"\\dxup.conf", text)) {
        configSource() = narrow(dllFolder) + "\\dxup.conf";
        return text;
      }
      std::wstring install = installFolder();
      while (!install.empty() && (install.back() == L'\\' || install.back() == L'/'))
        install.pop_back();
      if (install.empty() || !readFile(install + L"\\games.cfg", text))
        return {};
      std::string matched;
      std::string settings = gameSettings(text, dllFolder, fileNameOf(moduleFileName(nullptr)), matched);
      if (!matched.empty())
        configSource() = narrow(install) + "\\games.cfg (" + matched + ")";
      return settings;
#else
      return {};
#endif
    }

    // Games that rely on NVAPI's/AGS's UAV overlap for chains of dispatches
    // into the same views (as DXVK's built-in profiles: God of War).
    bool relaxedUavBarriersByDefault() {
#ifdef _WIN32
      static const wchar_t* const kGames[] = { L"GoW.exe" };
      const std::wstring exe = fileNameOf(moduleFileName(nullptr));
      for (const wchar_t* game : kGames) {
        if (CompareStringOrdinal(exe.data(), static_cast<int>(exe.size()), game, -1, TRUE) == CSTR_EQUAL)
          return true;
      }
#endif
      return false;
    }

    int32_t clampSetting(int32_t value, int32_t low, int32_t high) {
      return value < low ? -1 : (std::min)(value, high);
    }

  }

  std::wstring dxupInstallFolder() {
    return installFolder();
  }

  Config::Config(const std::string& text) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      const size_t comment = line.find('#');
      if (comment != std::string::npos)
        line.resize(comment);
      const size_t equals = line.find('=');
      if (equals == std::string::npos)
        continue;
      std::string key = lower(trim(line.substr(0, equals)));
      std::string value = trim(line.substr(equals + 1));
      if (key.empty() || value.empty())
        continue;
      // A later line for the same key wins.
      auto existing = std::find_if(m_entries.begin(), m_entries.end(),
        [&](const auto& entry) { return entry.first == key; });
      if (existing != m_entries.end())
        existing->second = std::move(value);
      else
        m_entries.emplace_back(std::move(key), std::move(value));
    }
  }

  const Config& Config::get() {
    static const Config config(readConfigFile());
    return config;
  }

  const std::string* Config::find(const char* key) const {
    const std::string wanted = lower(key);
    for (const auto& entry : m_entries) {
      if (entry.first == wanted)
        return &entry.second;
    }
    return nullptr;
  }

  int32_t Config::getInt(const char* key, int32_t fallback) const {
    const std::string* value = find(key);
    if (!value)
      return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value->c_str(), &end, 10);
    if (end == value->c_str() || *end)
      return fallback;
    return static_cast<int32_t>(parsed);
  }

  float Config::getFloat(const char* key, float fallback) const {
    const std::string* value = find(key);
    if (!value)
      return fallback;
    char* end = nullptr;
    const float parsed = std::strtof(value->c_str(), &end);
    if (end == value->c_str() || *end)
      return fallback;
    return parsed;
  }

  bool Config::getBool(const char* key, bool fallback) const {
    const std::string* value = find(key);
    if (!value)
      return fallback;
    const std::string text = lower(*value);
    if (text == "true" || text == "yes" || text == "on" || text == "1")
      return true;
    if (text == "false" || text == "no" || text == "off" || text == "0")
      return false;
    return fallback;
  }

  std::string Config::getString(const char* key, const char* fallback) const {
    const std::string* value = find(key);
    return value ? lower(*value) : lower(fallback);
  }

  std::string Config::getText(const char* key, const char* fallback) const {
    const std::string* value = find(key);
    return value ? *value : std::string(fallback);
  }

  // GPUs of each maker the application can be shown (dxgi.spoofVendor),
  // with a driver version of that maker's numbering.
  struct GpuPreset {
    const char*    name;
    uint32_t       vendorId;
    uint32_t       deviceId;
    const wchar_t* description;
    uint16_t       driverVersion[4];

    // As DXGI reports it (CheckInterfaceSupport).
    int64_t packedDriverVersion() const {
      return int64_t(driverVersion[0]) << 48 | int64_t(driverVersion[1]) << 32
           | int64_t(driverVersion[2]) << 16 | int64_t(driverVersion[3]);
    }
  };

  static const GpuPreset kGpuPresets[] = {
    { "nvidia", 0x10DE, 0x2503, L"NVIDIA GeForce RTX 3060", { 32, 0, 15, 6094 } },
    { "amd", 0x1002, 0x73DF, L"AMD Radeon RX 6700 XT", { 31, 0, 24033, 1003 } },
    { "intel", 0x8086, 0x56A0, L"Intel(R) Arc(TM) A770 Graphics", { 32, 0, 101, 6083 } },
  };

  static void readGpuSpoof(const Config& config, UserOverrides& overrides) {
    const std::string vendor = config.getString("dxgi.spoofVendor", "off");
    for (const GpuPreset& preset : kGpuPresets) {
      if (vendor != preset.name)
        continue;
      overrides.spoofVendorId = preset.vendorId;
      overrides.spoofDeviceId = preset.deviceId;
      overrides.spoofDescription = preset.description;
      overrides.spoofDriverVersion = preset.packedDriverVersion();
    }
    // Ids in hex, with or without 0x.
    auto hex = [&](const char* key) {
      const std::string text = config.getString(key, "");
      char* end = nullptr;
      const unsigned long value = std::strtoul(text.c_str(), &end, 16);
      return text.empty() || *end ? 0u : uint32_t(value & 0xFFFF);
    };
    if (const uint32_t vendorId = hex("dxgi.customVendorId")) {
      overrides.spoofVendorId = vendorId;
      // A maker's driver numbering for its vendor id: games read their
      // version number out of it.
      overrides.spoofDriverVersion = 0;
      for (const GpuPreset& preset : kGpuPresets) {
        if (preset.vendorId == vendorId)
          overrides.spoofDriverVersion = preset.packedDriverVersion();
      }
    }
    if (const uint32_t deviceId = hex("dxgi.customDeviceId"))
      overrides.spoofDeviceId = deviceId;
    const std::string description = config.getText("dxgi.customDeviceDesc", "");
#ifdef _WIN32
    if (!description.empty()) {
      wchar_t wide[128] = {};
      if (MultiByteToWideChar(CP_UTF8, 0, description.c_str(), -1, wide, 127))
        overrides.spoofDescription = wide;
    }
#endif
  }

  UserOverrides UserOverrides::fromConfig(const Config& config) {
    UserOverrides overrides;
    // 0 turns anisotropic filtering off, 1-16 force that many samples.
    overrides.samplerAnisotropy = clampSetting(config.getInt("d3d11.samplerAnisotropy", -1), 0, 16);
    // DXGI accepts sync intervals 0-4.
    overrides.syncInterval = clampSetting(config.getInt("dxgi.syncInterval", -1), 0, 4);
    // At least one frame is always queued; DXGI allows up to 16.
    overrides.maxFrameLatency = clampSetting(config.getInt("dxgi.maxFrameLatency", -1), 1, 16);
    // Sample counts D3D11 hardware offers.
    const int32_t samples = config.getInt("d3d11.msaaSamples", 0);
    overrides.msaaSamples = (samples == 2 || samples == 4 || samples == 8 || samples == 16) ? samples : 0;
    overrides.msaaForce = overrides.msaaSamples > 1 && config.getBool("d3d11.msaaForce", false);
    overrides.msaaScope = config.getString("d3d11.msaaScope", "geometry") == "all" ? MsaaScope::All : MsaaScope::Geometry;
    if (config.getString("d3d11.sgssaa", "") == "alpha")
      overrides.sgssaa = SgssaaMode::Alpha;
    else
      overrides.sgssaa = config.getBool("d3d11.sgssaa", false) ? SgssaaMode::Full : SgssaaMode::Off;
    // Supersampling looks sharper with a negative bias (-0.5 * log2 of the
    // sample count), which is left to the user: with per-sample shading it
    // costs the NVIDIA driver several milliseconds of CPU time per frame.
    overrides.samplerLodBias = config.getFloat("d3d11.samplerLodBias", 0.0f);
    overrides.asyncShaders = config.getBool("d3d11.asyncShaders", false);
    overrides.shaderPrecompile = config.getBool("d3d11.shaderPrecompile", true);
    const std::string upscaler = config.getString("d3d11.upscaler", "off");
    overrides.upscaler = upscaler == "dlss" ? 1u : upscaler == "fsr" ? 2u : upscaler == "xess" ? 3u : 0u;
    overrides.upscalerSharpness = (std::min)((std::max)(config.getFloat("d3d11.upscalerSharpness", 0.0f), 0.0f), 1.0f);
    overrides.upscalerShader = std::strtoull(config.getString("d3d11.upscalerShader", "0").c_str(), nullptr, 16);
    overrides.upscalerJitterScale[0] = config.getFloat("d3d11.upscalerJitterScaleX", 1.0f);
    overrides.upscalerJitterScale[1] = config.getFloat("d3d11.upscalerJitterScaleY", 1.0f);
    overrides.upscalerAuto = config.getBool("d3d11.upscalerAuto", false);
    overrides.upscalerInject = config.getBool("d3d11.upscalerInject", false);
    overrides.upscalerBuiltin = config.getBool("d3d11.upscalerBuiltin", true);
    overrides.spatialUpscale = config.getString("dxgi.upscale", "off") == "spatial";
    overrides.spatialSharpness = (std::min)((std::max)(config.getFloat("dxgi.upscaleSharpness", 0.2f), 0.0f), 1.0f);
    overrides.renderScale = (std::min)((std::max)(config.getFloat("dxgi.renderScale", 1.0f), 0.33f), 1.0f);
    overrides.relaxedUavBarriers = config.getBool("d3d11.relaxedUavBarriers", relaxedUavBarriersByDefault());
    overrides.barrierHoisting = config.getBool("d3d12.barrierHoisting", true);
    overrides.pixelOutputMasks = config.getBool("d3d12.pixelOutputMasks", true);
    overrides.adapter = config.getString("dxgi.adapter", "");
    overrides.directBackBuffer = config.getBool("dxgi.directBackBuffer", true);
    {
      const std::string emulation = config.getString("d3d9.floatEmulation", "auto");
      overrides.d3d9FloatEmulation = emulation == "strict" || emulation == "true" || emulation == "on" || emulation == "1" ? 1
        : emulation == "false" || emulation == "off" || emulation == "0" ? 0 : -1;
      overrides.d3d9DxupCompiler = config.getString("d3d9.shaderCompiler", "dxup") != "fxc";
      const std::string nanDepth = config.getString("d3d9.nanDepth", "auto");
      overrides.d3d9NanDepth = nanDepth == "zero" || nanDepth == "true" || nanDepth == "1" ? 1
        : nanDepth == "discard" || nanDepth == "false" || nanDepth == "0" ? 0 : -1;
    }
    readGpuSpoof(config, overrides);
    {
      const std::string lazy = config.getString("d3d12.lazyTargets", "auto");
      overrides.lazyTargets = lazy == "true" || lazy == "on" || lazy == "1" ? 1
        : lazy == "false" || lazy == "off" || lazy == "0" ? 0 : -1;
    }
    return overrides;
  }

  const UserOverrides& UserOverrides::get() {
    static const UserOverrides overrides = [] {
      UserOverrides result = fromConfig(Config::get());
      logMessage(LogLevel::Info, "dxup.conf: settings from %s", configSource().c_str());
      logMessage(LogLevel::Info,
        "dxup.conf: samplerAnisotropy %d, samplerLodBias %.2f, msaaSamples %d, msaaForce %d, msaaScope %s, sgssaa %s, syncInterval %d, maxFrameLatency %d, asyncShaders %d, shaderPrecompile %d, upscaler %u (sharpness %.2f, shader %016llx, auto %d), spatial upscale %d (sharpness %.2f, render scale %.2f), relaxedUavBarriers %d",
        result.samplerAnisotropy, result.samplerLodBias, result.msaaSamples, result.msaaForce ? 1 : 0,
        result.msaaScope == MsaaScope::All ? "all" : "geometry",
        result.sgssaa == SgssaaMode::Full ? "full" : result.sgssaa == SgssaaMode::Alpha ? "alpha" : "off",
        result.syncInterval, result.maxFrameLatency, result.asyncShaders ? 1 : 0, result.shaderPrecompile ? 1 : 0,
        result.upscaler, result.upscalerSharpness, static_cast<unsigned long long>(result.upscalerShader),
        result.upscalerAuto ? 1 : 0, result.spatialUpscale ? 1 : 0, result.spatialSharpness, result.renderScale,
        result.relaxedUavBarriers ? 1 : 0);
#ifdef _WIN32
      if (result.spoofVendorId || result.spoofDeviceId || !result.spoofDescription.empty()) {
        char description[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, result.spoofDescription.c_str(), -1, description, sizeof(description) - 1, nullptr, nullptr);
        logMessage(LogLevel::Info, "dxup.conf: the application sees GPU %04x:%04x \"%s\", driver %llu.%llu.%llu.%llu (0: real)",
          result.spoofVendorId, result.spoofDeviceId, description,
          static_cast<unsigned long long>(result.spoofDriverVersion >> 48 & 0xFFFF),
          static_cast<unsigned long long>(result.spoofDriverVersion >> 32 & 0xFFFF),
          static_cast<unsigned long long>(result.spoofDriverVersion >> 16 & 0xFFFF),
          static_cast<unsigned long long>(result.spoofDriverVersion & 0xFFFF));
      }
#endif
      return result;
    }();
    return overrides;
  }

}
