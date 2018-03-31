#ifndef OBJPIPE_DETAIL_INTERLOCK_PIPE_H
#define OBJPIPE_DETAIL_INTERLOCK_PIPE_H

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <type_traits>
#include <exception>
#include <utility>
#include <variant>
#include <objpipe/errc.h>
#include <objpipe/detail/transport.h>

namespace objpipe::detail {


/**
 * \brief Acceptor interface for interlock.
 * \ingroup objpipe_detail
 * \details Interlock needs an interface, in order to make the acceptor
 * available to its \ref interlock_writer "writers".
 *
 * \tparam T The type argument to interlock.
 */
template<typename T>
class interlock_acceptor_intf {
 public:
  ///\brief Value type of the interlock.
  using value_type = std::remove_cv_t<std::remove_reference_t<T>>;

  /** \brief Destructor. */
  virtual ~interlock_acceptor_intf() noexcept {}

  /**
   * \brief Publish method.
   * \details Used by \ref interlock_writer "writers" to publish values.
   */
  virtual auto publish(std::conditional_t<std::is_const_v<T>,
      std::add_lvalue_reference_t<std::add_const_t<value_type>>,
      std::add_rvalue_reference_t<value_type>> v)
  noexcept
  -> std::variant<objpipe_errc, interlock_acceptor_intf*> = 0;

  /**
   * \brief Publish method for const values.
   * \details This method only exists if the interlock is for non-const values,
   * and functions by publishing a copy the underlying value.
   *
   * The publish method does not return until the receiving side has consumed
   * the value.
   * \param[in] v A value to publish.
   * \returns An objpipe_errc indicating success or failure.
   * If the caller should replace its acceptor and the value was successfully published,
   * the variant will hold the second type, which is a pointer to the replacement interface.
   * (The replacement interface should have its select_on_writer_copy() method called.
   * It's safe to ignore the pointer, but performance will be less,
   * as future invocations of push will have to forward to this each time.)
   */
  template<bool Enable = !std::is_const_v<T>>
  auto publish(std::add_lvalue_reference_t<std::add_const_t<value_type>> v)
  noexcept
  -> std::enable_if_t<Enable, std::variant<objpipe_errc, interlock_acceptor_intf*>> {
    return publish(T(v));
  }

  /**
   * \brief Publish an exception.
   * \details Makes the exception available to the pipe or acceptor.
   */
  virtual auto publish_exception(std::exception_ptr exptr) noexcept -> objpipe_errc = 0;

  /**
   * \brief Increment writer reference counter.
   */
  virtual auto inc_writer() noexcept -> void = 0;
  /**
   * \brief Decrement writer reference counter.
   * \returns True if the acceptor is to be deleted.
   */
  virtual auto subtract_writer() noexcept -> bool = 0;

