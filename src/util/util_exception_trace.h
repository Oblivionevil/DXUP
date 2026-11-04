#pragma once

// What DXUP's logs say about exceptions. It only observes: faults in the DLL
// itself are logged with their location, and with dxup_exception_trace.txt
// next to the DLL every first-chance exception of the process is logged with
// a short call stack (diagnostics for crashes that happen in the application
// after a DXUP call went wrong). Handling is always left to the application.

#include "util_paths.h"
#include "util_types.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace dxup {

  namespace exception_trace {

    inline HMODULE g_module = nullptr;   // the DLL whose own faults are logged
    inline bool    g_trace = false;      // dxup_exception_trace.txt, read at install
    inline PVOID   g_handler = nullptr;

    inline void formatModuleAddress(const void* address, char* out, size_t outSize) {
      HMODULE module = nullptr;
      char path[MAX_PATH] = "?";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(address), &module)) {
        GetModuleFileNameA(module, path, MAX_PATH);
      }
      const char* name = strrchr(path, '\\');
      name = name ? name + 1 : path;
      sprintf_s(out, outSize, "%s+0x%llx", name,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    }

    __declspec(noinline) inline void traceException(PEXCEPTION_POINTERS pExInfo) {
      const DWORD code = pExInfo->ExceptionRecord->ExceptionCode;
      void* addr = pExInfo->ExceptionRecord->ExceptionAddress;
      // Thread naming and debug output are exceptions by design.
      if (code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A)
        return;
      static volatile LONG s_traced = 0;
      if (InterlockedIncrement(&s_traced) > 500)
        return;
      char where[MAX_PATH + 32];
      formatModuleAddress(addr, where, sizeof(where));
      std::string stack;
      void* frames[24] = {};
      const USHORT frameCount = CaptureStackBackTrace(0, 24, frames, nullptr);
      for (USHORT i = 0; i < frameCount; i++) {
        char frame[MAX_PATH + 32];
        formatModuleAddress(frames[i], frame, sizeof(frame));
        stack += "\n    ";
        stack += frame;
      }
      ULONG_PTR info0 = pExInfo->ExceptionRecord->NumberParameters > 0 ? pExInfo->ExceptionRecord->ExceptionInformation[0] : 0;
      ULONG_PTR info1 = pExInfo->ExceptionRecord->NumberParameters > 1 ? pExInfo->ExceptionRecord->ExceptionInformation[1] : 0;
      // The top of the stack: a call through a bad pointer leaves the caller's
      // arguments (the object of a COM call among them) there.
      char top[256] = {};
      if (pExInfo->ContextRecord) {
    #if defined(_M_IX86)
        const auto* sp = reinterpret_cast<const ULONG_PTR*>(pExInfo->ContextRecord->Esp);
    #else
        const auto* sp = reinterpret_cast<const ULONG_PTR*>(pExInfo->ContextRecord->Rsp);
    #endif
        ULONG_PTR words[4] = {};
        if (!IsBadReadPtr(sp, sizeof(words)))
          std::memcpy(words, sp, sizeof(words));
        ULONG_PTR object[4] = {};
        const auto* objectPtr = reinterpret_cast<const ULONG_PTR*>(words[1]);
        if (objectPtr && !IsBadReadPtr(objectPtr, sizeof(object)))
          std::memcpy(object, objectPtr, sizeof(object));
        snprintf(top, sizeof(top), "\n    stack %llx %llx %llx %llx, [sp+1] -> %llx %llx %llx %llx",
          static_cast<unsigned long long>(words[0]), static_cast<unsigned long long>(words[1]),
          static_cast<unsigned long long>(words[2]), static_cast<unsigned long long>(words[3]),
          static_cast<unsigned long long>(object[0]), static_cast<unsigned long long>(object[1]),
          static_cast<unsigned long long>(object[2]), static_cast<unsigned long long>(object[3]));
      }
      logMessage(LogLevel::Error, "exception trace: 0x%08X at %s tid=%lu info=%llx,%llx%s%s",
        code, where, static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long long>(info0), static_cast<unsigned long long>(info1), top, stack.c_str());
    }

    __declspec(noinline) inline void logOwnFault(PEXCEPTION_POINTERS pExInfo) {
      const DWORD code = pExInfo->ExceptionRecord->ExceptionCode;
      void* addr = pExInfo->ExceptionRecord->ExceptionAddress;
      HMODULE module = nullptr;
      if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCSTR>(addr), &module)
       || module != g_module)
        return;
      char where[MAX_PATH + 32];
      formatModuleAddress(addr, where, sizeof(where));
      logMessage(LogLevel::Error, "DXUP: exception 0x%08X at %s tid=%lu", code, where,
        static_cast<unsigned long>(GetCurrentThreadId()));
    }

    // The handler itself keeps a small frame, and the logging (a few KB of
    // stack) runs only where the stack has room: games raise exceptions on
    // threads that are nearly out of it (Just Cause 2 at start), and a fault in
    // a vectored handler ends the process.
    inline LONG WINAPI vectoredHandler(PEXCEPTION_POINTERS pExInfo) {
      if (!pExInfo || !pExInfo->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
      const DWORD code = pExInfo->ExceptionRecord->ExceptionCode;
      if (code == EXCEPTION_STACK_OVERFLOW || code == STATUS_GUARD_PAGE_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
      ULONG_PTR low = 0, high = 0;
      GetCurrentThreadStackLimits(&low, &high);
      const ULONG_PTR here = reinterpret_cast<ULONG_PTR>(&low);
      if (here <= low || here - low < 64 * 1024)
        return EXCEPTION_CONTINUE_SEARCH;
      if (g_trace)
        traceException(pExInfo);
      else if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_INT_DIVIDE_BY_ZERO
            || code == EXCEPTION_ILLEGAL_INSTRUCTION)
        logOwnFault(pExInfo);
      return EXCEPTION_CONTINUE_SEARCH;
    }

  }

  // module: the DLL this is built into.
  inline void installExceptionTrace(HMODULE module) {
    exception_trace::g_module = module;
    exception_trace::g_trace = dxupSiblingExists("dxup_exception_trace.txt");
    exception_trace::g_handler = AddVectoredExceptionHandler(1, exception_trace::vectoredHandler);
  }

  // Unloaded with FreeLibrary: the handler's code goes with the DLL.
  inline void removeExceptionTrace() {
    if (exception_trace::g_handler) {
      RemoveVectoredExceptionHandler(exception_trace::g_handler);
      exception_trace::g_handler = nullptr;
    }
  }

}
