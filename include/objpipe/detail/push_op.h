#ifndef OBJPIPE_DETAIL_PUSH_OP_H
#define OBJPIPE_DETAIL_PUSH_OP_H

///\file
///\ingroup objpipe_detail
///\brief Operations for push based objpipe.

#include <exception>
#include <future>
#include <list>
#include <type_traits>
#include <stdexcept>
#include <limits>
#include <utility>
#include <vector>
#include <objpipe/push_policies.h>
#include <objpipe/detail/adapt.h>
#include <objpipe/detail/invocable_.h>

namespace objpipe::detail {


#if 0 // XXX example sink
template<typename Sink>
class push_adapter_t {
 public:
  constexpr push_adapter_t(Sink&& sink)
  : sink_(std::move(sink))
  {}

  // Required
  template<typename T>
  auto operator()(T&& v)
  noexcept(noexcept(std::declval<Sink&>()(std::declval<T>())))
  -> objpipe_errc {
    return sink_(v);
  }

  // Required
  template<typename T>
  auto push_exception(std::exception_ptr&& p)
  noexcept // May not throw
  -> void {
    sink_.push_exception(std::move(p));
  }

  ~push_adapter_t() noexcept {
#if 0
    try {
      sink_.close();
    } catch (...) {
      push_exception(std::current_exception());
    }
#endif
  }
};
#endif


namespace adapt_async {


/**
 * \brief Asynchronous reduction operation.
 * \ingroup objpipe_detail
 * \details
 * Provides acceptor implementations for a given reduction.
 *
 * \tparam ObjpipeVType Value type of the objpipe.
 * \tparam Init Functor returning a reduction state.
 * \tparam Acceptor
 * Functor that accepts values (second argument, by rvalue reference)
 * and adds them to the reduction state (first argument, by lvalue reference).
 * \tparam Merger
 * Functor that merges the reduction state (second argument, by rvalue reference)
 * into another reduction state (first argument, by lvalue reference).
 * \tparam Extractor
 * Functor that extracts the result of a reduction from the reduction state
 * (passed by rvalue reference).
 */
template<typename ObjpipeVType, typename Init, typename Acceptor, typename Merger, typename Extractor>
class promise_reducer {
 public:
  ///\brief Value type of the underlying source.
  using objpipe_value_type = ObjpipeVType;
  ///\brief Type of the reduction state.
  using state_type = std::remove_cv_t<std::remove_reference_t<decltype(std::declval<Init>()())>>;
  ///\brief Type returned by the reduction.
  using value_type = std::remove_cv_t<std::remove_reference_t<decltype(std::declval<Extractor>()(std::declval<state_type>()))>>;

  ///\brief Constructor.
  ///\param[in] prom The promise that will receive the result of the reduction.
  ///\param[in] init Implementation of the Init functor.
  ///\param[in] acceptor Implementation of the Acceptor functor.
  ///\param[in] merger Implementation of the Merger functor.
  ///\param[in] extractor Implementation of the Extractor functor.
  template<typename InitArg, typename AcceptorArg, typename MergerArg, typename ExtractorArg>
  promise_reducer(std::promise<value_type>&& prom, InitArg&& init, AcceptorArg&& acceptor, MergerArg&& merger, ExtractorArg&& extractor)
  : prom_(std::move(prom)),
    init_(std::forward<InitArg>(init)),
    acceptor_(std::forward<AcceptorArg>(acceptor)),
    merger_(std::forward<MergerArg>(merger)),
    extractor_(std::forward<ExtractorArg>(extractor))
  {}

 private:
  ///\brief Shared state for unordered reductions.
  ///\ingroup objpipe_detail
  ///\details
  ///The shared state holds data common to all threads participating in
  ///a \ref multithread_unordered_push "unordered, multithreaded" reduction.
  class unordered_shared_state {
   public:
    using state_type = promise_reducer::state_type;
    using acceptor_type = Acceptor;

    unordered_shared_state(std::promise<value_type>&& prom, Init&& init, Acceptor&& acceptor, Merger&& merger, Extractor&& extractor)
    noexcept(std::is_nothrow_copy_constructible_v<std::promise<value_type>>
        && std::is_nothrow_copy_constructible_v<Acceptor>
        && std::is_nothrow_copy_constructible_v<Merger>
        && std::is_nothrow_copy_constructible_v<Extractor>)
    : prom_(std::move(prom)),
      init_(std::move(init)),
      acceptor_(std::move(acceptor)),
      merger_(std::move(merger)),
      extractor_(std::move(extractor))
    {}

    unordered_shared_state(unordered_shared_state&&) = delete;
    unordered_shared_state(const unordered_shared_state&) = delete;
    unordered_shared_state& operator=(unordered_shared_state&&) = delete;
    unordered_shared_state& operator=(const unordered_shared_state&) = delete;

    ~unordered_shared_state() noexcept {
      if (bad_.load(std::memory_order_acquire)) {
        prom_.set_exception(std::move(exptr_));
      } else {
        try {
          if (pstate_.has_value()) { // Should only fail to hold, if constructors fail.
            if constexpr(std::is_same_v<void, value_type>) {
              std::invoke(std::move(extractor_), std::move(*pstate_));
              prom_.set_value();
            } else {
              prom_.set_value(std::invoke(std::move(extractor_), std::move(*pstate_)));
            }
          }
        } catch (...) {
          prom_.set_exception(std::current_exception());
        }
      }
    }

