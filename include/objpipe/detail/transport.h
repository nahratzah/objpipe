#ifndef OBJPIPE_DETAIL_TRANSPORT_H
#define OBJPIPE_DETAIL_TRANSPORT_H

#include <memory>
#include <type_traits>
#include <variant>
#include <objpipe/errc.h>

namespace objpipe::detail {


template<typename T>
class transport {
  static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>,
      "Transport-by-value may not be const or volatile.");
  static_assert(!std::is_reference_v<T>,
      "Programmer error: one of the reference specializations is to be selected.");

 public:
  using value_type = T;
  using type = T;

  constexpr transport(std::in_place_index_t<0>, type&& arg)
  noexcept(std::is_nothrow_move_constructible_v<T>)
  : data_(std::in_place_index<0>, std::move(arg))
  {}

  constexpr transport(std::in_place_index_t<0>, const type& arg)
  noexcept(std::is_nothrow_copy_constructible_v<T>)
  : data_(std::in_place_index<0>, arg)
  {}

  constexpr transport(std::in_place_index_t<1>, objpipe_errc e)
  noexcept
  : data_(std::in_place_index<1>, e)
  {}

  friend auto swap(transport& x, transport& y)
  noexcept(std::is_nothrow_move_constructible_v<value_type>
      && std::is_nothrow_swappable_v<value_type>)
  -> void {
    using std::swap;
    swap(x.data_, y.data_);
  }

  auto has_value() const
  noexcept
  -> bool {
    return data_.index() == 0;
  }

  auto value() const &
  noexcept
  -> std::add_lvalue_reference_t<std::add_const_t<T>> {
    assert(has_value());
    return std::get<0>(data_);
  }

  auto value() const &&
  noexcept
  -> std::add_lvalue_reference_t<std::add_const_t<T>> {
    assert(has_value());
    return std::get<0>(data_);
  }

  auto value() &
  noexcept
  -> std::add_lvalue_reference_t<T> {
    assert(has_value());
    return std::get<0>(data_);
  }

  auto value() &&
  noexcept
  -> std::add_rvalue_reference_t<T> {
    assert(has_value());
    return std::get<0>(std::move(data_));
  }

  auto ref()
  noexcept
  -> std::add_lvalue_reference_t<value_type> {
    assert(has_value());
    return std::get<0>(data_);
  }

  auto errc() const
  noexcept
  -> objpipe_errc {
    return (has_value() ? objpipe_errc::success : std::get<1>(data_));
  }

  template<typename... Args>
  auto emplace(std::in_place_index_t<0>, Args&&... args)
  noexcept(std::is_nothrow_constructible_v<T, std::add_rvalue_reference_t<Args>...>)
  -> void {
    data_.template emplace<0>(std::forward<Args>(args)...);
  }

  auto emplace(std::in_place_index_t<1>, objpipe_errc e)
  noexcept
  -> void {
    data_.template emplace<1>(e);
  }

  auto by_value() &&
  noexcept(std::is_nothrow_copy_constructible_v<value_type>)
  -> transport {
    return std::move(*this);
  }

 private:
  std::variant<T, objpipe_errc> data_;
};

template<typename T>
class transport<T&> {
  static_assert(std::is_const_v<T>,
      "Transport-by-reference must be const.");

 public:
  using value_type = std::remove_cv_t<T>;
  using type = T&;

  constexpr transport(std::in_place_index_t<0>, T& v)
  noexcept
  : data_(std::in_place_index<0>, std::addressof(v))
  {}

  constexpr transport(std::in_place_index_t<1>, objpipe_errc e)
  noexcept
  : data_(std::in_place_index<1>, e)
  {}

  friend auto swap(transport& x, transport& y)
  noexcept
  -> void {
    using std::swap;
    swap(x.data_, y.data_);
  }

  auto has_value() const
  noexcept
  -> bool {
    return data_.index() == 0;
  }

  auto value() const
  noexcept
  -> T& {
    assert(has_value());
    return *std::get<0>(data_);
  }

  auto value_ptr() const
  noexcept
  -> T* {
    assert(has_value());
    return std::get<0>(data_);
  }

  auto ref()
  noexcept
  -> type {
    return value();
  }

  auto errc() const
  noexcept
  -> objpipe_errc {
    return (has_value() ? objpipe_errc::success : std::get<1>(data_));
  }

  auto emplace(std::in_place_index_t<0>, T& v)
  noexcept
  -> void {
    data_.template emplace<0>(std::addressof(v));
  }

  auto emplace(std::in_place_index_t<1>, objpipe_errc e)
  noexcept
  -> void {
    data_.template emplace<1>(e);
  }

  operator transport<std::remove_cv_t<std::remove_reference_t<type>>>() &&
  noexcept(std::is_nothrow_copy_constructible_v<value_type>) {
    using rv_type = transport<std::remove_cv_t<std::remove_reference_t<T>>>;

    if (has_value())
      return rv_type(std::in_place_index<0>, std::move(*this).value());
    else
      return rv_type(std::in_place_index<1>, errc());
  }

  auto by_value() &&
  noexcept(std::is_nothrow_copy_constructible_v<value_type>)
  -> transport<value_type> {
    return std::move(*this);
  }

 private:
  std::variant<T*, objpipe_errc> data_;
};

template<typename T>
class transport<T&&> {
  static_assert(!std::is_const_v<T>,
      "Transport-by-rvalue-reference may not be const.");

 public:
  using value_type = T;
  using type = T&&;

  constexpr transport(std::in_place_index_t<0>, T&& v)
  noexcept
  : data_(std::in_place_index<0>, std::addressof(static_cast<T&>(v)))
  {}

  constexpr transport(std::in_place_index_t<1>, objpipe_errc e)
  noexcept
  : data_(std::in_place_index<1>, e)
  {}

  friend auto swap(transport& x, transport& y)
  noexcept
  -> void {
    using std::swap;
    swap(x.data_, y.data_);
  }

  auto has_value() const
  noexcept
  -> bool {
    return data_.index() == 0;
  }

  auto value() const
  noexcept
  -> T&& {
    assert(has_value());
    return std::move(*std::get<0>(data_));
  }

  auto value_ptr() const
  noexcept
  -> T* {
    assert(has_value());
    return std::get<0>(data_);
  }

  auto ref()
  noexcept
  -> std::add_lvalue_reference_t<value_type> {
    assert(has_value());
    return *std::get<0>(data_);
  }

  auto errc() const
  noexcept
  -> objpipe_errc {
    return (has_value() ? objpipe_errc::success : std::get<1>(data_));
  }

  auto emplace(std::in_place_index_t<0>, T&& v)
  noexcept
  -> void {
    data_.template emplace<0>(std::addressof(static_cast<T&>(v)));
  }

  auto emplace(std::in_place_index_t<1>, objpipe_errc e)
  noexcept
  -> void {
    data_.template emplace<1>(e);
  }

  operator transport<std::remove_cv_t<std::remove_reference_t<type>>>() &&
  noexcept(std::is_nothrow_move_constructible_v<value_type>) {
    using rv_type = transport<std::remove_cv_t<std::remove_reference_t<type>>>;

    if (has_value())
      return rv_type(std::in_place_index<0>, std::move(*this).value());
    else
      return rv_type(std::in_place_index<1>, errc());
  }

  auto by_value() &&
  noexcept(std::is_nothrow_move_constructible_v<value_type>)
  -> transport<value_type> {
    return std::move(*this);
  }

 private:
  std::variant<T*, objpipe_errc> data_;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_TRANSPORT_H */
