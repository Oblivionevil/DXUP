#pragma once

#ifdef _WIN32
#include <windows.h>

#include <cstring>
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

  // A wide path in the code page of the narrow file functions (fopen,
  // CreateFileA); false when a character has none there: it would become
  // '?' or a look-alike (best fit) and name another file.
  inline bool dxupNarrowPath(const std::wstring& wide, std::string& narrow) {
    narrow.clear();
    if (wide.empty())
      return true;
    const UINT codePage = AreFileApisANSI() ? GetACP() : GetOEMCP();
    // UTF-8 as the process's code page holds every character.
    const DWORD flags = codePage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL lossy = FALSE;
    BOOL* lossyOut = codePage == CP_UTF8 ? nullptr : &lossy;
    const int length = WideCharToMultiByte(codePage, flags, wide.data(), static_cast<int>(wide.size()),
      nullptr, 0, nullptr, lossyOut);
    if (length <= 0 || lossy)
      return false;
    narrow.resize(static_cast<size_t>(length));
    WideCharToMultiByte(codePage, flags, wide.data(), static_cast<int>(wide.size()), &narrow[0], length, nullptr, nullptr);
    return true;
  }

  // The DLL's folder for the narrow file functions: as it is, else in its
  // short (8.3) form where the volume has one; empty when neither can name
  // it. The wide functions below take any folder.
  inline std::string dxupModuleFolderA() {
    const std::wstring folder = dxupModuleFolderW();
    std::string narrow;
    if (folder.empty())
      return std::string();
    if (dxupNarrowPath(folder, narrow))
      return narrow;
    const DWORD size = GetShortPathNameW(folder.c_str(), nullptr, 0);
    if (!size)
      return std::string();
    std::wstring shortFolder(size, L'\0');
    const DWORD length = GetShortPathNameW(folder.c_str(), &shortFolder[0], size);
    if (!length || length >= size)
      return std::string();
    shortFolder.resize(length);
    return dxupNarrowPath(shortFolder, narrow) ? narrow : std::string();
  }

  // The path of a file next to the DLL, for the narrow file functions;
  // empty when the folder cannot be named so (see dxupModuleFolderA). Code
  // that can should take dxupSiblingPathW, which names any folder.
  inline std::string dxupSiblingPath(const char* name) {
    const std::string folder = dxupModuleFolderA();
    return folder.empty() || !name ? std::string() : folder + name;
  }

  // The same into a buffer; false when it is unknown or does not fit.
  inline bool dxupSiblingPathTo(char* path, size_t pathSize, const char* name) {
    if (!path || !pathSize || !name)
      return false;
    const std::string full = dxupSiblingPath(name);
    if (full.empty() || full.size() >= pathSize) {
      path[0] = '\0';
      return false;
    }
    std::memcpy(path, full.c_str(), full.size() + 1);
    return true;
  }

  // A wide path (any characters, any length); empty when the DLL's path is
  // unknown.
  inline std::wstring dxupSiblingPathW(const wchar_t* name) {
    const std::wstring folder = dxupModuleFolderW();
    return folder.empty() || !name ? std::wstring() : folder + name;
  }

  // Whether a file (a dxup_*.txt marker, named in ASCII) lies next to the
  // DLL, whatever characters its folder holds.
  inline bool dxupSiblingExists(const char* name) {
    if (!name)
      return false;
    std::wstring wideName;
    for (const char* c = name; *c; c++)
      wideName += static_cast<wchar_t>(static_cast<unsigned char>(*c));
    const std::wstring path = dxupSiblingPathW(wideName.c_str());
    const DWORD attributes = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
  }

}
#endif
