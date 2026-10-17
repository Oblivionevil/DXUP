#include "util_types.h"
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dxup {

  static std::mutex g_logMutex;
  static FILE*      g_logFile = nullptr;
  static bool       g_logInitialized = false;

  // Whether a marker file lies next to this DLL.
  static bool dllSiblingExists(const char* fileName) {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    HMODULE hModule = nullptr;
    GetModuleHandleExA(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCSTR>(&dllSiblingExists),
      &hModule);
    if (hModule)
      GetModuleFileNameA(hModule, path, MAX_PATH);
    char* lastSlash = strrchr(path, '\\');
    if (!lastSlash)
      return false;
    *(lastSlash + 1) = '\0';
    strcat_s(path, MAX_PATH, fileName);
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)fileName;
    return false;
#endif
  }

  static LogLevel getMinimumLogLevel() {
    static LogLevel level = [] {
#ifdef _WIN32
      char markerPath[MAX_PATH] = {};
      HMODULE hModule = nullptr;
      GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&getMinimumLogLevel),
        &hModule);

      if (hModule)
        GetModuleFileNameA(hModule, markerPath, MAX_PATH);

      char* lastSlash = strrchr(markerPath, '\\');
      if (lastSlash) {
        *(lastSlash + 1) = '\0';
        strcat_s(markerPath, MAX_PATH, "dxup_info_log.txt");
        DWORD attributes = GetFileAttributesA(markerPath);
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
          return LogLevel::Info;
      }
#endif
      return LogLevel::Warn;
    }();
    return level;
  }

  static void initLogFile() {
    if (g_logInitialized)
      return;
    g_logInitialized = true;

#ifdef _WIN32
    // Place logdxup.log next to the DLL
    char dllPath[MAX_PATH] = {};
    HMODULE hModule = nullptr;
    GetModuleHandleExA(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCSTR>(&initLogFile),
      &hModule);

    if (hModule)
      GetModuleFileNameA(hModule, dllPath, MAX_PATH);

    // Strip filename, append logdxup.log. Each DLL has its own log state,
    // so the others (dxgi.dll, ddraw.dll) write files of their own instead
    // of truncating d3d11's: logdxup_dxgi.log, logdxup_ddraw.log.
    char* lastSlash = strrchr(dllPath, '\\');
    char stem[64] = {};
    strncpy_s(stem, lastSlash ? lastSlash + 1 : dllPath, _TRUNCATE);
    if (char* dot = strrchr(stem, '.'))
      *dot = '\0';
    const bool isD3D11 = !stem[0] || _stricmp(stem, "d3d11") == 0;
    if (lastSlash) {
      *(lastSlash + 1) = '\0';
    } else {
      dllPath[0] = '\0';
    }
    strcat_s(dllPath, MAX_PATH, "logdxup");
    if (!isD3D11) {
      strcat_s(dllPath, MAX_PATH, "_");
      strcat_s(dllPath, MAX_PATH, stem);
    }
    strcat_s(dllPath, MAX_PATH, ".log");

    g_logFile = fopen(dllPath, "w");
#else
    g_logFile = fopen("logdxup.log", "w");
#endif

    if (g_logFile) {
      fprintf(g_logFile, "=== DXUP Log Started ===\n");
      fflush(g_logFile);
    }
  }

  bool logLevelEnabled(LogLevel level) {
    return level >= getMinimumLogLevel();
  }

  bool processExiting() {
#ifdef _WIN32
    using ShutdownInProgress = BOOLEAN (NTAPI*)();
    static const auto shutdownInProgress = reinterpret_cast<ShutdownInProgress>(
      GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlDllShutdownInProgress"));
    return shutdownInProgress && shutdownInProgress();
#else
    return false;
#endif
  }

  void logMessageImpl(LogLevel level, const char* fmt, ...) {
    if (level < getMinimumLogLevel()) return;

    const char* prefix = "";
    switch (level) {
      case LogLevel::Trace: prefix = "[TRACE] "; break;
      case LogLevel::Debug: prefix = "[DEBUG] "; break;
      case LogLevel::Info:  prefix = "[INFO]  "; break;
      case LogLevel::Warn:  prefix = "[WARN]  "; break;
      case LogLevel::Error: prefix = "[ERROR] "; break;
      default: return;
    }

    std::lock_guard<std::mutex> lock(g_logMutex);

    initLogFile();

    // Get timestamp
    time_t now = time(nullptr);
    struct tm timeInfo = {};
#ifdef _WIN32
    localtime_s(&timeInfo, &now);
#else
    localtime_r(&now, &timeInfo);
#endif
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeInfo);

    // Format the message
    va_list args;
    va_start(args, fmt);

    if (g_logFile) {
      fprintf(g_logFile, "[%s] %s", timeBuf, prefix);
      vfprintf(g_logFile, fmt, args);
      fprintf(g_logFile, "\n");
      // Only fflush for warnings and above to minimize I/O overhead; with
      // info logging (diagnostics) everything, the process may be killed.
      if (level >= LogLevel::Warn || getMinimumLogLevel() <= LogLevel::Info)
        fflush(g_logFile);
    }

    va_end(args);

    // Only write to stderr and debug output for errors
    if (level >= LogLevel::Error) {
      va_start(args, fmt);
      fprintf(stderr, "[%s] %s", timeBuf, prefix);
      vfprintf(stderr, fmt, args);
      fprintf(stderr, "\n");
      va_end(args);

#ifdef _WIN32
      char debugBuf[2048];
      va_start(args, fmt);
      int len = snprintf(debugBuf, sizeof(debugBuf), "DXUP %s", prefix);
      vsnprintf(debugBuf + len, sizeof(debugBuf) - len, fmt, args);
      va_end(args);
      strncat_s(debugBuf, sizeof(debugBuf), "\n", 1);
      OutputDebugStringA(debugBuf);
#endif
    }
  }

  void logFlush() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile)
      fflush(g_logFile);
  }

  void logShutdown() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
      fprintf(g_logFile, "=== DXUP Log Ended ===\n");
      fclose(g_logFile);
      g_logFile = nullptr;
    }
    g_logInitialized = false;
  }

#ifdef _WIN32
  std::string formatGuid(const GUID& guid) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer),
      "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
      static_cast<unsigned long>(guid.Data1),
      guid.Data2,
      guid.Data3,
      guid.Data4[0], guid.Data4[1],
      guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buffer;
  }

  bool traceConstantBuffersEnabled() {
    static const bool enabled = dllSiblingExists("dxup_trace_cbs.txt");
    return enabled;
  }

  bool hitchTraceEnabled() {
    static const bool enabled = dllSiblingExists("dxup_hitch_trace.txt");
    return enabled;
  }

  int64_t hitchTimestamp() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
  }

  HitchScope::~HitchScope() {
    if (!m_name)
      return;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double ms = double(hitchTimestamp() - m_start) * 1000.0 / double(frequency.QuadPart);
    if (ms >= 4.0)
      logMessageImpl(LogLevel::Warn, "hitch: %s took %.1f ms (detail %llu, tid %lu)", m_name, ms,
        static_cast<unsigned long long>(m_detail), GetCurrentThreadId());
  }
#endif

}
