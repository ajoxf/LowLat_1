// Pre-allocated, fixed-capacity object pool.
//
// All storage is reserved up front at construction. allocate() and
// deallocate() are O(1) free-list operations with zero heap traffic on the hot
// path. Pool exhaustion is reported (never a silent crash) by returning
// nullptr; callers on the critical path are expected to treat that as CRITICAL.
#ifndef HFT_COMMON_MEMORY_POOL_HPP_
#define HFT_COMMON_MEMORY_POOL_HPP_

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#include "common/compiler.hpp"

namespace hft {

template <typename T>
class MemoryPool {
 public:
  explicit MemoryPool(size_t capacity)
      : capacity_(capacity),
        storage_(static_cast<Slot*>(::operator new(sizeof(Slot) * capacity))),
        free_head_(nullptr),
        free_count_(capacity),
        exhaustion_count_(0) {
    // Thread the slots onto a singly-linked free list.
    for (size_t i = 0; i < capacity_; ++i) {
      Slot* slot = &storage_[i];
      slot->next = free_head_;
      free_head_ = slot;
    }
  }

  ~MemoryPool() { ::operator delete(storage_); }

  MemoryPool(const MemoryPool&) = delete;
  MemoryPool& operator=(const MemoryPool&) = delete;

  // Pops a slot from the free list and constructs a T in place. Returns
  // nullptr on exhaustion (and bumps the exhaustion counter for diagnostics).
  template <typename... Args>
  HFT_ALWAYS_INLINE T* allocate(Args&&... args) {
    if (HFT_UNLIKELY(free_head_ == nullptr)) {
      ++exhaustion_count_;
      return nullptr;
    }
    Slot* slot = free_head_;
    free_head_ = slot->next;
    --free_count_;
    return ::new (&slot->object) T(static_cast<Args&&>(args)...);
  }

  // Destroys the object and returns its slot to the free list.
  HFT_ALWAYS_INLINE void deallocate(T* ptr) {
    if (HFT_UNLIKELY(ptr == nullptr)) {
      return;
    }
    ptr->~T();
    Slot* slot = reinterpret_cast<Slot*>(ptr);
    slot->next = free_head_;
    free_head_ = slot;
    ++free_count_;
  }

  size_t capacity() const { return capacity_; }
  size_t available() const { return free_count_; }
  size_t in_use() const { return capacity_ - free_count_; }
  uint64_t exhaustion_count() const { return exhaustion_count_; }

 private:
  // A slot holds either a live object or a free-list link. Storage is raw so
  // that no T constructor runs until allocate().
  union Slot {
    Slot* next;
    alignas(T) unsigned char object[sizeof(T)];
  };

  size_t capacity_;
  Slot* storage_;
  Slot* free_head_;
  size_t free_count_;
  uint64_t exhaustion_count_;
};

}  // namespace hft

#endif  // HFT_COMMON_MEMORY_POOL_HPP_
