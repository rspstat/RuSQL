#pragma once

#include <string>
#include <utility>
#include <variant>

namespace engine {

// Mirrors the `Result<T, String>` convention used throughout rusql-core (Ok = success
// payload, Err = human-readable error message). Kept as an explicit value type rather
// than exceptions so that the ported code can preserve the original's early-return
// control flow (Rust's `?` operator) as literal early-return checks, per the
// "faithful port" migration philosophy.
template <typename T, typename E = std::string>
class Result {
public:
    static Result Ok(T value) { return Result(std::in_place_index<0>, std::move(value)); }
    static Result Err(E error) { return Result(std::in_place_index<1>, std::move(error)); }

    bool is_ok() const noexcept { return data_.index() == 0; }
    bool is_err() const noexcept { return data_.index() == 1; }
    explicit operator bool() const noexcept { return is_ok(); }

    const T& value() const& { return std::get<0>(data_); }
    T& value() & { return std::get<0>(data_); }
    T&& value() && { return std::get<0>(std::move(data_)); }

    const E& error() const& { return std::get<1>(data_); }
    E& error() & { return std::get<1>(data_); }

private:
    template <std::size_t I, typename U>
    Result(std::in_place_index_t<I> idx, U&& v) : data_(idx, std::forward<U>(v)) {}

    std::variant<T, E> data_;
};

// Specialization mirroring Rust's `Result<(), E>` (e.g. Catalog::create_table).
template <typename E>
class Result<void, E> {
public:
    static Result Ok() { return Result(true, E{}); }
    static Result Err(E error) { return Result(false, std::move(error)); }

    bool is_ok() const noexcept { return ok_; }
    bool is_err() const noexcept { return !ok_; }
    explicit operator bool() const noexcept { return ok_; }

    const E& error() const& { return error_; }

private:
    Result(bool ok, E error) : ok_(ok), error_(std::move(error)) {}
    bool ok_;
    E error_;
};

// The dominant shape found across rusql-core: `Result<String, String>`
// (Ok = human-readable success message, Err = human-readable error message).
using StringResult = Result<std::string, std::string>;

} // namespace engine
