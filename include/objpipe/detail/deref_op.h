#ifndef OBJPIPE_DETAIL_DEREF_OP_H
#define OBJPIPE_DETAIL_DEREF_OP_H

#include <type_traits>
#include <utility>
#include <objpipe/detail/invocable_.h>

namespace objpipe::detail {


///\brief Implements the dereference operator for T.
///\details Simply invokes operator* on an instance of T.
///\bug Deref operation should mark its returned reference as const, if the input reference is const.
struct deref_op {
  template<typename T>
  static constexpr bool is_valid = is_invocable_v<deref_op, const T&>
      || is_invocable_v<deref_op, T&>
      || is_invocable_v<deref_op, T&&>;

  template<typename T>
  constexpr auto operator()(T&& v) const
  noexcept(noexcept(*std::declval<T>()))
  -> decltype(*std::declval<T>()) {
    return static_cast<decltype(*std::declval<T>())>(*v);
  }
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_DEREF_OP_H */
