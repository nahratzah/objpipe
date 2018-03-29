#ifndef OBJPIPE_DETAIL_THREAD_POOL_H
#define OBJPIPE_DETAIL_THREAD_POOL_H

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <objpipe/detail/invocable_.h>

namespace objpipe::detail {


///\brief Thread pool task interface.
///\ingroup objpipe_detail
class thread_pool_task {
 public:
  virtual ~thread_pool_task() noexcept {}
  virtual auto operator()() -> void = 0;
};

///\brief Thread pool task implementation.
///\ingroup objpipe_detail
template<typename F>
class thread_pool_task_impl final
: public thread_pool_task
{
 public:
  explicit thread_pool_task_impl(F&& f)
  noexcept(std::is_nothrow_move_constructible_v<F>)
  : f_(std::move(f))
  {}

  explicit thread_pool_task_impl(const F& f)
  noexcept(std::is_nothrow_copy_constructible_v<F>)
  : f_(f)
  {}

  thread_pool_task_impl(const thread_pool_task_impl&) = delete;

  ~thread_pool_task_impl() noexcept override {}

  auto operator()()
  noexcept(is_nothrow_invocable_v<F>)
  -> void override {
    std::invoke(std::move(f_));
  }

 private:
  F f_;
};

///\brief Thread pool task that completes a future.
///\ingroup objpipe_detail
template<typename F>
class thread_pool_task_future final
: public thread_pool_task
{
 public:
  using functor_result = std::remove_cv_t<std::remove_reference_t<invoke_result_t<F>>>;
  using promise_type = std::promise<functor_result>;

  ///\brief Constructor.
  ///\param[in] prom The promise to complete after running the functor.
  ///\param[in] f The functor to run.
  explicit thread_pool_task_future(promise_type&& prom, F&& f)
  noexcept(std::is_nothrow_move_constructible_v<F>)
  : f_(std::move(f)),
    prom_(std::move(prom))
  {}

  ///\brief Constructor.
  ///\param[in] prom The promise to complete after running the functor.
  ///\param[in] f The functor to run.
  explicit thread_pool_task_future(promise_type&& prom, const F& f)
  noexcept(std::is_nothrow_copy_constructible_v<F>)
  : f_(f),
    prom_(std::move(prom))
  {}

  ~thread_pool_task_future() noexcept {}

  ///\brief Invokes the callable and completes the future with the result.
  auto operator()()
  noexcept(is_nothrow_invocable_v<F>)
  -> void {
    try {
      if constexpr(std::is_same_v<void, functor_result>) {
        this->thread_pool_task_impl<F>::operator()();
        prom_.set_value();
      } else {
        prom_.set_value(this->thread_pool_task_impl<F>::operator()());
      }
    } catch (...) {
      prom_.set_exception(std::current_exception());
    }
  }

 private:
  F f_;
  promise_type prom_;
};

/**
 * \brief Implementation of the thread pool.
 * \ingroup objpipe_detail
 * \details
 * The thread pool manages a number of threads, which execute tasks given
 * to the pool.
 *
 * \bug The thread pool is probably inefficient if given lots of really fast
 * to complete tasks, as it uses a single mutex shared between task-publishing
 * and worker threads.
 */
class thread_pool_impl {
 public:
  ///\brief Duration type for thread expiry.
  using duration_t = std::chrono::milliseconds;
  ///\brief Pointer type for tasks.
  using task_ptr = std::unique_ptr<thread_pool_task>;

 private:
  ///\brief Constructor.
  ///\param[in] thr_expire Idle expiry timeout for threads.
  ///\param[in] max_threads Max number of threads. A value of 0 is silently treated as 1.
  thread_pool_impl(duration_t thr_expire, unsigned int max_threads)
  : max_threads_(std::max(1u, max_threads)),
    thr_expire_(std::max(thr_expire, duration_t(0)))
  {}

  ///\brief Deleter for external usage of thread_pool_impl.
  ///\details Clears the live bit, as only external users can publish tasks.
  static auto on_delete_(thread_pool_impl* self) noexcept
  -> void {
    std::unique_lock<std::mutex> lck{ self->mtx_ };
    self->live_ = false;
    if (self->num_threads_ == 0) {
      lck.unlock();
      delete self;
      return;
    }

    self->wakeup_.notify_all();
  }

