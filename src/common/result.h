// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <utility>
#include <variant>

#include "common/error.h"

namespace lr {

template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : v_(std::in_place_index<1>, std::move(error)) {}

  explicit operator bool() const { return v_.index() == 0; }
  bool ok() const { return v_.index() == 0; }
  T& value() { return std::get<0>(v_); }
  const T& value() const { return std::get<0>(v_); }
  const Error& error() const { return std::get<1>(v_); }

 private:
  std::variant<T, Error> v_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : err_(std::move(error)) {}

  explicit operator bool() const { return !err_.has_value(); }
  bool ok() const { return !err_.has_value(); }
  const Error& error() const { return *err_; }

 private:
  std::optional<Error> err_;
};

}  // namespace lr

#define LR_DETAIL_CAT2(a, b) a##b
#define LR_DETAIL_CAT(a, b) LR_DETAIL_CAT2(a, b)

// Evaluates expr (a Result<...>); on failure returns its Error from the enclosing function.
#define LR_TRY(expr)                                         \
  do {                                                       \
    auto&& lr_try_r_ = (expr);                               \
    if (!lr_try_r_) return ::lr::Error(lr_try_r_.error());   \
  } while (0)

#define LR_DETAIL_TRY_ASSIGN(tmp, var, expr)                 \
  auto&& tmp = (expr);                                       \
  if (!tmp) return ::lr::Error(tmp.error());                 \
  auto var = std::move(tmp.value())

// (Uses __LINE__: __COUNTER__ trips -Wpedantic.) Declares `var` holding the value of expr, or returns the Error on failure.
#define LR_TRY_ASSIGN(var, expr) \
  LR_DETAIL_TRY_ASSIGN(LR_DETAIL_CAT(lr_try_assign_, __LINE__), var, expr)