    bool is_bad() const noexcept {
      return bad_.load(std::memory_order_relaxed);
    }

    auto push_exception(std::exception_ptr exptr)
    noexcept
    -> void {
      bool expect_bad = false;
      if (bad_.compare_exchange_strong(expect_bad, true, std::memory_order_acq_rel, std::memory_order_relaxed))
        exptr_ = std::move(exptr);
    }

    auto publish(state_type&& state)
    noexcept
    -> void {
      if (is_bad()) return; // Discard if objpipe went bad.

      try {
        std::unique_lock<std::mutex> lck{ mtx_ };

        for (;;) {
          if (!pstate_.has_value()) {
            pstate_.emplace(std::move(state));
            return;
          }

          state_type merge_into_ = std::move(pstate_).value();
          pstate_.reset();

          // Release lock during merge operation.
          lck.unlock();
          std::invoke(merger(), state, std::move(merge_into_));
          if (is_bad()) return; // Discard if objpipe went bad.
          lck.lock();
        }
      } catch (...) {
        push_exception(std::current_exception());
      }
    }

    auto new_state() const
    noexcept(is_nothrow_invocable_v<const Init&>)
    -> state_type {
      return std::invoke(init());
    }

    auto new_state([[maybe_unused]] const state_type& s) const
    noexcept(is_nothrow_invocable_v<const Init&>)
    -> state_type {
      return new_state();
    }

    auto init() const
    noexcept
    -> const Init& {
      return init_;
    }

    auto acceptor() const
    noexcept
    -> const acceptor_type& {
      return acceptor_;
    }

    auto merger() const
    noexcept
    -> const Merger& {
      return merger_;
    }

   private:
    std::atomic<bool> bad_{ false };
    std::mutex mtx_;
    std::optional<state_type> pstate_;
    std::promise<value_type> prom_;
    Init init_;
    acceptor_type acceptor_;
    Merger merger_;
    Extractor extractor_;
    std::exception_ptr exptr_;
  };

  ///\brief Shared state for ordered reductions.
  ///\ingroup objpipe_detail
  ///\details
  ///The shared state holds data common to all threads participating in
  ///a \ref multithread_push "ordered, multithreaded" reduction.
  class ordered_shared_state {
   private:
    ///\brief Internal state elements of the reducer.
    class internal_state {
     public:
      ///\brief Constructor.
      internal_state(promise_reducer::state_type&& state)
      noexcept(std::is_nothrow_move_constructible_v<promise_reducer::state_type>)
      : state(std::move(state))
      {}

      internal_state(const internal_state&) = delete;
      internal_state(internal_state&&) = delete;
      internal_state& operator=(const internal_state&) = delete;
      internal_state& operator=(internal_state&&) = delete;

      ///\brief Reducer state.
      promise_reducer::state_type state;
      ///\brief Indicates if this reducer state has completed.
      bool ready = false;
    };

    ///\brief List of internal states.
    ///\note Must be a list, to prevent iterator invalidation.
    using internal_state_list = std::list<internal_state>;

   public:
    ///\brief Exposed state type.
    ///\details By exposing the iterator, we can merge with siblings at
    ///completion of this shard of the reduction.
    using state_type = typename internal_state_list::iterator;

    ///\brief Wrap an adapter, so it works on internal_state.
    ///\ingroup objpipe_detail
    class acceptor_type {
     public:
      ///\brief Constructor.
      ///\param[in] impl The acceptor that is to be wrapped.
      acceptor_type(Acceptor&& impl)
      noexcept(std::is_nothrow_move_constructible_v<Acceptor>)
      : impl_(std::move(impl))
      {}

      ///\brief Wraps invocation of Acceptor.
      template<typename Arg>
      auto operator()(state_type& s, Arg&& v) const
      noexcept(is_nothrow_invocable_v<Acceptor, state_type&, Arg>)
      -> objpipe_errc {
        return std::invoke(impl_, s->state, std::forward<Arg>(v));
      }

     private:
      ///\brief Wrapped acceptor implementation.
      Acceptor impl_;
    };

    ///\brief Create the shared state.
    ordered_shared_state(std::promise<value_type>&& prom, Init&& init, Acceptor&& acceptor, Merger&& merger, Extractor&& extractor)
    noexcept(std::is_nothrow_copy_constructible_v<std::promise<value_type>>
        && std::is_nothrow_copy_constructible_v<Acceptor>
        && std::is_nothrow_copy_constructible_v<Merger>
        && std::is_nothrow_copy_constructible_v<Extractor>)
    : prom_(std::move(prom)),
      init_(std::move(init)),
      acceptor_(std::move(acceptor)),
      merger_(std::move(merger)),
      extractor_(std::move(extractor))
    {}

    ordered_shared_state(ordered_shared_state&&) = delete;
    ordered_shared_state(const ordered_shared_state&) = delete;
    ordered_shared_state& operator=(ordered_shared_state&&) = delete;
    ordered_shared_state& operator=(const ordered_shared_state&) = delete;

