#pragma once

#ifdef _WIN32
#include <windows.h>

#include <share.h>

#include <cstdio>
#include <string>

namespace dxup {

  // The full path of the DLL this code is in (each DLL has its own copy),
  // wide and as long as it is (a game folder may hold any character and be
  // longer than MAX_PATH); empty when unknown.
  inline std::wstring dxupModulePathW() {
    HMODULE module = nullptr;
    GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&dxupModulePathW), &module);
    if (!module)
      return std::wstring();
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
      const DWORD length = GetModuleFileNameW(module, &path[0], static_cast<DWORD>(path.size()));
      if (!length)
        return std::wstring();
      if (length < path.size()) {
        path.resize(length);
        return path;
      }
      // Cut short: a longer buffer (paths end at 32767 characters).
      if (path.size() >= 32768)
        return std::wstring();
      path.resize(path.size() * 2);
    }
  }

  // The DLL's folder with a trailing separator; empty when unknown.
  inline std::wstring dxupModuleFolderW() {
    std::wstring path = dxupModulePathW();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
      return std::wstring();
    path.resize(slash + 1);
    return path;
  }

  // A wide path (any characters, any length); empty when the DLL's path is
  // unknown.
  inline std::wstring dxupSiblingPathW(const wchar_t* name) {
    const std::wstring folder = dxupModuleFolderW();
    return folder.empty() || !name ? std::wstring() : folder + name;
  }

  // An ASCII name (or fopen mode) as a wide string.
  inline std::wstring dxupWiden(const char* text) {
    std::wstring wide;
    for (const char* c = text; c && *c; c++)
      wide += static_cast<wchar_t>(static_cast<unsigned char>(*c));
    return wide;
  }

  // The wide path of a file next to the DLL named in ASCII.
  inline std::wstring dxupSiblingPathW(const char* name) {
    return name ? dxupSiblingPathW(dxupWiden(name).c_str()) : std::wstring();
  }

  // Whether a file (a dxup_*.txt marker, named in ASCII) lies next to the
  // DLL, whatever characters its folder holds.
  inline bool dxupSiblingExists(const char* name) {
    const std::wstring path = dxupSiblingPathW(name);
    const DWORD attributes = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
  }

  // Opens a file next to the DLL (named in ASCII), whatever characters its
  // folder holds; null when that fails.
  inline FILE* dxupOpenSibling(const char* name, const char* mode) {
    const std::wstring path = dxupSiblingPathW(name);
    return path.empty() ? nullptr : _wfsopen(path.c_str(), dxupWiden(mode).c_str(), _SH_DENYNO);
  }

  // Deletes a file next to the DLL (named in ASCII).
  inline void dxupDeleteSibling(const char* name) {
    const std::wstring path = dxupSiblingPathW(name);
    if (!path.empty())
      DeleteFileW(path.c_str());
  }

}
#endif
