#ifndef OBJPIPE_DETAIL_VIRTUAL_H
#define OBJPIPE_DETAIL_VIRTUAL_H

///\file
///\ingroup objpipe_detail

#include <cassert>
#include <type_traits>
#include <memory>
#include <stdexcept>
#include <objpipe/errc.h>
#include <objpipe/detail/adapt.h>
#include <objpipe/detail/transport.h>
#include <objpipe/detail/invocable_.h>
#include <objpipe/detail/push_op.h>

namespace objpipe::detail {


///\brief Interface for ioc_push acceptors.
///\ingroup objpipe_detail
///\details
///The interface allows bridging the interface barrier that virtual_pipe makes.
template<typename T>
class virtual_push_acceptor_intf {
 public:
  ///\brief Destructor.
  virtual ~virtual_push_acceptor_intf() noexcept {}

  ///\brief Push a value to the acceptor wrapped by the implementation.
  virtual auto operator()(T v) -> objpipe_errc = 0;
  ///\brief Push an exception to the acceptor wrapped by the implementation.
  virtual auto push_exception(std::exception_ptr exptr) noexcept -> void = 0;
  ///\brief Create a copy of the acceptor, if it is copyable.
  ///\throws std::logic_error if the implementation is not copy constructible.
  virtual auto clone() const -> std::unique_ptr<virtual_push_acceptor_intf> = 0;
};

///\brief Implementation of virtual_push_acceptor_intf that wraps an acceptor.
///\ingroup objpipe_detail
template<typename T, typename Impl>
class virtual_push_acceptor_impl
: public virtual_push_acceptor_intf<T>
{
 public:
  ///\brief Constructor.
  ///\param[in] impl The acceptor to wrap.
  explicit virtual_push_acceptor_impl(Impl&& impl)
  noexcept(std::is_nothrow_move_constructible_v<Impl>)
  : impl_(std::move(impl))
  {}

  ///\brief Constructor.
  ///\param[in] impl The acceptor to wrap.
  explicit virtual_push_acceptor_impl(const Impl& impl)
  noexcept(std::is_nothrow_copy_constructible_v<Impl>)
  : impl_(impl)
  {}

  ///\brief Destructor.
  ~virtual_push_acceptor_impl() noexcept override {}

  ///\copydoc virtual_push_acceptor_intf::operator()(T)
  auto operator()(T v)
  -> objpipe_errc override {
    if constexpr(is_invocable_v<Impl&, T>)
      return std::invoke(impl_, std::move(v));
    else
      return std::invoke(impl_, v);
  }

  ///\copydoc virtual_push_acceptor_intf::push_exception
  auto push_exception(std::exception_ptr exptr)
  noexcept
  -> void override {
    impl_.push_exception(exptr);
  }

  ///\copydoc virtual_push_acceptor_intf::clone
  auto clone()
  const
  -> std::unique_ptr<virtual_push_acceptor_intf<T>> override {
    if constexpr(std::is_copy_constructible_v<Impl>)
      return std::make_unique<virtual_push_acceptor_impl>(impl_);
    else
      throw std::logic_error("push acceptor is not copy constructible");
  }

 private:
  ///\brief Wrapped acceptor implementation.
  Impl impl_;
};

///\brief Acceptor used by virtual_pipe's ioc_push interface.
template<typename T>
class virtual_push_acceptor {
 public:
  virtual_push_acceptor() = delete;

  ///\brief Wrap an acceptor.
  ///\note This constructor only participates in overload resolution
  ///if \p impl is not virtual_push_acceptor.
  ///\param[in] impl The acceptor to wrap.
  template<typename Impl, typename = std::enable_if_t<!std::is_base_of_v<virtual_push_acceptor, std::decay_t<Impl>>>>
  explicit virtual_push_acceptor(Impl&& impl)
  : impl_(std::make_unique<virtual_push_acceptor_impl<T, std::decay_t<Impl>>>(std::forward<Impl>(impl)))
  {}