    ///\brief Destructor.
    ///\details Publishes the reduction result, if it completed properly.
    ~ordered_shared_state() noexcept {
      if (bad_.load(std::memory_order_acquire)) {
        prom_.set_exception(std::move(exptr_));
      } else {
        try {
          if (pstate_.size() == 1u && pstate_.front().ready) { // Should only fail to hold, if constructors fail.
            if constexpr(std::is_same_v<void, value_type>) {
              std::invoke(std::move(extractor_), std::move(pstate_.front().state));
              prom_.set_value();
            } else {
              prom_.set_value(std::invoke(std::move(extractor_), std::move(pstate_.front().state)));
            }
          }
        } catch (...) {
          prom_.set_exception(std::current_exception());
        }
      }
    }

    ///\brief Test if any of the reduction shards pushed an exception.
    ///\details If the reduction is bad, there's no point in continuing and it's best to abort quickly.
    ///\returns True iff an exception was pushed.
    bool is_bad() const noexcept {
      return bad_.load(std::memory_order_relaxed);
    }

    ///\brief Push an exception into the result.
    auto push_exception(std::exception_ptr exptr)
    noexcept
    -> void {
      bool expect_bad = false;
      if (bad_.compare_exchange_strong(expect_bad, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        try {
          prom_.set_exception(std::move(exptr));
        } catch (const std::future_error& e) {
          if (e.code() == std::future_errc::promise_already_satisfied) {
            /* SKIP (swallow exception) */
          } else { // Only happens if the promise is in a bad state.
            throw; // aborts, because noexcept, this is intentional
          }
        }
      }
    }

    ///\brief Mark the given \p state as ready.
    ///\details Marks the state ready and performs ordered reduction with sibling states that are ready.
    auto publish(state_type state)
    noexcept
    -> void {
      if (is_bad()) return; // Discard if objpipe went bad.

      try {
        std::unique_lock<std::mutex> lck{ mtx_ };

        for (;;) { // Note: state is an iterator in pstate_.
          assert(!state->ready);

          if (state != pstate_.begin() && std::prev(state)->ready) {
            state_type predecessor = std::prev(state);
            predecessor->ready = false;

            // Perform merge with lock released.
            lck.unlock();
            std::invoke(merger(), predecessor->state, std::move(state->state));
            if (is_bad()) return; // Discard if objpipe went bad.
            lck.lock();

            // Erase state, as it is merged into predecessor.
            // Continue by trying to further merge predecessor.
            pstate_.erase(state);
            state = predecessor;
          } else if (std::next(state) != pstate_.end() && std::next(state)->ready) {
            state_type successor = std::next(state);
            successor->ready = false;

            // Perform merge with lock released.
            lck.unlock();
            std::invoke(merger(), state->state, std::move(successor->state));
            if (is_bad()) return; // Discard if objpipe went bad.
            lck.lock();

            // Erase successor, as it is merged into state.
            // Continue by trying to further merge state.
            pstate_.erase(successor);
          } else {
            state->ready = true;
            return;
          }
        }
      } catch (...) {
        push_exception(std::current_exception());
      }
    }

    ///\brief Create the state for a new reducer shard.
    ///\note This function may only be called once, for the initial state.
    auto new_state()
    -> state_type {
      std::lock_guard<std::mutex> lck{ mtx_ };

      return pstate_.emplace(pstate_.end(), std::invoke(init()));
    }

    ///\brief Create a state for a new reducer shard, positioned after \p pos.
    ///\note This function is used to construct sibling states.
    auto new_state(const state_type& pos)
    -> state_type {
      std::lock_guard<std::mutex> lck{ mtx_ };

      return pstate_.emplace(std::next(pos), std::invoke(init()));
    }

    ///\brief Reference to state factory functor.
    auto init() const
    noexcept
    -> const Init& {
      return init_;
    }

    ///\brief Reference to acceptor functor.
    auto acceptor() const
    noexcept
    -> const acceptor_type& {
      return acceptor_;
    }

    ///\brief Reference to merger functor.
    auto merger() const
    noexcept
    -> const Merger& {
      return merger_;
    }

   private:
    ///\brief Flag that marks the reducer as bad.
    std::atomic<bool> bad_{ false };
    ///\brief Mutex to protect pstate_.
    std::mutex mtx_;
    ///\brief Ordered list of shards.
    internal_state_list pstate_;
    ///\brief Promise to make ready at the end of the reduction.
    std::promise<value_type> prom_;
    ///\brief Initialization functor.
    ///\note Use init() to access, for const correctness.
    Init init_;
    ///\brief Acceptor functor.
    ///\note Use acceptor() to access, for const correctness.
    acceptor_type acceptor_;
    ///\brief Merger functor.
    ///\note Use merger() to access, for const correctness.
    Merger merger_;
    ///\brief Extractor functor.
    Extractor extractor_;
    ///\brief Pending exception.
    std::exception_ptr exptr_;
  };

  ///\brief Reducer state for multithread push.
  ///\implements IocAcceptorConcept
  ///\ingroup objpipe_detail
  ///\details
  ///This reducer state has logic to allow multiple copies to cooperate
  ///in a reduction.
  template<typename SharedState>
  class local_state {
   public:
    using state_type = typename SharedState::state_type;
    using acceptor_type = typename SharedState::acceptor_type;

