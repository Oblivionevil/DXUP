#pragma once

#ifdef _WIN32
#include <windows.h>

#include <cstring>
#include <string>

namespace dxup {

  // The path of a file next to the DLL this code is in (each DLL has its
  // own copy); the name alone when the DLL's path is unknown.
  inline std::string dxupSiblingPath(const char* name) {
    char path[MAX_PATH] = {};
    HMODULE module = nullptr;
    GetModuleHandleExA(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCSTR>(&dxupSiblingPath), &module);
    if (module)
      GetModuleFileNameA(module, path, MAX_PATH);
    char* lastSlash = strrchr(path, '\\');
    if (lastSlash)
      *(lastSlash + 1) = '\0';
    else
      path[0] = '\0';
    return std::string(path) + name;
  }

  // The same into a buffer; false when it does not fit.
  inline bool dxupSiblingPathTo(char* path, size_t pathSize, const char* name) {
    if (!path || !pathSize || !name)
      return false;
    const std::string full = dxupSiblingPath(name);
    if (full.size() >= pathSize) {
      path[0] = '\0';
      return false;
    }
    std::memcpy(path, full.c_str(), full.size() + 1);
    return true;
  }

  // A wide path (any characters), empty when the DLL's path is unknown.
  inline std::wstring dxupSiblingPathW(const wchar_t* name) {
    wchar_t path[MAX_PATH] = {};
    HMODULE module = nullptr;
    GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&dxupSiblingPathW), &module);
    const DWORD length = module ? GetModuleFileNameW(module, path, MAX_PATH) : 0;
    wchar_t* lastSlash = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!lastSlash)
      return std::wstring();
    lastSlash[1] = L'\0';
    return std::wstring(path) + name;
  }

  // Whether a file (a dxup_*.txt marker) lies next to the DLL.
  inline bool dxupSiblingExists(const char* name) {
    const DWORD attributes = GetFileAttributesA(dxupSiblingPath(name).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
  }

}
#endif