  /**
   * \brief Selects an acceptor interface and increments its writer.
   * \details The default implementation shares itself with all writers.
   * A specialization of this method can return a newly allocated copy instead.
   * \returns An acceptor that has had its writer count incremented.
   *
   * \bug Should return a std::unique_ptr, instead of a raw pointer.
   */
  virtual auto select_on_writer_copy() -> interlock_acceptor_intf* {
    inc_writer();
    return this;
  }
};


/**
 * \brief Implementation of interlock_acceptor_impl, where the wrapped acceptor is shared across multiple threads.
 * \ingroup objpipe_detail
 * \details Since the acceptor is shared, this acceptor must implement synchronization.
 */
template<typename T, typename Acceptor>
class interlock_acceptor_impl_shared final
: public interlock_acceptor_intf<T>
{
 public:
  ///\brief Value type of the interlock.
  using value_type = typename interlock_acceptor_intf<T>::value_type;

  /**
   * \brief Constructor, wrapping an acceptor.
   * \param[in] acceptor The acceptor to wrap.
   */
  explicit interlock_acceptor_impl_shared(Acceptor&& acceptor)
  noexcept(std::is_nothrow_move_constructible_v<Acceptor>)
  : acceptor_(std::move(acceptor))
  {}

  /**
   * \brief Constructor, wrapping an acceptor.
   * \param[in] acceptor The acceptor to wrap.
   */
  explicit interlock_acceptor_impl_shared(const Acceptor& acceptor)
  noexcept(std::is_nothrow_copy_constructible_v<Acceptor>)
  : acceptor_(std::move(acceptor))
  {}

  ///\brief Destructor.
  ~interlock_acceptor_impl_shared() noexcept override {}

  ///\brief Push a value into the acceptor.
  auto publish(std::conditional_t<std::is_const_v<T>,
      std::add_lvalue_reference_t<std::add_const_t<value_type>>,
      std::add_rvalue_reference_t<value_type>> v)
  noexcept
  -> std::variant<objpipe_errc, interlock_acceptor_intf<T>*> override {
    std::lock_guard<std::mutex> lck{ mtx_ };

    try {
      if constexpr(std::is_const_v<T>)
        return acceptor_(v);
      else
        return acceptor_(std::move(v));
    } catch (...) {
      acceptor_.push_exception(std::current_exception());
      return objpipe_errc::bad;
    }
  }

  ///\brief Push an exception into the acceptor.
  auto publish_exception(std::exception_ptr exptr)
  noexcept
  -> objpipe_errc override {
    std::lock_guard<std::mutex> lck{ mtx_ };

    acceptor_.push_exception(exptr);
    return objpipe_errc::success;
  }

  ///\copydoc interlock_acceptor_intf::inc_writer
  auto inc_writer()
  noexcept
  -> void override {
    writer_count_.fetch_add(1u, std::memory_order_acquire);
  }

  ///\copydoc interlock_acceptor_intf::subtract_writer
  auto subtract_writer()
  noexcept
  -> bool override {
    return writer_count_.fetch_sub(1u, std::memory_order_release) == 1u;
  }

 private:
  ///\brief Mutex, to serialize access to acceptor_.
  std::mutex mtx_;
  ///\brief Acceptor that is to receive values.
  Acceptor acceptor_;
  ///\brief Number of writers referencing this.
  std::atomic<std::uintptr_t> writer_count_ = 0;
};


///\brief Acceptor wrapper for unordered traversal of interlock.
///\ingroup objpipe_detail
///\details In the unordered case, we can use copies of the acceptor,
///instead of synchronizing shared access via a mutex.
template<typename T, typename Acceptor>
class interlock_acceptor_impl_unordered final
: public interlock_acceptor_intf<T>
{
 public:
  ///\brief Value type of the interlock.
  using value_type = typename interlock_acceptor_intf<T>::value_type;

  /**
   * \brief Constructor, wrapping an acceptor.
   * \param[in] acceptor The acceptor to wrap.
   */
  explicit interlock_acceptor_impl_unordered(Acceptor&& acceptor)
  noexcept(std::is_nothrow_move_constructible_v<Acceptor>)
  : acceptor_(std::move(acceptor))
  {}

  /**
   * \brief Constructor, wrapping an acceptor.
   * \param[in] acceptor The acceptor to wrap.
   */
  explicit interlock_acceptor_impl_unordered(const Acceptor& acceptor)
  noexcept(std::is_nothrow_copy_constructible_v<Acceptor>)
  : acceptor_(std::move(acceptor))
  {}

  ///\brief Copy constructor.
  ///\details Copies the wrapped acceptor.
  interlock_acceptor_impl_unordered(const interlock_acceptor_impl_unordered& rhs)
  noexcept(std::is_nothrow_copy_constructible_v<Acceptor>)
  : acceptor_(rhs.acceptor_)
  {}

  ///\brief Destructor.
  ~interlock_acceptor_impl_unordered() noexcept override {}

  ///\brief Push a value into the acceptor.
  ///\note This method is not thread safe.
  auto publish(std::conditional_t<std::is_const_v<T>,
      std::add_lvalue_reference_t<std::add_const_t<value_type>>,
      std::add_rvalue_reference_t<value_type>> v)
  noexcept
  -> std::variant<objpipe_errc, interlock_acceptor_intf<T>*> override {
    try {
      if constexpr(std::is_const_v<T>)
        return acceptor_(v);
      else
        return acceptor_(std::move(v));
    } catch (...) {
      acceptor_.push_exception(std::current_exception());
      return objpipe_errc::bad;
    }
  }

  ///\brief Push an exception into the acceptor.
  ///\note This method is not thread safe.
  auto publish_exception(std::exception_ptr exptr)
  noexcept
  -> objpipe_errc override {
    acceptor_.push_exception(exptr);
    return objpipe_errc::success;
  }

  ///\copydoc interlock_acceptor_intf::inc_writer
  auto inc_writer()
  noexcept
  -> void override {
    writer_count_.fetch_add(1u, std::memory_order_acquire);
  }

  ///\copydoc interlock_acceptor_intf::subtract_writer
  auto subtract_writer()
  noexcept
  -> bool override {
    return writer_count_.fetch_sub(1u, std::memory_order_release) == 1u;
  }

  ///\brief Create a copy of this acceptor.
  auto select_on_writer_copy() -> interlock_acceptor_impl_unordered* override {
    // Don't invoke parent, since default impl is for sharing.
    interlock_acceptor_impl_unordered* copy = new interlock_acceptor_impl_unordered(*this);
    copy->inc_writer();
    return copy;
  }

 private:
  ///\brief Wrapped acceptor.
  Acceptor acceptor_;
  ///\brief Reference counter.
  std::atomic<std::uintptr_t> writer_count_ = 0;
};


/**
 * \brief Implementation of interlock shared data structure.
 * \ingroup objpipe_detail
 * \details
 * This data structure is used to communicate between
 * the \ref interlock_writer "interlock writers"
 * and the \ref interlock_reader "interlock readers".
 */
template<typename T>
class interlock_impl final
: public interlock_acceptor_intf<T>
{
 public:
  ///\brief Value type of the interlock.
  using value_type = typename interlock_acceptor_intf<T>::value_type;

 private:
  ///\brief Type that is returned by the ``front()`` method of the interlock source.
  using front_type = std::conditional_t<std::is_const_v<T>,
        std::add_lvalue_reference_t<T>,
        std::add_rvalue_reference_t<T>>;
  ///\brief Type that is returned by the ``pull()`` method of the interlock source.
  using pull_type = value_type;

 public:
  ///\brief Destructor.
  ~interlock_impl() noexcept override {
    if (acceptor_ != nullptr && acceptor_->subtract_writer())
      delete acceptor_;
  }

  auto is_pullable()
  -> bool {
    std::lock_guard<std::mutex> lck_{ guard_ };
    return offered_ != nullptr || exptr_ != nullptr || writer_count_ > 0;
  }

  auto wait()
  -> objpipe_errc {
    std::unique_lock<std::mutex> lck_{ guard_ };
    for (;;) {
      if (exptr_ != nullptr)
        std::rethrow_exception(exptr_);
      if (offered_ != nullptr)
        return objpipe_errc::success;
      if (writer_count_ == 0)
        return objpipe_errc::closed;
      read_ready_.wait(lck_);
    }
  }

  auto front()
  -> transport<front_type> {
    std::unique_lock<std::mutex> lck_{ guard_ };
    read_ready_.wait(lck_,
        [&]() {
          return offered_ != nullptr || exptr_ != nullptr || writer_count_ == 0;
        });
    if (exptr_ != nullptr)
      std::rethrow_exception(exptr_);
    if (writer_count_ == 0)
      return transport<front_type>(std::in_place_index<1>, objpipe_errc::closed);
    return transport<front_type>(std::in_place_index<0>, get(lck_));
  }

  auto pop_front()
  -> objpipe_errc {
    std::unique_lock<std::mutex> lck_{ guard_ };
    read_ready_.wait(lck_,
        [&]() {
          return offered_ != nullptr || exptr_ != nullptr || writer_count_ == 0;
        });
    if (offered_ != nullptr) {
      offered_ = nullptr;
      lck_.unlock();
      write_ready_.notify_one();

      // We need to notify all, because an additional write could have enetered
      // due to us unlocking the mutex.
      // There will be at most two threads awoken, so should be fine.
      write_done_.notify_all();
      return objpipe_errc::success;
    }

    if (exptr_ != nullptr)
      std::rethrow_exception(exptr_);
    assert(writer_count_ == 0);
    return objpipe_errc::closed;
  }

  auto pull()
  -> transport<pull_type> {
    std::unique_lock<std::mutex> lck_{ guard_ };
    read_ready_.wait(lck_,
        [&]() {
          return offered_ != nullptr || exptr_ != nullptr || writer_count_ == 0;
        });
    if (exptr_ != nullptr)
      std::rethrow_exception(exptr_);
    if (writer_count_ == 0)
      return transport<pull_type>(std::in_place_index<1>, objpipe_errc::closed);

    assert(offered_ != nullptr);
    auto result = transport<pull_type>(std::in_place_index<0>, get(lck_));
    offered_ = nullptr;
    lck_.unlock();
    write_ready_.notify_one();

    // We need to notify all, because an additional write could have enetered
    // due to us unlocking the mutex.
    // There will be at most two threads awoken, so should be fine.
    write_done_.notify_all();
    return result;
  }

  auto try_pull()
  -> transport<pull_type> {
    std::unique_lock<std::mutex> lck_{ guard_ };
    if (exptr_ != nullptr)
      std::rethrow_exception(exptr_);

    if (offered_ == nullptr) {
      if (writer_count_ == 0)
        return transport<pull_type>(std::in_place_index<1>, objpipe_errc::closed);
      else
        return transport<pull_type>(std::in_place_index<1>, objpipe_errc::success);
    }

    auto result = transport<pull_type>(std::in_place_index<0>, get(lck_));
    offered_ = nullptr;
    lck_.unlock();
    write_ready_.notify_one();

    // We need to notify all, because an additional write could have enetered
    // due to us unlocking the mutex.
    // There will be at most two threads awoken, so should be fine.
    write_done_.notify_all();
    return result;
  }

  auto publish(std::conditional_t<std::is_const_v<T>,
      std::add_lvalue_reference_t<std::add_const_t<value_type>>,
      std::add_rvalue_reference_t<value_type>> v) noexcept
  -> std::variant<objpipe_errc, interlock_acceptor_intf<T>*> override {
    std::unique_lock<std::mutex> lck_{ guard_ };
    write_ready_.wait(lck_,
        [this]() {
          return offered_ == nullptr || exptr_ != nullptr || reader_count_ == 0 || acceptor_ != nullptr;
        });
    if (acceptor_ != nullptr) {
      std::variant<objpipe_errc, interlock_acceptor_intf<T>*> publish_result;
      if constexpr(std::is_const_v<T>)
        publish_result = acceptor_->publish(v);
      else
        publish_result = acceptor_->publish(std::move(v));
      if (std::holds_alternative<objpipe_errc>(publish_result) && std::get<objpipe_errc>(publish_result) != objpipe_errc::success)
        return publish_result;
      return acceptor_;
    }

    if (exptr_ != nullptr) return objpipe_errc::bad;
    if (reader_count_ == 0) return objpipe_errc::closed;

    const std::add_pointer_t<T> v_ptr = std::addressof(v);
    offered_ = v_ptr;
    read_ready_.notify_one();
    write_done_.wait(lck_,
        [this, v_ptr]() {
          return offered_ != v_ptr || exptr_ != nullptr || reader_count_ == 0;
        });

    if (offered_ != v_ptr) {
      if (acceptor_ != nullptr) return acceptor_;
      return objpipe_errc::success;
    }

    offered_ = nullptr;
    if (exptr_ != nullptr) return objpipe_errc::bad;
    assert(reader_count_ == 0);
    return objpipe_errc::closed;
  }

  auto publish_exception(std::exception_ptr exptr) noexcept
  -> objpipe_errc override {
    assert(exptr != nullptr);

    std::unique_lock<std::mutex> lck_{ guard_ };
    if (acceptor_ != nullptr)
      return acceptor_->publish_exception(std::move(exptr));
    if (reader_count_ == 0) return objpipe_errc::closed;
    if (exptr_ != nullptr) return objpipe_errc::bad;
    exptr_ = std::move(exptr);

    lck_.unlock();
    read_ready_.notify_all();
    write_ready_.notify_all(); // Notify all writers that objpipe went bad.
    write_done_.notify_all(); // Also notify in-progress writes.
    return objpipe_errc::success;
  }

  auto inc_reader() noexcept -> void {
    std::lock_guard<std::mutex> lck{ guard_ };
    ++reader_count_;
    assert(reader_count_ > 0);
  }

  auto subtract_reader() noexcept -> bool {
    std::lock_guard<std::mutex> lck{ guard_ };
    assert(reader_count_ > 0);
    if (--reader_count_ == 0) write_ready_.notify_all();
    return reader_count_ == 0 && writer_count_ == 0;
  }

  auto inc_writer() noexcept -> void override {
    std::lock_guard<std::mutex> lck{ guard_ };
    ++writer_count_;
    assert(writer_count_ > 0);
  }

  auto subtract_writer() noexcept -> bool override {
    std::lock_guard<std::mutex> lck{ guard_ };
    assert(writer_count_ > 0);
    if (--writer_count_ == 0)
      read_ready_.notify_all();
    return reader_count_ == 0 && writer_count_ == 0;
  }

  template<typename Acceptor>
  auto ioc_push([[maybe_unused]] existingthread_push tag, Acceptor&& acceptor)
  -> void {
    std::lock_guard<std::mutex> lck{ guard_ };
    // If an exception is ready, just propagate that immediately.
    if (exptr_ != nullptr) {
      acceptor.push_exception(exptr_);
      return;
    }

    // Create acceptor.
    acceptor_ = new interlock_acceptor_impl_shared<T, std::decay_t<Acceptor>>(std::forward<Acceptor>(acceptor));
    acceptor_->inc_writer();

    // Propagate in-progress offered value immediately.
    if (offered_ != nullptr) {
      try {
        if constexpr(std::is_const_v<T>)
          acceptor_->publish(*offered_);
        else
          acceptor_->publish(std::move(*offered_));
        offered_ = nullptr;
      } catch (...) {
        acceptor_->publish_exception(exptr_ = std::current_exception());
      }

      // Pessimistic notify (under a lock), so notify_one() suffices.
      write_done_.notify_one();
    }

    // Notify all writers to replace their implementation with acceptor_.
    write_ready_.notify_all();
  }

  template<typename Acceptor>
  auto ioc_push([[maybe_unused]] multithread_unordered_push tag, Acceptor&& acceptor)
  -> void {
    std::lock_guard<std::mutex> lck{ guard_ };
    // If an exception is ready, just propagate that immediately.
    if (exptr_ != nullptr) {
      acceptor.push_exception(exptr_);
      return;
    }

    // Create acceptor.
    acceptor_ = new interlock_acceptor_impl_unordered<T, std::decay_t<Acceptor>>(std::forward<Acceptor>(acceptor));
    acceptor_->inc_writer();

    // Propagate in-progress offered value immediately.
    if (offered_ != nullptr) {
      try {
        if constexpr(std::is_const_v<T>)
          acceptor_->publish(*offered_);
        else
          acceptor_->publish(std::move(*offered_));
      } catch (...) {
        acceptor_->publish_exception(std::current_exception());
      }
      offered_ = nullptr;

      // Pessimistic notify (under a lock), so notify_one() suffices.
      write_done_.notify_one();
    }

    // Notify all writers to replace their implementation with acceptor_.
    write_ready_.notify_all();
  }

 private:
  auto get(const std::unique_lock<std::mutex>& lck) const
  noexcept
  -> std::conditional_t<std::is_const_v<T>,
      std::add_lvalue_reference_t<std::add_const_t<value_type>>,
      std::add_rvalue_reference_t<value_type>> {
    assert(lck.owns_lock() && lck.mutex() == &guard_);
    assert(offered_ != nullptr);
    if constexpr(std::is_const_v<T>)
      return *offered_;
    else
      return std::move(*offered_);
  }

  std::mutex guard_;
  std::condition_variable read_ready_;
  std::condition_variable write_ready_;
  std::condition_variable write_done_;
  std::add_pointer_t<T> offered_ = nullptr;
  std::exception_ptr exptr_ = nullptr;
  std::uintptr_t writer_count_ = 0;
  std::uintptr_t reader_count_ = 0;
  interlock_acceptor_intf<T>* acceptor_ = nullptr;
};

/**
 * \brief Objpipe for interlock.
 * \implements SourceConcept
 * \implements IocPushConcept
 * \ingroup objpipe_detail
 *
 * \details
 * The interlock pipe is able to read elements that are published by a \ref interlock_writer "writer".
 *
 * \tparam T The (possibly const qualified) type of elements in the objpipe.
 * \sa interlock_writer
 */
template<typename T>
class interlock_pipe {
 public:
  constexpr interlock_pipe() = default;