   private:
    ///\brief Initializing constructor.
    ///\details Initializes the local state using \p sptr.
    ///\param[in] sptr A newly constructed shared state.
    explicit local_state(std::shared_ptr<SharedState> sptr)
    noexcept(std::is_nothrow_constructible_v<state_type, decltype(std::declval<SharedState&>().new_state())>
        && noexcept(std::declval<SharedState&>().new_state())
        && std::is_nothrow_move_constructible_v<state_type>
        && is_nothrow_invocable_v<Init>)
    : state_(sptr->new_state()),
      sptr_(std::move(sptr))
    {}

    ///\brief Sibling constructor.
    ///\details Used to construct a sibling state.
    ///The sibling state is positioned after \p s.
    explicit local_state(std::shared_ptr<SharedState> sptr, const state_type& s)
    noexcept(std::is_nothrow_constructible_v<state_type, decltype(std::declval<SharedState&>().new_state(std::declval<const state_type&>()))>
        && noexcept(std::declval<SharedState&>().new_state(std::declval<const state_type&>()))
        && std::is_nothrow_move_constructible_v<state_type>
        && is_nothrow_invocable_v<Init>)
    : state_(sptr->new_state(s)),
      sptr_(std::move(sptr))
    {}

   public:
    ///\brief Construct a new local state.
    ///\param[in] prom The promise to fulfill at the end of the reduction.
    ///\param[in] init A functor constructing an initial reduction-state.
    ///\param[in] acceptor A functor combining values into the reduction state.
    ///\param[in] merger A functor merging the right-hand-side reduction-state into the left-hand-side reduction-state.
    ///\param[in] extractor A functor to retrieve the reduce outcome from the reduction-state.
    local_state(std::promise<value_type> prom,
        Init&& init,
        Acceptor&& acceptor,
        Merger&& merger,
        Extractor&& extractor)
    : local_state(std::make_shared<SharedState>(
            std::move(prom),
            std::move(init),
            std::move(acceptor),
            std::move(merger),
            std::move(extractor)))
    {}

    ///\brief Copy constructor creates a sibling state.
    ///\details Sibling state is positioned directly after rhs.
    ///\note If the shared state is unordered, positional information is not maintained.
    local_state(const local_state& rhs)
    : local_state(rhs.sptr_, rhs.state_)
    {}

    ///\brief Move constructor.
    local_state(local_state&& rhs)
    noexcept(std::is_nothrow_move_constructible_v<state_type>)
    = default;

    auto operator=(local_state&& rhs)
    noexcept(std::is_nothrow_move_assignable_v<state_type>)
    -> local_state& = default;

    auto operator=(const local_state& rhs)
    -> local_state& {
      return *this = local_state(rhs);
    }

    ///\brief Acceptor for rvalue reference.
    ///\details Accepts a single value by rvalue reference.
    ///\param[in] v The accepted value.
    auto operator()(objpipe_value_type&& v)
    -> objpipe_errc {
      if (sptr_->is_bad()) return objpipe_errc::bad;

      if constexpr(is_invocable_v<acceptor_type, state_type&, objpipe_value_type&&>)
        return std::invoke(sptr_->acceptor(), state_, std::move(v));
      else
        return std::invoke(sptr_->acceptor(), state_, v);
    }

    ///\brief Acceptor for const reference.
    ///\details Accepts a single value by const reference.
    ///\param[in] v The accepted value.
    auto operator()(const objpipe_value_type& v)
    -> objpipe_errc {
      if (sptr_->is_bad()) return objpipe_errc::bad;

      if constexpr(is_invocable_v<acceptor_type, state_type&, const objpipe_value_type&>)
        return std::invoke(sptr_->acceptor(), state_, v);
      else
        return (*this)(value_type(v));
    }

    ///\brief Acceptor for lvalue reference.
    ///\details Accepts a single value by lvalue reference.
    ///\param[in] v The accepted value.
    auto operator()(objpipe_value_type& v)
    -> objpipe_errc {
      if (sptr_->is_bad()) return objpipe_errc::bad;

      if constexpr(is_invocable_v<acceptor_type, state_type&, objpipe_value_type&>)
        return std::invoke(sptr_->acceptor(), state_, v);
      else
        return (*this)(value_type(v));
    }

    ///\brief Acceptor for exceptions.
    ///\details Immediately completes the promise with the exception.
    ///\param[in] exptr An exception pointer.
    auto push_exception(std::exception_ptr exptr)
    noexcept
    -> void {
      sptr_->push_exception(std::move(exptr));
    }

    ///\brief Destructor, publishes the reduction-state.
    ~local_state() noexcept {
      if (sptr_ != nullptr) // May have been moved.
        sptr_->publish(std::move(state_));
    }

   private:
    ///\brief Reduction-state.
    ///\details Values are accepted into the reduction state.
    state_type state_;
    ///\brief Shared state pointer.
    std::shared_ptr<SharedState> sptr_;
  };

  ///\brief Single thread reducer state.
  ///\implements IocAcceptorConcept
  ///\ingroup objpipe_detail
  ///\details
  ///Accepts values and fills in the associated promise when destroyed.
  ///\note Single thread state is not copy constructible.
  class single_thread_state {
   public:
    ///\brief Constructor.
    single_thread_state(std::promise<value_type>&& prom, Init&& init, Acceptor&& acceptor, Extractor&& extractor)
    noexcept(std::is_nothrow_constructible_v<state_type, invoke_result_t<Init>>
        && std::is_nothrow_move_constructible_v<std::promise<value_type>>
        && std::is_nothrow_move_constructible_v<Acceptor>
        && std::is_nothrow_move_constructible_v<Extractor>
        && is_nothrow_invocable_v<Init>)
    : prom_(std::move(prom)),
      state_(std::invoke(std::move(init))),
      acceptor_(std::move(acceptor)),
      extractor_(std::move(extractor))
    {}