 public:
  ///\brief Create a thread pool.
  ///\details Number of threads is set to std::thread::hardware_concurrency.
  ///\param[in] thr_expire Idle expiry timeout for threads.
  ///\returns A shared pointer to the thread_pool_impl.
  static auto create(duration_t thr_expire)
  -> std::shared_ptr<thread_pool_impl> {
    return create(thr_expire, std::thread::hardware_concurrency());
  }

  ///\brief Create a thread pool.
  ///\param[in] thr_expire Idle expiry timeout for threads.
  ///\param[in] max_threads Number of threads in the pool.
  ///\returns A shared pointer to the thread_pool_impl.
  static auto create(duration_t thr_expire, unsigned int max_threads)
  -> std::shared_ptr<thread_pool_impl> {
    return std::shared_ptr<thread_pool_impl>(
        new thread_pool_impl(thr_expire, max_threads),
        &thread_pool_impl::on_delete_);
  }

  ///\brief Publish a task.
  auto publish(task_ptr task)
  -> void {
    std::unique_lock<std::mutex> lck{ mtx_ };
    const bool skip_notify = maybe_start_thread_(lck);
    assert(lck.owns_lock());
    tasks_.push_back(std::move(task));

    lck.unlock();
    if (!skip_notify) wakeup_.notify_one();
  }

  ///\brief Change the number of threads in the thread pool.
  ///\param[in] max_threads New limit on number of threads in the pool.
  ///\note Zero is a magic value, to use std::thread::hardware_concurrency.
  ///If hardware_concurrency is zero (i.e. the c++ library cannot determine the
  ///number of processors in the system), a value of 1 processor is assumed.
  ///Consequently, the thread pool can never fall below 1 max threads.
  auto max_threads(unsigned int max_threads) {
    if (max_threads == 0) max_threads = std::thread::hardware_concurrency();

    std::unique_lock<std::mutex> lck{ mtx_ };
    max_threads_ = std::max(1u, max_threads);

    // If we have excess threads, notify the excess number of threads to
    // recheck their lifetime.
    if (num_threads_ > max_threads_) {
      for (unsigned int i = max_threads_; i < num_threads_; ++i)
        wakeup_.notify_one();
    }

    // If we have room to spare, create more threads to handle all outstanding
    // tasks.
    if (num_threads_ < max_threads_ && tasks_.size() > idle_threads_) {
      const unsigned int slack = max_threads_ - num_threads_;
      const auto need_more = tasks_.size() - idle_threads_;
      for (unsigned int i = 0; i < slack && i < need_more; ++i)
        maybe_start_thread_(lck);
    }
  }

  ///\brief Retrieve the number of threads.
  auto max_threads() const
  noexcept
  -> unsigned int {
    std::lock_guard<std::mutex> lck{ mtx_ };
    return max_threads_;
  }

  ///\brief Change expire timer of threads.
  ///\param[in] thr_expire Threads end if idle in excess of the duration.
  ///\note Negative durations are silently treated as 0.
  auto thr_expire(duration_t thr_expire)
  noexcept
  -> void {
    std::lock_guard<std::mutex> lck{ mtx_ };
    thr_expire_ = std::max(thr_expire, duration_t(0));
  }

  ///\brief Query the expire timer of threads.
  ///\returns The duration for which a worker thread will sleep with no
  ///work pending, until it stops itself.
  auto thr_expire() const
  noexcept
  -> duration_t {
    std::lock_guard<std::mutex> lck{ mtx_ };
    return thr_expire_;
  }

