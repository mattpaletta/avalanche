//
// Created by Kirill on 25/01/18.
//

#ifndef AVALANCHE_MEMORY_MANAGER_H
#define AVALANCHE_MEMORY_MANAGER_H

#include <memory>

#include "CL_cust/cl2.hpp"

#include "avalanche/CLBufferPool.h"

namespace avalanche {

using DeviceIndex = std::size_t;

class CLMemoryManager {
    /*
     * The manager should track buffers for all devices.
     */
public:
    CLMemoryManager() :_device_counter{0} {}
    ~CLMemoryManager();
    void init_for_all_gpus();
    void init_for_all(cl_device_type device_type);
    BufferPoolRef buffer_pool(DeviceIndex idx);
    std::size_t num_devices() const;
    static std::shared_ptr<CLMemoryManager> get_default();

private:
    std::vector<BufferPoolRef> _buffer_pools;
    std::size_t _device_counter;
};

} //namespace

#endif //AVALANCHE_MEMORY_MANAGER_H