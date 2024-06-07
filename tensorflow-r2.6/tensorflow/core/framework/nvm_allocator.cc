#include <libpmem.h>
#include <iostream>
#include <string>
#include <mutex>
#include <unordered_map>

#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/platform/mem.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/framework/tracking_allocator.h"
#include "tensorflow/core/framework/nvm_allocator.h"

struct PersistenceHeader {
    size_t size;
    // other fields
};

// Initialize static members outside the class
NVMAllocator* NVMAllocator::instance_ = nullptr;
char* NVMAllocator::pmem_base_ = nullptr;
size_t NVMAllocator::mapped_len_ = 0;
size_t NVMAllocator::offset_ = 0;
size_t NVMAllocator::reserved_meta_offset_ = 0;
std::mutex NVMAllocator::mu_;
tensorflow::AllocatorStats NVMAllocator::stats_;
std::unordered_map<void*, size_t> NVMAllocator::allocation_map_;

NVMAllocator::NVMAllocator() {
    Init();
}

NVMAllocator::~NVMAllocator() {
    if (pmem_base_) {
        pmem_unmap(pmem_base_, mapped_len_);
    }
}

std::string NVMAllocator::Name() {
    return "NVMAllocator";
}

void NVMAllocator::Init() {
    const std::string file_path = "/pmem/zhr/NCheck";
    size_t pmem_len = 1024ULL * 1024 * 1024 * 200;  
    int is_pmem;

    if (pmem_base_ == nullptr) {
        pmem_base_ = (char*)pmem_map_file(file_path.c_str(), pmem_len, PMEM_FILE_CREATE, 0666, &mapped_len_, &is_pmem);
        if (pmem_base_ == nullptr) {
            std::cerr << "Failed to map persistent memory file: " << file_path << std::endl;
            std::exit(EXIT_FAILURE);
        }

        if (!is_pmem) {
            std::cerr << "Warning: Memory mapped is not on a PMem, but continuing anyway." << std::endl;
        }
        //offset_ = sizeof(PersistenceHeader);
    }
}

NVMAllocator* NVMAllocator::GetInstance() {
    static std::mutex local_mutex;
    std::lock_guard<std::mutex> lock(local_mutex);
    if (instance_ == nullptr) {
        instance_ = new NVMAllocator();
    }
    return instance_;
}

void* NVMAllocator::AllocateRaw(size_t alignment, size_t num_bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    size_t current_offset = (offset_ + alignment - 1) & ~(alignment - 1);
    if (current_offset + num_bytes > mapped_len_) {
        return nullptr;  // Out of memory
    }

    void* ptr = pmem_base_ + current_offset;
    pmem_persist(ptr, num_bytes);
    offset_ = current_offset + num_bytes;

    allocation_map_[ptr] = num_bytes;
    stats_.bytes_in_use += num_bytes;
    stats_.peak_bytes_in_use = std::max(stats_.peak_bytes_in_use, stats_.bytes_in_use);
    stats_.largest_alloc_size = std::max(stats_.largest_alloc_size, static_cast<tensorflow::int64>(num_bytes));
    stats_.num_allocs++;
    return ptr;
}

void NVMAllocator::ResetOffsetForIteration() {
    std::lock_guard<std::mutex> lock(mu_);
    offset_ = reserved_meta_offset_;  // Reset offset after reserved metadata area
}

void NVMAllocator::DeallocateRaw(void* ptr) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = allocation_map_.find(ptr);
    if (it != allocation_map_.end()) {
        size_t alloc_size = it->second;
        stats_.bytes_in_use -= alloc_size;
        allocation_map_.erase(it);
        // Note: Actual memory is not "freed" in persistent memory
    }
}

size_t NVMAllocator::GetAllocatedSize(const void* ptr) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = allocation_map_.find(const_cast<void*>(ptr));
    if (it != allocation_map_.end()) {
        return it->second;
    }
    return 0;
}

void* NVMAllocator::GetBaseAddress() {
    return pmem_base_;
}