  ///\brief Move constructor.
  virtual_push_acceptor(virtual_push_acceptor&& rhs)
  noexcept
  : impl_(std::move(rhs.impl_))
  {}

  ///\brief Copy constructor.
  ///details Throws std::logic_error if the acceptor is not copyable.
  virtual_push_acceptor(const virtual_push_acceptor& rhs)
  : impl_(rhs.impl_ == nullptr ? nullptr : rhs.impl_->clone())
  {}

  virtual_push_acceptor& operator=(const virtual_push_acceptor& rhs) = delete;

  ///\brief Move assignment.
  ///\details Required to meet the swappable concept.
  virtual_push_acceptor& operator=(virtual_push_acceptor&& rhs) {
    impl_ = std::move(rhs.impl_);
    return *this;
  }

  ///\brief Accept a value by rvalue reference.
  ///\details
  ///Forwards the value to the wrapper, which in turn forwards it
  ///to the implementation that was passed at construction time.
  auto operator()(T&& v) -> objpipe_errc {
    return (*impl_)(std::move(v));
  }

  ///\brief Accept a value by const reference.
  ///\details
  ///Forwards the value to the wrapper, which in turn forwards it
  ///to the implementation that was passed at construction time.
  auto operator()(const T& v) -> objpipe_errc {
    return (*impl_)(v);
  }

  ///\brief Accept an exception.
  auto push_exception(std::exception_ptr exptr)
  noexcept
  -> void {
    impl_->push_exception(std::move(exptr));
  }

 private:
  ///\brief Pointer to the wrapper around implementation.
  std::unique_ptr<virtual_push_acceptor_intf<T>> impl_;
};


///\brief Internal interface to virtualize an objpipe.
///\ingroup objpipe_detail
template<typename T>
class virtual_intf {
 public:
  virtual ~virtual_intf() noexcept {}

  virtual auto is_pullable() noexcept -> bool = 0;
  virtual auto wait() -> objpipe_errc = 0;
  virtual auto front() -> transport<T> = 0;
  virtual auto pop_front() -> objpipe_errc = 0;
  virtual auto pull() -> transport<T> = 0;
  virtual auto try_pull() -> transport<T> = 0;

