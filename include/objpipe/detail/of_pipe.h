#ifndef OBJPIPE_DETAIL_OF_PIPE_H
#define OBJPIPE_DETAIL_OF_PIPE_H

///\file
///\ingroup objpipe_detail

#include <optional>
#include <type_traits>
#include <utility>
#include <objpipe/errc.h>
#include <objpipe/detail/transport.h>

namespace objpipe::detail {


/**
 * \brief Object pipe containing a single element.
 * \ingroup objpipe_detail
 *
 * \details The object pipe yields the single element it was constructed with.
 * \sa \ref objpipe::of
 */
template<typename T>
class of_pipe {
 private:
  using type = std::add_rvalue_reference_t<T>;
  using transport_type = transport<type>;

 public:
  explicit constexpr of_pipe(T&& v)
  noexcept(std::is_nothrow_move_constructible_v<T>)
  : val_(std::move(v))
  {}

  explicit constexpr of_pipe(const T& v)
  noexcept(std::is_nothrow_copy_constructible_v<T>)
  : val_(v)
  {}

  constexpr of_pipe(of_pipe&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
  of_pipe(const of_pipe&) = delete;
  of_pipe& operator=(const of_pipe&) = delete;
  of_pipe& operator=(of_pipe&&) = delete;

  constexpr auto is_pullable()
  noexcept
  -> bool {
    return !consumed_;
  }

  constexpr auto wait()
  noexcept
  -> objpipe_errc {
    return (!consumed_ ? objpipe_errc::success : objpipe_errc::closed);
  }

  ///\note An rvalue reference is returned, since front() is only allowed to be called at most once before pop_front() or pull().
  constexpr auto front()
  noexcept
  -> transport_type {
    if (consumed_)
      return transport_type(std::in_place_index<1>, objpipe_errc::closed);
    return transport_type(std::in_place_index<0>, std::move(val_));
  }

  constexpr auto pop_front()
  noexcept
  -> objpipe_errc {
    if (consumed_) return objpipe_errc::closed;
    consumed_ = true;
    return objpipe_errc::success;
  }

  constexpr auto try_pull()
  noexcept
  -> transport_type {
    return pull();
  }

  constexpr auto pull()
  noexcept
  -> transport_type {
    if (consumed_)
      return transport_type(std::in_place_index<1>, objpipe_errc::closed);
    consumed_ = true;
    return transport_type(std::in_place_index<0>, std::move(val_));
  }

 private:
  bool consumed_ = false;
  T val_;
};

template<typename T>
class of_pipe<std::reference_wrapper<T>> {
 private:
  using type = std::add_lvalue_reference_t<std::add_const_t<T>>;
  using transport_type = transport<type>;

 public:
  explicit constexpr of_pipe(std::reference_wrapper<T> ref)
  noexcept
  : val_(&ref.get())
  {}

  constexpr of_pipe(of_pipe&&) noexcept = default;

  of_pipe(const of_pipe&) = delete;
  of_pipe& operator=(const of_pipe&) = delete;
  of_pipe& operator=(of_pipe&&) = delete;

  constexpr auto is_pullable()
  noexcept
  -> bool {
    return val_ != nullptr;
  }

  constexpr auto wait() const
  noexcept
  -> objpipe_errc {
    return (val_ != nullptr ? objpipe_errc::success : objpipe_errc::closed);
  }

  constexpr auto front() const
  noexcept
  -> transport_type {
    if (val_ == nullptr)
      return transport_type(std::in_place_index<1>, objpipe_errc::closed);
    return transport_type(std::in_place_index<0>, *val_);
  }

  constexpr auto pop_front()
  noexcept
  -> objpipe_errc {
    if (val_ == nullptr) return objpipe_errc::closed;
    val_ = nullptr;
    return objpipe_errc::success;
  }

  constexpr auto try_pull()
  noexcept
  -> transport_type {
    return pull();
  }

  constexpr auto pull()
  noexcept
  -> transport_type {
    auto rv = front();
    val_ = nullptr;
    return rv;
  }

 private:
  std::add_const_t<T>* val_;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_OF_PIPE_H */