    ///\brief Move constructor.
    single_thread_state(single_thread_state&& other)
    noexcept(std::is_nothrow_move_constructible_v<Acceptor>
        && std::is_nothrow_move_constructible_v<Extractor>
        && std::is_nothrow_move_constructible_v<state_type>)
    : prom_(std::move(other.prom_)),
      moved_away_(std::exchange(other.moved_away_, true)), // Ensure other won't attempt to assign a value at destruction.
      state_(std::move(other.state_)),
      acceptor_(std::move(other.acceptor_)),
      extractor_(std::move(other.extractor_)),
      exptr_(std::move(other.exptr_))
    {}

    single_thread_state(const single_thread_state&) = delete;
    single_thread_state& operator=(single_thread_state&&) = delete;
    single_thread_state& operator=(const single_thread_state&) = delete;

    ///\brief Accept a value by rvalue reference.
    auto operator()(objpipe_value_type&& v)
    -> objpipe_errc {
      if (moved_away_) return objpipe_errc::bad;

      if constexpr(is_invocable_v<Acceptor, state_type&, objpipe_value_type&&>)
        return std::invoke(acceptor_, state_, std::move(v));
      else
        return std::invoke(acceptor_, state_, v);
    }

    ///\brief Accept a value by const reference.
    auto operator()(const objpipe_value_type& v)
    -> objpipe_errc {
      if (moved_away_) return objpipe_errc::bad;

      if constexpr(is_invocable_v<Acceptor, state_type&, const objpipe_value_type&>)
        return std::invoke(acceptor_, state_, v);
      else
        return (*this)(value_type(v));
    }

    ///\brief Accept a value by lvalue reference.
    auto operator()(objpipe_value_type& v)
    -> objpipe_errc {
      if (moved_away_) return objpipe_errc::bad;

      if constexpr(is_invocable_v<Acceptor, state_type&, objpipe_value_type&>)
        return std::invoke(acceptor_, state_, v);
      else
        return (*this)(value_type(v));
    }

    ///\brief Accept an exception.
    ///\details
    ///Immediately completes the promise using the exception pointer.
    auto push_exception(std::exception_ptr exptr)
    noexcept
    -> void {
      if (exptr_ == nullptr) exptr_ = std::move(exptr);
    }

    ///\brief Destructor, publishes the result of the reduction.
    ///\details
    ///Unless the promise is already completed, the reduction outcome
    ///is assigned.
    ~single_thread_state() noexcept {
      if (!moved_away_) {
        if (exptr_ == nullptr) {
          try {
            if constexpr(std::is_same_v<void, value_type>) {
              std::invoke(std::move(extractor_), std::move(state_));
              prom_.set_value();
            } else {
              prom_.set_value(std::invoke(std::move(extractor_), std::move(state_)));
            }
          } catch (...) {
            push_exception(std::current_exception());
          }
        } else if (exptr_ != nullptr) {
          prom_.set_exception(exptr_);
        }
      }
    }

   private:
    ///\brief The promise to fulfill at the end of the reduction.
    std::promise<value_type> prom_;
    ///\brief Indicator that gets set if the stream is invalidated.
    ///\details Used to quickly abort the reduction if an exception is published.
    bool moved_away_ = false;
    ///\brief Reduction state.
    ///\details Only a single instance of the reduction state is used.
    state_type state_;
    ///\brief Acceptor functor.
    ///\details Accepts values into state_.
    Acceptor acceptor_;
    ///\brief Extractor functor.
    ///\details Used to retrieve the result of the reduction.
    Extractor extractor_;
    ///\brief Pending exception.
    std::exception_ptr exptr_;
  };

 public:
  ///\brief Create a new reducer for
  ///\ref singlethread_push "single threaded push" and
  ///\ref existingthread_push "existing-only single thread push".
  ///\returns A non-copyable, non-threadsafe acceptor.
  auto new_state(existingthread_push tag)
  -> single_thread_state {
    // Merger is not forwarded, since single threaded reduction does not perform merging of reducer states.
    return single_thread_state(
        std::move(prom_),
        std::move(init_),
        std::move(acceptor_),
        std::move(extractor_));
  }

  ///\brief Create a new reducer for \ref multithread_push "ordered, multi threaded push".
  ///\returns A copyable, threadsafe acceptor.
  ///The acceptor maintains ordering constraint.
  auto new_state(multithread_push tag)
  -> local_state<ordered_shared_state> {
    return local_state<ordered_shared_state>(
        std::move(prom_),
        std::move(init_),
        std::move(acceptor_),
        std::move(merger_),
        std::move(extractor_));
  }

  ///\brief Create a new reducer for \ref multithread_unordered_push "unordered, multi threaded push".
  ///\returns A copyable, threadsafe acceptor.
  ///The acceptor does not maintain ordering constraint.
  auto new_state(multithread_unordered_push tag)
  -> local_state<unordered_shared_state> {
    return local_state<unordered_shared_state>(
        std::move(prom_),
        std::move(init_),
        std::move(acceptor_),
        std::move(merger_),
        std::move(extractor_));
  }

