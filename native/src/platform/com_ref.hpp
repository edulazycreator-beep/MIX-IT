// Minimal owning pointer for COM interfaces (avoids a WRL/ATL dependency).
// Windows only.
#pragma once

#if defined(_WIN32)

namespace mixit {

template <typename T>
class ComRef {
public:
    ComRef() = default;
    ~ComRef() { reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ComRef(ComRef&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ComRef& operator=(ComRef&& o) noexcept {
        if (this != &o) {
            reset();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }
    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    T** put() noexcept {
        reset();
        return &ptr_;
    }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    void reset() noexcept {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

}  // namespace mixit

#endif