 private:
  ///\brief Maybe start a new thread.
  ///\details
  ///Only starts a new thread if there is room for another thread,
  ///and the number of unclaimed tasks is greater than or equal to the number of idle threads.
  ///
  ///\note The latter condition looks like an off-by-one, but it's correct
  ///as this method is to be invoked *prior* to a new task being enqueued.
  ///\param[in] lck Lock. Only used to verify the lock is held.
  ///\returns True if a new thread was started, false otherwise.
  auto maybe_start_thread_([[maybe_unused]] const std::unique_lock<std::mutex>& lck)
  -> bool {
    assert(lck.owns_lock() && lck.mutex() == &mtx_);
    // Don't create threads in excess of max_threads_.
    if (num_threads_ >= max_threads_) return false;
    // Don't create a new thread if there's an idle thread available.
    if (tasks_.size() < idle_threads_) return false;

    // Start a thread and detach it.
    std::thread(&thread_pool_impl::worker_, this)
        .detach();
    ++num_threads_;
    return true;
  }

  ///\brief Worker invocation.
  static auto worker_(thread_pool_impl* self)
  -> void {
    const bool delete_self = self->worker_loop_();

    // Yes, we leak in the case of an exception.
    // Note however, that std::thread will terminate the application
    // in the case of an exception.
    // So it should be fine.
    if (delete_self) delete self;
  }

  ///\brief Worker task.
  ///\details Takes tasks from the tasks_ list and executes them.
  ///\returns True if the thread_pool_impl is to be destroyed by the caller.
  auto worker_loop_()
  -> bool {
    std::unique_lock<std::mutex> lck{ mtx_ };
    for (;;) {
      assert(lck.owns_lock());
      {
        // Nested scope, to publish thread as idle only while waiting.
        idle_thread_ publish_idle_thread{ *this, lck };
        if (!wakeup_.wait_for(
                lck,
                thr_expire_,
                [this]() {
                  return !tasks_.empty() || !live_ || num_threads_ > max_threads_;
                })) {
          break; // Idle for too long, end this thread.
        }
      }

      // Clean up once all threads are gone.
      if (tasks_.empty() && !live_) break;

      // Stop running if number of threads is excessive.
      // (Only happens if max_threads_ was adjusted downward while running.)
      if (num_threads_ > max_threads_) break;

      // Claim a task.
      task_ptr task = std::move(tasks_.front());
      tasks_.pop_front();

      // Run a task, without holding the lock.
      lck.unlock();
      (*task)();
      lck.lock();
    }

    assert(lck.owns_lock());
    --num_threads_;
    return num_threads_ == 0 && !live_;
  }

  ///\brief Scoped object that increments the idle_thread_ counter while it exists.
  ///\ingroup objpipe_detail
  class idle_thread_ {
   public:
    idle_thread_(thread_pool_impl& self, std::unique_lock<std::mutex>& lck)
    noexcept
    : self_(self),
      lck_(lck)
    {
      assert(lck_.owns_lock() && lck_.mutex() == &self_.mtx_);
      ++self_.idle_threads_;
    }

    ~idle_thread_() noexcept {
      assert(lck_.mutex() == &self_.mtx_);
      if (!lck_.owns_lock()) lck_.lock();
      --self_.idle_threads_;
    }

    idle_thread_(const idle_thread_&) = delete;

   private:
    thread_pool_impl& self_;
    std::unique_lock<std::mutex>& lck_;
  };

  ///\brief Lock protecting member variables.
  mutable std::mutex mtx_;
  ///\brief Wakeup condition for worker threads.
  ///\details Signalled when more work is available,
  ///or parameters change that necessitate threads ending.
  std::condition_variable wakeup_;
  ///\brief List of outstanding tasks.
  std::deque<task_ptr> tasks_;
  ///\brief Indicator that new tasks can be posted.
  ///\details Cleared by the destructor of the shared pointer managing the thread_pool_impl.
  bool live_ = true;
  ///\brief Active number of threads.
  unsigned int num_threads_ = 0;
  ///\brief Max number of threads.
  unsigned int max_threads_;
  ///\brief Number of threads waiting for a wakeup signal.
  ///\details Note that this value is changed from within the thread,
  ///thus it must be compared against tasks_ size.
  unsigned int idle_threads_ = 0;
  ///\brief Idle thread timeout.
  ///\details If a thread is idle in excess of this timeout, it ends.
  duration_t thr_expire_;
};

/**
 * \brief Thread pool.
 * \ingroup objpipe_detail
 * \details
 * A thread pool contains one or more threads that execute tasks given to it.
 *
 * \note Threads in the thread pool are run in detached mode.
 * Their life will thus not affect the life of the application,
 * but application termination will also not properly unwind the threads.
 * If a task must complete in these cases, you should not use thread pool,
 * or use thread_pool::publish_future and wait for its future to complete.
 */
class thread_pool {
 public:
  using duration_t = thread_pool_impl::duration_t;