 private:
  ///\brief Promise to complete at the end of the reduction.
  std::promise<value_type> prom_;
  ///\brief Initial state functor, creates a reducer-state.
  Init init_;
  ///\brief Accept functor, accepts values into the reducer-state.
  Acceptor acceptor_;
  ///\brief Merge functor, merges two reducer-states.
  ///\details
  ///The completion of the merge must be placed into the left argument.
  ///
  ///In other words:
  ///\code
  ///void Merger(ReducerState& x, ReducerState&& y) {
  ///  ...
  ///}
  ///\endcode
  ///In this example, the values of \p y must be merged into the values of \p x.
  Merger merger_;
  ///\brief Extract functor, retrieves the reduce outcome from the reducer-state.
  Extractor extractor_;
};


} /* namespace objpipe::detail::adapt_async */


///\brief Wrap an reduction acceptor, so that it can accept non-rvalue-reference arguments.
///\ingroup objpipe_detail
///\tparam Acceptor The wrapped acceptor.
template<typename Acceptor>
struct acceptor_adapter {
 public:
  ///\brief Constructor.
  explicit acceptor_adapter(Acceptor&& acceptor)
  noexcept(std::is_nothrow_move_constructible_v<Acceptor>)
  : acceptor_(std::move(acceptor))
  {}

  ///\brief Constructor.
  explicit acceptor_adapter(const Acceptor& acceptor)
  noexcept(std::is_nothrow_copy_constructible_v<Acceptor>)
  : acceptor_(acceptor)
  {}

  ///\brief Rvalue acceptor.
  template<typename State, typename T>
  auto operator()(State& s, T&& v) const
  -> std::enable_if_t<std::is_rvalue_reference_v<T&&>, objpipe_errc> {
    if constexpr(is_invocable_v<Acceptor, State&, T&&>)
      return std::invoke(acceptor_, s, std::move(v));
    else
      return std::invoke(acceptor_, s, v);
  }

  ///\brief Lvalue acceptor.
  template<typename State, typename T>
  auto operator()(State& s, T& v) const
  -> std::enable_if_t<std::is_lvalue_reference_v<T&>, objpipe_errc> {
    if constexpr(is_invocable_v<Acceptor, State&, T&>)
      return std::invoke(acceptor_, s, v);
    else
      return (*this)(s, T(v));
  }

 private:
  ///\brief Wrapped acceptor.
  Acceptor acceptor_;
};


///\brief Reduction merger that does not do anything.
///\ingroup objpipe_detail
struct noop_merger {
  ///\brief Noop.
  template<typename T>
  void operator()(T& x, T&& y) const {}
};

///\brief Reduction extractor that does nothing and yields a void result.
///\ingroup objpipe_detail
struct void_extractor {
  ///\brief Noop.
  template<typename T>
  void operator()(T&& v) const {}
};


/**
 * \brief Objpipe adapter for asynchronous operations.
 * \ingroup objpipe_detail
 * \note Constructed by calling adapter_t::async.
 *
 * \tparam Source Objpipe source type.
 * \tparam PushTag Push policy for the adapter.
 */
template<typename Source, typename PushTag>
class async_adapter_t {
 public:
  using value_type = adapt::value_type<Source>;

  explicit async_adapter_t(Source&& src, PushTag push_tag = PushTag())
  noexcept(std::is_nothrow_move_constructible_v<Source>
      && std::is_nothrow_move_constructible_v<PushTag>)
  : src_(std::move(src)),
    push_tag_(std::move(push_tag))
  {}

  /**
   * \brief Sink values into an \ref IocAcceptorConcept acceptor.
   * \details
   * Values are emitted into the acceptor, according to the push policy.
   * \throws objpipe_error if the push operation cannot be performed under the given constraints.
   */
  template<typename Acceptor>
  auto push(Acceptor&& acceptor) &&
  -> void {
    adapt::ioc_push(
        std::move(src_),
        std::move(push_tag_),
        std::forward<Acceptor>(acceptor));
  }