  explicit interlock_pipe(interlock_impl<T>* ptr)
  noexcept
  : ptr_(ptr)
  {
    if (ptr_ != nullptr) ptr_->inc_reader();
  }

  interlock_pipe(interlock_pipe&& other)
  noexcept
  : ptr_(std::exchange(other.ptr_, nullptr))
  {}

  auto operator=(interlock_pipe&& other)
  noexcept
  -> interlock_pipe& {
    using std::swap;
    swap(ptr_, other.ptr_);
    return *this;
  }

  interlock_pipe(const interlock_pipe& other) = delete;
  auto operator=(const interlock_pipe& other) -> interlock_pipe& = delete;

  ~interlock_pipe()
  noexcept {
    if (ptr_ != nullptr && ptr_->subtract_reader())
      delete ptr_;
  }

  friend auto swap(interlock_pipe& x, interlock_pipe& y)
  noexcept
  -> void {
    using std::swap;
    swap(x.ptr_, y.ptr_);
  }

  auto is_pullable()
  -> bool {
    assert(ptr_ != nullptr);
    return ptr_->is_pullable();
  }

  auto wait()
  -> objpipe_errc {
    assert(ptr_ != nullptr);
    return ptr_->wait();
  }

  auto front()
  -> decltype(auto) {
    assert(ptr_ != nullptr);
    return ptr_->front();
  }

