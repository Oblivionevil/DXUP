#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <thread>
#endif

namespace dxup {

#ifdef _WIN32
  // A thread with a stack of its own size. std::thread takes the
  // executable's default, which old games set as low as 64 KB: too little
  // for the drivers our threads call into.
  class Thread {
  public:
    static constexpr size_t kStackSize = 2u << 20;

    Thread() = default;

    template <typename Fn>
    explicit Thread(Fn&& fn) {
      auto task = std::make_unique<std::function<void()>>(std::forward<Fn>(fn));
      m_handle = CreateThread(nullptr, kStackSize, &Thread::entry, task.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
      if (!m_handle)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateThread");
      task.release();
    }

    Thread(Thread&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) { }

    Thread& operator=(Thread&& other) noexcept {
      if (this != &other) {
        if (joinable())
          std::terminate();
        m_handle = std::exchange(other.m_handle, nullptr);
      }
      return *this;
    }

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    ~Thread() {
      if (joinable())
        std::terminate();
    }

    bool joinable() const { return m_handle != nullptr; }

    void join() {
      WaitForSingleObject(m_handle, INFINITE);
      CloseHandle(std::exchange(m_handle, nullptr));
    }

    void detach() {
      CloseHandle(std::exchange(m_handle, nullptr));
    }

  private:
    static DWORD WINAPI entry(void* parameter) {
      std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()>*>(parameter));
      (*task)();
      return 0;
    }

    HANDLE m_handle = nullptr;
  };
#else
  using Thread = std::thread;
#endif

}
