#pragma once
// An atomically replaceable immutable snapshot, on every target.
//
// The engine's render layer hands whole configurations to the renderer as
// immutable shared_ptr snapshots (prompt labels, developer weapon assets). On
// Windows those are `std::atomic<std::shared_ptr<T>>`; the PS5 SDK's libc++ has
// no such specialization (`std::atomic<T>` there still requires a trivially
// copyable T), so the console uses a mutex around the same load/store pair.
//
// The two have the same interface for the operations the layer performs, and
// the mutex version is a plain lock: the snapshots are read once per draw at
// most, never on a hot per-vertex path.
#include <memory>
#include <mutex>

namespace DarkRecomp {

#if defined(__cpp_lib_atomic_shared_ptr)

template <class T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;

#else

template <class T>
class AtomicSharedPtr {
public:
    AtomicSharedPtr() = default;
    explicit AtomicSharedPtr(std::shared_ptr<T> value) : value_(std::move(value)) {}

    std::shared_ptr<T> load(std::memory_order = std::memory_order_acquire) const {
        std::lock_guard lock(mutex_);
        return value_;
    }
    void store(std::shared_ptr<T> value, std::memory_order = std::memory_order_release) {
        std::lock_guard lock(mutex_);
        value_ = std::move(value);
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<T> value_;
};

#endif

} // namespace DarkRecomp