  auto pop_front()
  -> decltype(auto) {
    assert(ptr_ != nullptr);
    return ptr_->pop_front();
  }

  auto pull()
  -> decltype(auto) {
    assert(ptr_ != nullptr);
    return ptr_->pull();
  }

  auto try_pull()
  -> decltype(auto) {
    assert(ptr_ != nullptr);
    return ptr_->try_pull();
  }

  constexpr auto can_push([[maybe_unused]] const existingthread_push& tag) const
  noexcept
  -> bool {
    return true;
  }

  constexpr auto can_push([[maybe_unused]] const multithread_unordered_push& tag) const
  noexcept
  -> bool {
    return true;
  }

  template<typename Acceptor>
  auto ioc_push(const existingthread_push& tag, Acceptor&& acceptor) &&
  -> void {
    assert(ptr_ != nullptr);
    ptr_->ioc_push(tag, std::forward<Acceptor>(acceptor));

    if (ptr_ != nullptr && ptr_->subtract_reader())
      delete ptr_;
    ptr_ = nullptr;
  }

  template<typename Acceptor>
  auto ioc_push(const multithread_unordered_push& tag, Acceptor&& acceptor) &&
  -> void {
    assert(ptr_ != nullptr);
    ptr_->ioc_push(tag, std::forward<Acceptor>(acceptor));

    if (ptr_ != nullptr && ptr_->subtract_reader())
      delete ptr_;
    ptr_ = nullptr;
  }