  /**
   * \brief Perform a reduction.
   * \details
   * The reduction is described by the four arguments to this function,
   * and performed according to the PushTag passed by adapter_t::async.
   *
   * Invokes \ref adapt::ioc_push ioc_push() to schedule the reduction.
   * If the Source does not support ioc_push(), a future will instead be created,
   * using std::async.
   *
   * If the PushTag is existingthread_push and the source does not implement
   * this policy, the returned future will be a lazy future.
   * Otherwise, it will be an async future.
   *
   * \note \p init is a functor.
   * \sa adapt_async::promise_reducer
   * \returns A future for the result of the reduction.
   */
  template<typename Init, typename Acceptor, typename Merger, typename Extractor>
  auto reduce(Init&& init, Acceptor&& acceptor, [[maybe_unused]] Merger&& merger, Extractor&& extractor) &&
  -> std::future<std::remove_cv_t<std::remove_reference_t<decltype(std::declval<Extractor&>()(std::declval<Init>()()))>>> {
    static_assert(is_invocable_v<const std::decay_t<Init>&>,
        "Init must be invocable, returning a reducer state.");

    using state_type = std::remove_cv_t<std::remove_reference_t<invoke_result_t<Init>>>;

    static_assert(is_invocable_v<std::decay_t<const Acceptor&>, state_type&, value_type&&>
        || is_invocable_v<std::decay_t<Acceptor>, state_type&, const value_type&>
        || is_invocable_v<std::decay_t<Acceptor>, state_type&, value_type&>,
        "Acceptor must be invocable with reducer-state reference and value_type (rref/lref/cref).");
    static_assert(is_invocable_v<const std::decay_t<Merger>&, state_type&, state_type&&>,
        "Merger must be invocable with lref reducer-state and rref reducer-state.");
    static_assert(is_invocable_v<std::decay_t<Extractor>&&, state_type&&>,
        "Extractor must be invocable with rref reducer-state.");

    using result_type = std::remove_cv_t<std::remove_reference_t<decltype(std::declval<Extractor&>()(std::declval<state_type>()))>>;

    // Default case: use ioc_push to perform push operation.
    // Note the double if:
    // - one to test if the code is valid,
    // - one to test if the function is likely to succeed.
    if constexpr(adapt::has_ioc_push<Source, PushTag>) {
      if (src_.can_push(push_tag_)) {
        std::promise<result_type> p;
        std::future<result_type> f = p.get_future();

        std::move(*this).push(
            adapt_async::promise_reducer<value_type, std::decay_t<Init>, std::decay_t<Acceptor>, std::decay_t<Merger>, std::decay_t<Extractor>>(std::move(p), std::forward<Init>(init), std::forward<Acceptor>(acceptor), std::forward<Merger>(merger), std::forward<Extractor>(extractor)).new_state(push_tag_));

        return f;
      }
    }

    if constexpr(std::is_base_of_v<singlethread_push, PushTag>) {
      std::promise<result_type> p;
      std::future<result_type> f = p.get_future();

      push_tag_.post(
          make_task(
              [](std::promise<result_type> p, Source&& src, std::decay_t<Init>&& init, const acceptor_adapter<std::decay_t<Acceptor>>& acceptor, std::decay_t<Extractor>&& extractor) -> void {
                try {
                  auto state = std::invoke(std::move(init));
                  for (;;) {
                    auto tx_val = adapt::raw_pull(src);
                    if (tx_val.has_value()) {
                      std::invoke(acceptor, state, std::move(tx_val).value());
                    } else if (tx_val.errc() == objpipe_errc::closed) {
                      break;
                    } else {
                      throw objpipe_error(tx_val.errc());
                    }
                  }

                  p.set_value(std::invoke(std::move(extractor), std::move(state)));
                } catch (...) {
                  p.set_exception(std::current_exception());
                }
              },
              std::move(p),
              std::move(src_),
              std::forward<Init>(init),
              acceptor_adapter<std::decay_t<Acceptor>>(std::forward<Acceptor>(acceptor)),
              std::forward<Extractor>(extractor)));
      return f;
    } else {
      // For existing_thread: use (lazy or threaded) future to perform reduction.
      return std::async(
          std::launch::deferred,
          [](Source&& src, std::decay_t<Init>&& init, const acceptor_adapter<std::decay_t<Acceptor>>& acceptor, std::decay_t<Extractor>&& extractor) -> result_type {
            auto state = std::invoke(std::move(init));
            for (;;) {
              auto tx_val = adapt::raw_pull(src);
              if (tx_val.has_value()) {
                std::invoke(acceptor, state, std::move(tx_val).value());
              } else if (tx_val.errc() == objpipe_errc::closed) {
                break;
              } else {
                throw objpipe_error(tx_val.errc());
              }
            }

            return std::invoke(std::move(extractor), std::move(state));
          },
          std::move(src_),
          std::forward<Init>(init),
          acceptor_adapter<std::decay_t<Acceptor>>(std::forward<Acceptor>(acceptor)),
          std::forward<Extractor>(extractor));
    }
  }

  ///\brief Reduce operation.
  ///\note \p init is a value.
  template<typename Init, typename Acceptor>
  auto reduce(Init&& init, Acceptor&& acceptor) &&
  -> std::future<std::remove_cv_t<std::remove_reference_t<Init>>> {
    using result_type = std::remove_cv_t<std::remove_reference_t<Init>>;

    return std::move(*this).reduce(
        [init] { return init; },
        [acceptor](auto&& x, auto&& y) {
          std::invoke(acceptor, x, y);
          return objpipe_errc::success;
        },
        acceptor,
        [](result_type&& v) -> result_type&& { return std::move(v); });
  }

  ///\brief Asynchronously create a vector.
  ///\details This is implemented in terms of a reduction.
  ///Multithread reductions may perform poorly, as the merge stage
  ///of each reduction needs to move the contents of the vector.
  template<typename Alloc = std::allocator<adapt::value_type<Source>>>
  auto to_vector(Alloc alloc = Alloc()) &&
  -> std::future<std::vector<value_type, Alloc>> {
    using result_type = std::vector<value_type, Alloc>;

    return std::move(*this).reduce(
        [alloc]() { return result_type(alloc); },
        [](result_type& vector, auto&& v) {
          vector.push_back(std::forward<decltype(v)>(v));
          return objpipe_errc::success;
        },
        [](result_type& vector, result_type&& rhs) {
          vector.insert(vector.end(), std::make_move_iterator(rhs.begin()), std::make_move_iterator(rhs.end()));
        },
        [](result_type&& vector) -> result_type&& {
          return std::move(vector);
        });
  }

