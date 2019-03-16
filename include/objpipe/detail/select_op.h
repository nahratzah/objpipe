#ifndef OBJPIPE_DETAIL_SELECT_OP_H
#define OBJPIPE_DETAIL_SELECT_OP_H

#include <type_traits>
#include <utility>

namespace objpipe::detail {


///\brief Select the first item of std::pair.
struct select_first_op {
  template<typename T>
  static constexpr bool is_valid = is_invocable_v<select_first_op, T&>
      || is_invocable_v<select_first_op, const T&>
      || is_invocable_v<select_first_op, T&&>;

  template<typename T, typename U>
  constexpr auto operator()(const std::pair<T, U>& v) const
  noexcept
  -> std::add_lvalue_reference_t<std::add_const_t<T>> {
    return v.first;
  }

  template<typename T, typename U>
  constexpr auto operator()(std::pair<T, U>& v) const
  noexcept
  -> std::add_lvalue_reference_t<T> {
    return v.first;
  }

  template<typename T, typename U>
  constexpr auto operator()(std::pair<T, U>&& v) const
  noexcept
  -> std::enable_if_t<!std::is_const_v<T>, std::add_rvalue_reference_t<T>> {
    return static_cast<std::add_rvalue_reference_t<T>>(v.first);
  }

  template<typename T, typename U>
  constexpr auto operator()(std::pair<T, U>&& v) const
  noexcept
  -> std::enable_if_t<std::is_const_v<T>, std::add_lvalue_reference_t<T>> {
    return v.first;
  }
};

///\brief Select the second item of std::pair.
struct select_second_op {
  template<typename T>
  static constexpr bool is_valid = is_invocable_v<select_second_op, T&>
      || is_invocable_v<select_second_op, const T&>
      || is_invocable_v<select_second_op, T&&>;

  template<typename T, typename U>
  constexpr auto operator()(const std::pair<T, U>& v) const
  noexcept
  -> std::add_lvalue_reference_t<std::add_const_t<U>> {
    return v.second;
  }

  template<typename T, typename U>
  constexpr auto operator()(std::pair<T, U>& v) const
  noexcept
  -> std::add_lvalue_reference_t<U> {
    return v.second;
  }

  template<typename T, typename U>
  constexpr auto operator()(std::pair<T, U>&& v) const
  noexcept
  -> std::enable_if_t<!std::is_const_v<U>, std::add_rvalue_reference_t<U>> {
    return static_cast<std::add_rvalue_reference_t<U>>(v.second);
  }

  template<typename T, typename U>
  constexpr auto operator()(std::pair<T, U>&& v) const
  noexcept
  -> std::enable_if_t<std::is_const_v<U>, std::add_rvalue_reference_t<U>> {
    return v.second;
  }
};

///\brief Invokes std::get<Idx> on its value.
template<size_t Idx>
struct select_index_op {
  template<typename T>
  static constexpr bool is_valid = is_invocable_v<select_index_op, T&>
      || is_invocable_v<select_index_op, const T&>
      || is_invocable_v<select_index_op, T&&>;

  template<typename T>
  constexpr auto operator()(T&& v) const
  noexcept(noexcept(std::get<Idx>(std::declval<T>())))
  -> decltype(std::get<Idx>(std::declval<T>())) {
    return std::get<Idx>(std::forward<T>(v));
  }
};

///\brief Invokes std::get<Type> on its value.
template<typename Type>
struct select_type_op {
  template<typename T>
  static constexpr bool is_valid = is_invocable_v<select_type_op, T&>
      || is_invocable_v<select_type_op, const T&>
      || is_invocable_v<select_type_op, T&&>;

  template<typename T>
  constexpr auto operator()(T&& v) const
  noexcept(noexcept(std::get<Type>(std::declval<T>())))
  -> decltype(std::get<Type>(std::declval<T>())) {
    return std::get<Type>(std::forward<T>(v));
  }
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_SELECT_OP_H */