 private:
  interlock_impl<T>* ptr_ = nullptr;
};

/**
 * \brief Writer for the interlock.
 * \implements IocAcceptorConcept
 * \ingroup objpipe_detail
 *
 * \details
 * The writer publishes elements for the interlock_pipe to consume.
 *
 * The writer blocks until the object has been consumed.
 *
 * \note
 * Writers are not thread safe.
 * If you wish to use a writer across multiple threads, you should make a copy
 * for each thread (multiple copies share access to the receiving interlock_pipe).
 * (Alternatively, you can add external synchronization.)
 *
 * \tparam T The (possibly const qualified) type of elements in the objpipe.
 * \sa interlock_writer
 */
template<typename T>
class interlock_writer {
 public:
  interlock_writer() = default;

  explicit interlock_writer(interlock_impl<T>* ptr)
  noexcept
  : ptr_(ptr)
  {
    if (ptr_ != nullptr) ptr_->inc_writer();
  }

  interlock_writer(interlock_writer&& other)
  noexcept
  : ptr_(std::exchange(other.ptr_, nullptr))
  {}

  interlock_writer(const interlock_writer& other)
  noexcept
  : ptr_(other.ptr_ == nullptr ? nullptr : other.ptr_->select_on_writer_copy())
  {}

  auto operator=(const interlock_writer& other)
  noexcept
  -> interlock_writer& {
    return *this = interlock_writer(other);
  }

