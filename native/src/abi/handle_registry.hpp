// Maps opaque ABI handle values to live engines.
//
// * Handle values are unique 64-bit ids, never reused, so a stale handle can
//   never alias a newer engine (no ABA through address reuse).
// * Lookups return a shared_ptr, so an engine stays alive until every call
//   that already resolved it has returned — even if another thread destroys
//   the handle concurrently. New lookups after destroy fail cleanly.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace mixit {

template <typename T>
class HandleRegistry {
public:
    std::uint64_t add(std::shared_ptr<T> object) {
        std::lock_guard lock(mutex_);
        const std::uint64_t id = next_id_;
        next_id_ += 2;  // odd ids only: never 0 and never a plausible aligned pointer
        objects_.emplace(id, std::move(object));
        return id;
    }

    std::shared_ptr<T> find(std::uint64_t id) const {
        std::lock_guard lock(mutex_);
        const auto it = objects_.find(id);
        return it == objects_.end() ? nullptr : it->second;
    }

    std::shared_ptr<T> remove(std::uint64_t id) {
        std::lock_guard lock(mutex_);
        const auto it = objects_.find(id);
        if (it == objects_.end()) return nullptr;
        std::shared_ptr<T> object = std::move(it->second);
        objects_.erase(it);
        return object;
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return objects_.size();
    }

private:
    mutable std::mutex mutex_;
    std::uint64_t next_id_ = 0x4D49'5801;  // arbitrary odd start
    std::unordered_map<std::uint64_t, std::shared_ptr<T>> objects_;
};

}  // namespace mixit
