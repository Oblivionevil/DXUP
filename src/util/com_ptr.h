#pragma once

#ifdef _WIN32

#include <unknwn.h>

namespace dxup {

  template<typename T>
  class ComPtr {
  public:
    ComPtr() = default;

    ComPtr(std::nullptr_t) { }

    ComPtr(T* ptr)
    : m_ptr(ptr) {
      internalAddRef();
    }

    ComPtr(const ComPtr& other)
    : m_ptr(other.m_ptr) {
      internalAddRef();
    }

    ComPtr(ComPtr&& other) noexcept
    : m_ptr(other.detach()) {
    }

    ~ComPtr() {
      internalRelease();
    }

    ComPtr& operator=(std::nullptr_t) {
      reset();
      return *this;
    }

    // The new pointer is referenced before the old one is released: the
    // old object may hold the only reference to the new one.
    ComPtr& operator=(T* ptr) {
      if (m_ptr != ptr) {
        if (ptr)
          ptr->AddRef();
        internalRelease();
        m_ptr = ptr;
      }
      return *this;
    }

    ComPtr& operator=(const ComPtr& other) {
      if (this != &other)
        *this = other.m_ptr;
      return *this;
    }

    // Taken before the old object goes, which may own other.
    ComPtr& operator=(ComPtr&& other) noexcept {
      if (this != &other) {
        T* ptr = other.detach();
        internalRelease();
        m_ptr = ptr;
      }
      return *this;
    }

    T* Get() const {
      return m_ptr;
    }

    T** GetAddressOf() {
      return &m_ptr;
    }

    T* const* GetAddressOf() const {
      return &m_ptr;
    }

    T** ReleaseAndGetAddressOf() {
      reset();
      return &m_ptr;
    }

    void Attach(T* ptr) {
      internalRelease();
      m_ptr = ptr;
    }

    T* Detach() {
      return detach();
    }

    void Reset() {
      reset();
    }

    T* operator->() const {
      return m_ptr;
    }

    T& operator*() const {
      return *m_ptr;
    }

    explicit operator bool() const {
      return m_ptr != nullptr;
    }

    template<typename U>
    HRESULT As(ComPtr<U>* other) const {
      if (!other)
        return E_POINTER;

      if (!m_ptr) {
        other->Reset();
        return E_NOINTERFACE;
      }

      return m_ptr->QueryInterface(
        __uuidof(U),
        reinterpret_cast<void**>(other->ReleaseAndGetAddressOf()));
    }

  private:
    void internalAddRef() {
      if (m_ptr)
        m_ptr->AddRef();
    }

    void internalRelease() {
      T* ptr = m_ptr;
      m_ptr = nullptr;

      if (ptr)
        ptr->Release();
    }

    T* detach() {
      T* ptr = m_ptr;
      m_ptr = nullptr;
      return ptr;
    }

    void reset() {
      internalRelease();
    }

    T* m_ptr = nullptr;
  };

}

template<typename T>
void** IID_PPV_ARGS_Helper(dxup::ComPtr<T>* ptr) {
  return reinterpret_cast<void**>(ptr->ReleaseAndGetAddressOf());
}

#endif