  ///\brief Default time limit on how long threads are allowed to be idle.
  static constexpr auto default_thr_expire = std::chrono::minutes(1);

  ///\brief Create a new thread pool.
  thread_pool()
  : pimpl_(thread_pool_impl::create(default_thr_expire))
  {}

  ///\brief Create a thread pool.
  ///\param[in] thr_expire Thread idle timeout for the thread pool.
  ///  Threads idle in excess of this time, will end.
  thread_pool(duration_t thr_expire)
  : pimpl_(thread_pool_impl::create(thr_expire))
  {}

  ///\brief Create a thread pool.
  ///\param[in] thr_expire Thread idle timeout for the thread pool.
  ///  Threads idle in excess of this time, will end.
  ///\param[in] max_threads Max number of threads in the thread pool.
  ///  Threads are created as needed.
  thread_pool(duration_t thr_expire, unsigned int max_threads)
  : pimpl_(thread_pool_impl::create(thr_expire, max_threads))
  {}

  ///\brief Default thread pool for objpipe threads.
  static auto default_pool()
  -> thread_pool {
    static thread_pool impl;
    return impl;
  }

  /**
   * \brief Run the functor in the thread pool.
   * \details
   * Functor \p f will be invoked by the thread pool exactly once.
   *
   * \note
   * If \p f throws an exception when run or destroyed,
   * std::terminate will be called.
   */
  template<typename F>
  auto publish(F&& f)
  -> void {
    pimpl_->publish(
        std::make_unique<thread_pool_task_impl<std::decay_t<F>>>(std::forward<F>(f)));
  }

  /**
   * \brief Run the functor in the thread pool.
   * \details
   * Functor \p f will be invoked by the thread pool exactly once.
   *
   * The result of invoking \p f will be made available in the returned future.
   * \returns A future with the result of invoking \p f.
   */
  template<typename F>
  auto publish_with_future(F&& f)
  -> std::future<invoke_result_t<std::decay_t<F>>> {
    using type = std::remove_cv_t<std::remove_reference_t<invoke_result_t<std::decay_t<F>>>>;

    std::promise<type> p;
    std::future<type> result = p.get_future();

    pimpl_->publish(
        std::make_unique<thread_pool_task_future<std::decay_t<F>>>(std::move(p), std::forward<F>(f)));
    return result;
  }

  ///\brief Retrieve the max number of threads in the thread pool.
  ///\returns The max number of threads in the thread pool.
  auto max_threads() const
  noexcept
  -> unsigned int {
    return pimpl_->max_threads();
  }

  ///\brief Set the max number of threads in the thread pool.
  ///\param[in] n The max number of threads in the pool.
  ///   A magic value of 0 indicates to use std::thread::hardware_concurrency
  ///   to select an appropriate number.
  auto max_threads(unsigned int n) const
  noexcept
  -> void {
    pimpl_->max_threads(n);
  }

  ///\brief Retrieve thread idle timeout.
  ///\details Threads idle in excess of the timeout are ended.
  ///\returns Idle timeout for threads.
  auto thr_expire() const noexcept
  -> duration_t {
    return pimpl_->thr_expire();
  }

  ///\brief Set thread idle timeout.
  ///\details Threads idle in excess of the timeout are ended.
  ///\note Implementation detail:
  ///   the timeout does not affect threads that are currently idle.
  ///   Only threads that become idle after this function completes,
  ///   are subject to the new timeout.
  ///\param[in] timeout Thread idle timeout. Negative durations are silently treated as 0.
  auto thr_expire(duration_t timeout) const noexcept
  -> void {
    pimpl_->thr_expire(timeout);
  }

 private:
  ///\brief Shared pointer to implementation.
  std::shared_ptr<thread_pool_impl> pimpl_;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_THREAD_POOL_H */