  /**
   * \brief Copy the elements in the objpipe into the output iterator.
   * \note This function is not available in
   * \ref multithread_push "multithreaded policy"
   * (but *is* available in the
   * \ref multithread_unordered_push "unordered, multithreaded policy").
   *
   * \note If the policy is multithreaded, the output iterator shall be thread safe.
   * \param[out] out An output iterator to which to emit values.
   * \param[out] out An output iterator to which to emit values.
   * \returns A void-future that completes once all source elements have been copied.
   */
  template<typename OutputIterator,
      bool Enable = !std::is_base_of_v<multithread_push, PushTag> || std::is_base_of_v<multithread_unordered_push, PushTag>>
  auto copy(OutputIterator&& out) &&
  -> std::enable_if_t<Enable, std::future<void>> {
    return std::move(*this).reduce(
        [out]() { return out; },
        [](std::decay_t<OutputIterator>& out, auto&& v) {
          *out++ = std::forward<decltype(v)>(v);
          return objpipe_errc::success;
        },
        noop_merger(),
        void_extractor());
  }

  /**
   * \brief Invoke a functor on each of the elements in the objpipe.
   * \note This function is not available in
   * \ref multithread_push "multithreaded policy"
   * (but *is* available in the
   * \ref multithread_unordered_push "unordered, multithreaded policy").
   *
   * \note If the policy is multithreaded, the functor shall be thread safe.
   * \param[in] fn A functor to invoke for each value.
   * \returns A void-future that becomes ready when the functor completes.
   */
  template<typename Fn,
      bool Enable = !std::is_base_of_v<multithread_push, PushTag> || std::is_base_of_v<multithread_unordered_push, PushTag>>
  auto for_each(Fn&& fn) &&
  -> std::enable_if_t<Enable, std::future<void>> {
    return std::move(*this).reduce(
        [fn]() { return fn; },
        [](std::decay_t<Fn>& fn, auto&& v) {
          fn(std::forward<decltype(v)>(v));
          return objpipe_errc::success;
        },
        noop_merger(),
        void_extractor());
  }

  /**
   * \brief Count the elements in the objpipe.
   * \returns A future holding the number of elements in the objpipe.
   */
  auto count() &&
  -> std::future<std::uintmax_t> {
    return std::move(*this).reduce(
        []() -> std::uintmax_t { return 0u; },
        [](std::uintmax_t& c, [[maybe_unused]] const auto& v) {
          if (c == std::numeric_limits<std::uintmax_t>::max())
            throw std::overflow_error("objpipe count overflow");
          ++c;
          return objpipe_errc::success;
        },
        [](std::uintmax_t& x, std::uintmax_t&& y) {
          if (y > std::numeric_limits<std::uintmax_t>::max() - x)
            throw std::overflow_error("objpipe count overflow");
          x += y;
        },
        [](std::uintmax_t&& c) -> std::uintmax_t { return c; });
  }

  /**
   * \brief Retrieve the minimum element in the objpipe.
   * \details
   * If multiple elements compare the same, the first element is returned.
   * \note If the policy is unordered, it is undefined which of multiple minima is returned.
   * \returns The minimum value of elements in the objpipe.
   */
  template<typename Pred = std::less<value_type>>
  auto min(Pred pred = Pred()) &&
  -> std::future<std::optional<value_type>> {
    return std::move(*this).reduce(
        []() -> std::optional<value_type> { return {}; },
        [pred](std::optional<value_type>& c, auto&& v) {
          if (!c.has_value() || std::invoke(pred, v, *c))
            c.emplace(std::forward<decltype(v)>(v));
          return objpipe_errc::success;
        },
        [pred](std::optional<value_type>& x, std::optional<value_type>&& y) {
          if (!y.has_value()) return;
          if (!x.has_value() || std::invoke(pred, *y, *x))
            x = std::move(y);
        },
        [](std::optional<value_type>&& c) -> std::optional<value_type>&& { return std::move(c); });
  }

  /**
   * \brief Retrieve the maximum element in the objpipe.
   * \details
   * If multiple elements compare the same, the first element is returned.
   * \note If the policy is unordered, it is undefined which of multiple maxima is returned.
   * \returns The maximum value of elements in the objpipe.
   */
  template<typename Pred = std::less<value_type>>
  auto max(Pred pred = Pred()) &&
  -> std::future<std::optional<value_type>> {
    return std::move(*this).reduce(
        []() -> std::optional<value_type> { return {}; },
        [pred](std::optional<value_type>& c, auto&& v) {
          if (!c.has_value() || std::invoke(pred, *c, v))
            c.emplace(std::forward<decltype(v)>(v));
          return objpipe_errc::success;
        },
        [pred](std::optional<value_type>& x, std::optional<value_type>&& y) {
          if (!y.has_value()) return;
          if (!x.has_value() || std::invoke(pred, *x, *y))
            x = std::move(y);
        },
        [](std::optional<value_type>&& c) -> std::optional<value_type>&& { return std::move(c); });
  }

 private:
  ///\brief Source implementation.
  Source src_;
  ///\brief Push policy.
  PushTag push_tag_;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_PUSH_OP_H */