  auto operator=(interlock_writer&& other)
  noexcept
  -> interlock_writer& {
    using std::swap;
    swap(ptr_, other.ptr_);
    return *this;
  }

  ~interlock_writer()
  noexcept {
    if (ptr_ != nullptr && ptr_->subtract_writer())
      delete ptr_;
  }

  friend auto swap(interlock_writer& x, interlock_writer& y)
  noexcept
  -> void {
    using std::swap;
    swap(x.ptr_, y.ptr_);
  }

  template<typename Arg>
  auto operator()(Arg&& arg)
  -> void {
    assert(ptr_ != nullptr);
    objpipe_errc e = objpipe_errc::success;
    std::visit(
        [&e, this](auto publish_result) noexcept {
          if constexpr(std::is_same_v<objpipe_errc, decltype(publish_result)>) {
            e = publish_result;
          } else {
            auto old_ptr = std::exchange(ptr_, publish_result->select_on_writer_copy());
            if (old_ptr->subtract_writer()) delete old_ptr;
          }
        },
        ptr_->publish(std::forward<Arg>(arg)));
    if (e != objpipe_errc::success)
      throw objpipe_error(e);
  }

  template<typename Arg>
  auto operator()(Arg&& arg, objpipe_errc& e)
  -> void {
    assert(ptr_ != nullptr);
    e = objpipe_errc::success;
    std::visit(
        [&e, this](auto publish_result) noexcept {
          if constexpr(std::is_same_v<objpipe_errc, decltype(publish_result)>) {
            e = publish_result;
          } else {
            auto old_ptr = std::exchange(ptr_, publish_result->select_on_writer_copy());
            if (old_ptr->subtract_writer()) delete old_ptr;
          }
        },
        ptr_->publish(std::forward<Arg>(arg)));
  }

  auto push_exception(std::exception_ptr exptr)
  -> objpipe_errc {
    assert(ptr_ != nullptr);
    if (exptr == nullptr) throw std::invalid_argument("nullptr exception_ptr");
    return ptr_->publish_exception(std::move(exptr));
  }

  auto push_exception(std::exception_ptr exptr, objpipe_errc& e)
  -> void {
    assert(ptr_ != nullptr);
    if (exptr == nullptr) throw std::invalid_argument("nullptr exception_ptr");
    e = ptr_->publish_exception(std::move(exptr));
  }

 private:
  interlock_acceptor_intf<T>* ptr_ = nullptr;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_INTERLOCK_PIPE_H */