  virtual auto can_push(existingthread_push tag) const noexcept -> bool = 0;
  virtual auto ioc_push(existingthread_push tag, virtual_push_acceptor<T> acceptor) && -> void = 0;
  virtual auto can_push(singlethread_push tag) const noexcept -> bool = 0;
  virtual auto ioc_push(singlethread_push tag, virtual_push_acceptor<T> acceptor) && -> void = 0;
  virtual auto can_push(multithread_push tag) const noexcept -> bool = 0;
  virtual auto ioc_push(multithread_push tag, virtual_push_acceptor<T> acceptor) && -> void = 0;
  virtual auto can_push(multithread_unordered_push tag) const noexcept -> bool = 0;
  virtual auto ioc_push(multithread_unordered_push tag, virtual_push_acceptor<T> acceptor) && -> void = 0;
};

///\brief Internal implementation to virtualize an objpipe.
///\ingroup objpipe_detail
///\tparam Source The objpipe source that is to be abstracted.
template<typename Source>
class virtual_impl
: public virtual_intf<adapt::value_type<Source>>
{
 public:
  explicit virtual_impl(Source&& src)
  noexcept(std::is_nothrow_move_constructible_v<Source>)
  : src_(std::move(src))
  {}

  ~virtual_impl() noexcept override {}

  auto is_pullable()
  noexcept
  -> bool override {
    return src_.is_pullable();
  }

  auto wait()
  -> objpipe_errc override {
    return src_.wait();
  }

  auto front()
  -> transport<adapt::value_type<Source>> override {
    return src_.front();
  }

  auto pop_front()
  -> objpipe_errc override {
    return src_.pop_front();
  }

  auto try_pull()
  -> transport<adapt::value_type<Source>> override {
    return adapt::raw_try_pull(src_);
  }

  auto pull()
  -> transport<adapt::value_type<Source>> override {
    return adapt::raw_pull(src_);
  }

  auto can_push(existingthread_push tag) const
  noexcept
  -> bool override {
    if constexpr(adapt::has_ioc_push<Source, existingthread_push>)
      return src_.can_push(tag);
    else
      return false;
  }

  auto can_push(singlethread_push tag) const
  noexcept
  -> bool override {
    if constexpr(adapt::has_ioc_push<Source, singlethread_push>)
      return src_.can_push(tag);
    else
      return false;
  }

  auto can_push(multithread_push tag) const
  noexcept
  -> bool override {
    if constexpr(adapt::has_ioc_push<Source, multithread_push>)
      return src_.can_push(tag);
    else
      return false;
  }

  auto can_push(multithread_unordered_push tag) const
  noexcept
  -> bool override {
    if constexpr(adapt::has_ioc_push<Source, multithread_unordered_push>)
      return src_.can_push(tag);
    else
      return false;
  }

  auto ioc_push(existingthread_push tag, virtual_push_acceptor<adapt::value_type<Source>> acceptor) &&
  -> void override {
    adapt::ioc_push(std::move(src_), tag, std::move(acceptor));
  }

  auto ioc_push(singlethread_push tag, virtual_push_acceptor<adapt::value_type<Source>> acceptor) &&
  -> void override {
    adapt::ioc_push(std::move(src_), tag, std::move(acceptor));
  }

  auto ioc_push(multithread_push tag, virtual_push_acceptor<adapt::value_type<Source>> acceptor) &&
  -> void override {
    adapt::ioc_push(std::move(src_), tag, std::move(acceptor));
  }

  auto ioc_push(multithread_unordered_push tag, virtual_push_acceptor<adapt::value_type<Source>> acceptor) &&
  -> void override {
    adapt::ioc_push(std::move(src_), tag, std::move(acceptor));
  }

 private:
  ///\brief Underlying source.
  Source src_;
};

/**
 * \brief An objpipe that hides the source behind an interface.
 * \implements TransformationConcept
 * \ingroup objpipe_detail
 *
 * \details
 * The virtual_pipe hides an objpipe behind an interface.
 * It is used by the reader type to provide a uniform boundary for functions.
 *
 * \tparam T The type of elements iterated by the pipe.
 */
template<typename T>
class virtual_pipe {
 public:
  constexpr virtual_pipe() = default;

  template<typename Source>
  explicit virtual_pipe(Source&& src)
  : pimpl_(std::make_unique<virtual_impl<std::decay_t<Source>>>(std::forward<Source>(src)))
  {}

  auto is_pullable() noexcept
  -> bool {
    assert(pimpl_ != nullptr);
    return pimpl_->is_pullable();
  }

  auto wait()
  -> objpipe_errc {
    assert(pimpl_ != nullptr);
    return pimpl_->wait();
  }

  auto front()
  -> transport<T> {
    assert(pimpl_ != nullptr);
    return pimpl_->front();
  }

  auto pop_front()
  -> objpipe_errc {
    assert(pimpl_ != nullptr);
    return pimpl_->pop_front();
  }

  auto try_pull()
  -> transport<T> {
    assert(pimpl_ != nullptr);
    return pimpl_->try_pull();
  }

  auto pull()
  -> transport<T> {
    assert(pimpl_ != nullptr);
    return pimpl_->pull();
  }

  template<typename PushTag>
  auto can_push(PushTag tag) const noexcept -> bool {
    assert(pimpl_ != nullptr);
    return pimpl_->can_push(tag);
  }

  template<typename PushTag, typename Acceptor>
  auto ioc_push(PushTag&& tag, Acceptor&& acceptor) &&
  -> void {
    assert(pimpl_ != nullptr);
    std::move(*std::exchange(pimpl_, nullptr)).ioc_push(
        std::forward<PushTag>(tag),
        virtual_push_acceptor<T>(std::forward<Acceptor>(acceptor)));
  }

 private:
  ///\brief Pointer to the implementation of the acceptor that is abstracted away.
  std::unique_ptr<virtual_intf<T>> pimpl_;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_VIRTUAL_H */
