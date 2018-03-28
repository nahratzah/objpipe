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


template<typename T>
class interlock_acceptor_intf {
 public:
  using value_type = std::remove_cv_t<std::remove_reference_t<T>>;

  virtual ~interlock_acceptor_intf() noexcept {}

  virtual auto publish(std::conditional_t<std::is_const_v<T>,
      std::add_lvalue_reference_t<std::add_const_t<value_type>>,
      std::add_rvalue_reference_t<value_type>> v)
  noexcept
  -> std::variant<objpipe_errc, interlock_acceptor_intf*> = 0;

  template<bool Enable = !std::is_const_v<T>>
  auto publish(std::add_lvalue_reference_t<std::add_const_t<value_type>> v)
  noexcept
  -> std::enable_if_t<Enable, std::variant<objpipe_errc, interlock_acceptor_intf*>> {
    return publish(T(v));
  }

  virtual auto publish_exception(std::exception_ptr exptr) noexcept -> objpipe_errc = 0;

  virtual auto inc_writer() noexcept -> void = 0;
  virtual auto subtract_writer() noexcept -> bool = 0;

  virtual auto select_on_writer_copy() -> interlock_acceptor_intf* {
    inc_writer();
    return this;
  }
};


template<typename T, typename Acceptor>
class interlock_acceptor_impl_shared final
: public interlock_acceptor_intf<T>
{
 public:
  using value_type = typename interlock_acceptor_intf<T>::value_type;

  explicit interlock_acceptor_impl_shared(Acceptor&& acceptor)
  noexcept(std::is_nothrow_move_constructible_v<Acceptor>)
  : acceptor_(std::move(acceptor))
  {}

  explicit interlock_acceptor_impl_shared(const Acceptor& acceptor)
  noexcept(std::is_nothrow_copy_constructible_v<Acceptor>)
  : acceptor_(std::move(acceptor))
  {}

  ~interlock_acceptor_impl_shared() noexcept override {}

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

  auto publish_exception(std::exception_ptr exptr)
  noexcept
  -> objpipe_errc override {
    std::lock_guard<std::mutex> lck{ mtx_ };

    acceptor_.push_exception(exptr);
    return objpipe_errc::success;
  }

  auto inc_writer()
  noexcept
  -> void override {
    writer_count_.fetch_add(1u, std::memory_order_acquire);
  }

  auto subtract_writer()
  noexcept
  -> bool override {
    return writer_count_.fetch_sub(1u, std::memory_order_release) == 1u;
  }

 private:
  std::mutex mtx_;
  Acceptor acceptor_;
  std::atomic<std::uintptr_t> writer_count_ = 0;
};


template<typename T, typename Acceptor>
class interlock_acceptor_impl_unordered final
: public interlock_acceptor_intf<T>
{
 public:
  using value_type = typename interlock_acceptor_intf<T>::value_type;

  explicit interlock_acceptor_impl_unordered(Acceptor&& acceptor)
  noexcept(std::is_nothrow_move_constructible_v<Acceptor>)
  : acceptor_(std::move(acceptor))
  {}

  explicit interlock_acceptor_impl_unordered(const Acceptor& acceptor)
  noexcept(std::is_nothrow_copy_constructible_v<Acceptor>)
  : acceptor_(std::move(acceptor))
  {}

  interlock_acceptor_impl_unordered(const interlock_acceptor_impl_unordered& rhs)
  noexcept(std::is_nothrow_copy_constructible_v<Acceptor>)
  : acceptor_(rhs.acceptor_)
  {}

  ~interlock_acceptor_impl_unordered() noexcept override {}

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

  auto publish_exception(std::exception_ptr exptr)
  noexcept
  -> objpipe_errc override {
    acceptor_.push_exception(exptr);
    return objpipe_errc::success;
  }

  auto inc_writer()
  noexcept
  -> void override {
    writer_count_.fetch_add(1u, std::memory_order_acquire);
  }

  auto subtract_writer()
  noexcept
  -> bool override {
    return writer_count_.fetch_sub(1u, std::memory_order_release) == 1u;
  }

  auto select_on_writer_copy() -> interlock_acceptor_impl_unordered* override {
    // Don't invoke parent, since default impl is for sharing.
    interlock_acceptor_impl_unordered* copy = new interlock_acceptor_impl_unordered(*this);
    copy->inc_writer();
    return copy;
  }

 private:
  Acceptor acceptor_;
  std::atomic<std::uintptr_t> writer_count_ = 0;
};


