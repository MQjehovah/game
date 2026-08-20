#pragma once
#include <string>
#include <utility>

namespace neon::core {

template <class T>
class Result {
public:
    static Result Ok(T value) {
        Result r;
        r.ok_ = true;
        r.value_ = std::move(value);
        return r;
    }
    static Result Err(std::string message) {
        Result r;
        r.ok_ = false;
        r.error_ = std::move(message);
        return r;
    }

    bool Ok() const { return ok_; }
    T& Value() { return value_; }
    const T& Value() const { return value_; }
    const std::string& Error() const { return error_; }

private:
    bool ok_ = false;
    T value_{};
    std::string error_;
};

using Status = Result<bool>;

} // namespace neon::core
