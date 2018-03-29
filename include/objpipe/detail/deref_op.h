#ifndef OBJPIPE_DETAIL_DEREF_OP_H
#define OBJPIPE_DETAIL_DEREF_OP_H

#include <type_traits>
#include <utility>
#include <objpipe/detail/invocable_.h>

namespace objpipe::detail {


///\brief Implements the dereference operator for T.
///\details Simply invokes operator* on an instance of T.
struct deref_op {
  ///\brief Test if the type T is dereferencable.
  template<typename T>
  static constexpr bool is_valid = is_invocable_v<deref_op, const T&>
      || is_invocable_v<deref_op, T&>
      || is_invocable_v<deref_op, T&&>;

  ///\brief Result type adapter, that enforces an lvalue reference
  ///to be returned as const reference.
  template<typename T>
  using add_const_to_ref = std::conditional_t<
      std::is_lvalue_reference_v<T>,
      std::add_lvalue_reference_t<std::add_const_t<std::remove_reference_t<T>>>,
      T>;

  /**
   * \brief Dereference invocation.
   * \details
   * The type returned by the dereference operation is made const,
   * if it is a reference.
   *
   * This ensures that if the argument \p v is a pointer, objpipe won't
   * attempt to modify the contents of the pointer.
   * While it also ensures that, in the case of for example std::optional
   * passed by rvalue reference, the returned rvalue reference is kept mutable.
   *
   * \ref adapter_t::transform handles the life time considerations of the
   * returned reference.
   * Since we cannot reliably detect if the life time of the object exceeds
   * that of the pointer, we assume it doesn't.
   *
   * \param[in] v A value that is to be dereferenced.
   * \returns ``*v``
   */
  template<typename T>
  constexpr auto operator()(T&& v) const
  noexcept(noexcept(*std::declval<T>()))
  -> add_const_to_ref<decltype(*std::declval<T>())> {
    return *std::forward<T>(v);
  }
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_DEREF_OP_H */