template<typename T>
class interlock_impl final
: public interlock_acceptor_intf<T>
{
 public:
  using value_type = typename interlock_acceptor_intf<T>::value_type;

 private:
  using front_type = std::conditional_t<std::is_const_v<T>,
        std::add_lvalue_reference_t<T>,
        std::add_rvalue_reference_t<T>>;
  using pull_type = value_type;

 public:
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
    for (;;) {
      if (exptr_ != nullptr)
        std::rethrow_exception(exptr_);
      if (offered_ != nullptr)
        return transport<front_type>(std::in_place_index<0>, get(lck_));
      if (writer_count_ == 0)
        return transport<front_type>(std::in_place_index<1>, objpipe_errc::closed);
      read_ready_.wait(lck_);
    }
  }

  auto pop_front()
  -> objpipe_errc {
    std::unique_lock<std::mutex> lck_{ guard_ };
    while (offered_ == nullptr && exptr_ == nullptr) {
      if (writer_count_ == 0) return objpipe_errc::closed;
      read_ready_.wait(lck_);
    }

    if (exptr_ != nullptr)
      std::rethrow_exception(exptr_);

    offered_ = nullptr;
    lck_.unlock();
    write_ready_.notify_one();
    write_done_.notify_one();
    return objpipe_errc::success;
  }

  auto pull()
  -> transport<pull_type> {
    std::unique_lock<std::mutex> lck_{ guard_ };
    while (offered_ == nullptr && exptr_ == nullptr) {
      if (writer_count_ == 0)
        return transport<pull_type>(std::in_place_index<1>, objpipe_errc::closed);
      read_ready_.wait(lck_);
    }

    if (exptr_ != nullptr)
      std::rethrow_exception(exptr_);

    auto result = transport<pull_type>(std::in_place_index<0>, get(lck_));
    offered_ = nullptr;
    lck_.unlock();
    write_ready_.notify_one();
    write_done_.notify_one();
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
    write_done_.notify_one();
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
      if constexpr(std::is_const_v<T>)
        acceptor_->publish(v);
      else
        acceptor_->publish(std::move(v));
      return acceptor_;
    }

    if (exptr_ != nullptr) return objpipe_errc::bad;
    if (reader_count_ == 0) return objpipe_errc::closed;
    if (exptr_ != nullptr) return objpipe_errc::closed;

    std::add_pointer_t<T> v_ptr = nullptr;
    if constexpr(std::is_const_v<T>)
      v_ptr = std::addressof(v);
    else
      v_ptr = std::addressof(static_cast<std::add_lvalue_reference_t<value_type>>(v));
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
    if (reader_count_ == 0) return objpipe_errc::closed;
    if (exptr_ != nullptr) return objpipe_errc::bad;
    exptr_ = std::move(exptr);

    lck_.unlock();
    read_ready_.notify_all();
    write_ready_.notify_all(); // Notify all writers that objpipe went bad.
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
      } catch (...) {
        acceptor_->publish_exception(std::current_exception());
      }
      offered_ = nullptr;
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

  constexpr auto can_push([[maybe_unused]] existingthread_push tag) const
  noexcept
  -> bool {
    return true;
  }

  constexpr auto can_push([[maybe_unused]] multithread_unordered_push tag) const
  noexcept
  -> bool {
    return true;
  }

  template<typename Acceptor>
  auto ioc_push(existingthread_push tag, Acceptor&& acceptor) &&
  -> void {
    assert(ptr_ != nullptr);
    ptr_->ioc_push(tag, std::forward<Acceptor>(acceptor));

    if (ptr_ != nullptr && ptr_->subtract_reader())
      delete ptr_;
    ptr_ = nullptr;
  }

  template<typename Acceptor>
  auto ioc_push(multithread_unordered_push tag, Acceptor&& acceptor) &&
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
