#pragma once

// ============================================================
// DXUP Utility Types
//
// Common type definitions and platform abstractions.
// ============================================================

#include <cstdint>
#include <cstddef>
#include <string>

#ifdef _WIN32
#include <guiddef.h>
#endif

#if defined(_M_IX86)
#include <intrin.h>
#endif

#ifdef _WIN32
  #define DXUP_EXPORT __declspec(dllexport)
  #define DXUP_IMPORT __declspec(dllimport)
#else
  #define DXUP_EXPORT __attribute__((visibility("default")))
  #define DXUP_IMPORT
#endif

#ifdef DXUP_BUILD_DLL
  #define DXUP_API DXUP_EXPORT
#else
  #define DXUP_API DXUP_IMPORT
#endif

namespace dxup {

  // hash * the FNV-1a 64-bit prime (2^40 + 0x1b3). The same value
  // everywhere, in 32-bit halves on x86: a 64-bit multiply is a call to
  // _allmul there (the compiler turns 64-bit shifts and adds back into
  // one), and hashes over shader bytecode, descriptors and tracked buffers
  // run it per byte.
  inline uint64_t fnv64Multiply(uint64_t hash) {
#if defined(_M_IX86)
    const uint32_t low = uint32_t(hash), high = uint32_t(hash >> 32);
    const uint64_t product = __emulu(low, 0x1b3u);
    return uint64_t(uint32_t(product >> 32) + high * 0x1b3u + (low << 8)) << 32 | uint32_t(product);
#else
    return hash * 1099511628211ull;
#endif
  }

  // Logging levels
  enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    None
  };

  // Whether messages of this level reach logdxup.log.
  bool logLevelEnabled(LogLevel level);

  // Writes to logdxup.log next to the DLL. Use the logMessage macro instead.
  void logMessageImpl(LogLevel level, const char* fmt, ...);

  // Flush log file to disk
  void logFlush();

  // Close log file (call on DLL unload)
  void logShutdown();

  // Whether the process is exiting: its other threads are gone, so nothing
  // may wait for them or take locks they held. Objects released then are
  // left to the end of the process.
  bool processExiting();

#ifdef _WIN32
  // Formats a GUID / interface ID for debug logging.
  std::string formatGuid(const GUID& guid);

  // With dxup_hitch_trace.txt next to the DLL, operations that can stall a
  // frame log themselves when they take longer than a few milliseconds.
  bool hitchTraceEnabled();
  // With dxup_trace_cbs.txt next to the DLL, the draw trace also writes the
  // contents of each dispatch's constant buffers to files.
  bool traceConstantBuffersEnabled();
  int64_t hitchTimestamp();

  class HitchScope {
  public:
    explicit HitchScope(const char* name, uint64_t detail = 0)
    : m_name(hitchTraceEnabled() ? name : nullptr), m_detail(detail) {
      if (m_name)
        m_start = hitchTimestamp();
    }
    ~HitchScope();
    HitchScope(const HitchScope&) = delete;
    // What the hitch line names, once the scope knows which way it went.
    void rename(const char* name) {
      if (m_name)
        m_name = name;
    }
    HitchScope& operator=(const HitchScope&) = delete;

  private:
    const char* m_name;
    uint64_t    m_detail;
    int64_t     m_start = 0;
  };
#endif

}

// Checks the level before evaluating any argument, so disabled messages
// cost nothing even when their arguments format strings (formatGuid etc.).
#define logMessage(level, ...)                         \
  do {                                                 \
    if (::dxup::logLevelEnabled(level))                \
      ::dxup::logMessageImpl(level, __VA_ARGS__);      \
  } while (0)
