#ifndef OBJPIPE_DETAIL_CALLBACK_PIPE_H
#define OBJPIPE_DETAIL_CALLBACK_PIPE_H

///\file
///\ingroup objpipe_detail

#include <type_traits>
#include <utility>
#include <variant>
#include <boost/coroutine2/protected_fixedsize_stack.hpp>
#include <boost/coroutine2/coroutine.hpp>
#include <objpipe/detail/transport.h>
#include <objpipe/detail/invocable_.h>

namespace objpipe::detail {


template<typename T>
class callback_push {
 public:
  using impl_type =
      typename boost::coroutines2::coroutine<std::add_pointer_t<T>>::push_type;

  constexpr callback_push() = default;

  constexpr callback_push(impl_type& impl)
  : impl_(std::addressof(impl))
  {}

  void operator()(const T& v) {
    (*this)(T(v)); // Copy
  }

  void operator()(T&& v) {
    assert(impl_ != nullptr);
    (*impl_)(std::addressof(static_cast<T&>(v)));
  }

 private:
  impl_type* impl_ = nullptr;
};

template<typename T>
class callback_push<const T> {
 public:
  using impl_type =
      typename boost::coroutines2::coroutine<std::add_pointer_t<const T>>::push_type;

  constexpr callback_push() = default;

  constexpr callback_push(impl_type& impl)
  : impl_(std::addressof(impl))
  {}

  void operator()(const T& v) {
    assert(impl_ != nullptr);
    (*impl_)(std::addressof(v));
  }

 private:
  impl_type* impl_ = nullptr;
};

template<typename T, typename Fn>
class callback_fn_wrapper {
 public:
  constexpr callback_fn_wrapper(Fn&& fn)
  noexcept(std::is_nothrow_move_constructible_v<Fn>)
  : fn_(std::move(fn))
  {}

  constexpr callback_fn_wrapper(const Fn& fn)
  noexcept(std::is_nothrow_copy_constructible_v<Fn>)
  : fn_(fn)
  {}

  auto operator()(typename callback_push<T>::impl_type& push)
  -> void {
    callback_push<T> cb_push = callback_push<T>(push);
    if constexpr(is_invocable_v<Fn, callback_push<T>&&>) {
      std::invoke(fn_, std::move(cb_push));
    } else {
      std::invoke(fn_, cb_push);
    }
  }

 private:
  Fn fn_;
};

/**
 * \brief Extract values from fn, which emits them via a callback.
 * \implements SourceConcept
 * \ingroup objpipe_detail
 *
 * \details
 * This implementation creates a coroutine for the given functor.
 *
 * The functor is given an argument that implements two function operations:
 * \code
 * void operator()(T&&);
 * void operator()(const T&);
 * \endcode
 *
 * Except when the \p T argument is const, in which case only one function operation is implemented in the callback:
 * \code
 * void operator()(const T&);
 * \endcode
 *
 * The function is to emit values using the callback, which then transports
 * the value to the objpipe.
 *
 * The full setup of the coroutine is not done until the first time the objpipe
 * is read from.
 *
 * \note The implementation wraps the boost::coroutines2 push_type,
 * so the type passed to the function is not that.
 *
 * \tparam T The type of elements yielded by the function.
 * This should not be the plain type, without reference, const, or volatile qualifiers.
 * \tparam Fn The function type, which is to yield values using a callback.
 *
 * \note
 * C++ does not currently have co-routines, but they are likely to appear in C++20.
 * At that point, this implementation will be augmented to select native co-routines instead.
 */
template<typename T, typename Fn>
class callback_pipe {
  static_assert(!std::is_volatile_v<T>,
      "Type argument to callback may not be volatile.");
  static_assert(!std::is_lvalue_reference_v<T>
      && !std::is_rvalue_reference_v<T>,
      "Type argument to callback may not be a reference.");

 private:
  using coro_t = boost::coroutines2::coroutine<std::add_pointer_t<T>>;
  using fn_type = callback_fn_wrapper<T, Fn>;
  using result_type = std::conditional_t<std::is_const_v<T>,
        std::add_lvalue_reference_t<T>,
        std::add_rvalue_reference_t<T>>;

  static_assert(std::is_move_constructible_v<typename coro_t::pull_type>,
      "Coroutine object must be move constructible.");

 public:
  callback_pipe(callback_pipe&& other)
  noexcept(std::is_nothrow_move_constructible_v<fn_type>
      && std::is_nothrow_move_constructible_v<typename coro_t::pull_type>)
  : src_(std::move(other.src_))
  {}

  callback_pipe(const callback_pipe&) = delete;
  callback_pipe& operator=(const callback_pipe&) = delete;
  callback_pipe& operator=(callback_pipe&&) = delete;

  constexpr callback_pipe(Fn&& fn)
  noexcept(std::is_nothrow_move_constructible_v<Fn>)
  : src_(std::in_place_index<0>, std::move(fn))
  {}

  constexpr callback_pipe(const Fn& fn)
  noexcept(std::is_nothrow_copy_constructible_v<Fn>)
  : src_(std::in_place_index<0>, fn)
  {}

  auto is_pullable()
  noexcept
  -> bool {
    ensure_init_();
    return bool(std::get<1>(src_));
  }

  auto wait()
  -> objpipe_errc {
    ensure_init_();
    return (std::get<1>(src_) ? objpipe_errc::success : objpipe_errc::closed);
  }

  auto front()
  -> transport<result_type> {
    ensure_init_();
    auto& coro = std::get<1>(src_);

    if (!coro) return transport<result_type>(std::in_place_index<1>, objpipe_errc::closed);
    return transport<result_type>(std::in_place_index<0>, get());
  }

  auto pop_front()
  -> objpipe_errc {
    ensure_init_();
    auto& coro = std::get<1>(src_);

    if (!coro) return objpipe_errc::closed;
    coro();
    return objpipe_errc::success;
  }

 private:
  void ensure_init_() {
    if (src_.index() == 0) {
      fn_type fn = std::get<0>(std::move(src_));
      src_.template emplace<1>(
          boost::coroutines2::protected_fixedsize_stack(),
          std::move(fn));
    }
  }

  auto get()
  -> result_type {
    assert(src_.index() == 1);
    assert(std::get<1>(src_));
    if constexpr(std::is_const_v<result_type>)
      return *std::get<1>(src_).get();
    else
      return std::move(*std::get<1>(src_).get());
  }

  std::variant<fn_type, typename coro_t::pull_type> src_;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_CALLBACK_PIPE_H */
