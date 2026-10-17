#pragma once

// Resources shared between devices. Every device submits on a queue of its
// own; D3D11 orders the work of devices on a shared resource as the kernel
// orders shared allocations. A submission that uses a shared resource waits
// for the last submission of another device that used it.

#ifdef _WIN32

#include <d3d12.h>
#include "../util/com_ptr.h"

#include <cstdint>
#include <mutex>

namespace dxup::d3d12 {

  struct D3D12SharedSync {
    std::mutex          mutex;
    // The queue fence of the last submission that used the resource, and
    // the value it signals after that submission.
    ComPtr<ID3D12Fence> fence;
    uint64_t            value = 0;
  };

}

#endif
