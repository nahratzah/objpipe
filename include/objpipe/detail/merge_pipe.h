#ifndef OBJPIPE_DETAIL_MERGE_PIPE_H
#define OBJPIPE_DETAIL_MERGE_PIPE_H

#include <algorithm>
#include <functional>
#include <iterator>
#include <type_traits>
#include <vector>
#include <list>
#include <deque>
#include <mutex>
#include <objpipe/detail/invocable_.h>
#include <objpipe/detail/task.h>
#include <objpipe/detail/thread_pool.h>

namespace objpipe::detail {


template<typename T>
using merge_batch_type = std::list<T>;

///\brief Converts a batched stream of elements into its components.
template<typename T, typename Sink, bool Multithreaded>
class merge_concat_push {
 public:
  explicit merge_concat_push(Sink&& sink)
  noexcept(std::is_nothrow_move_constructible_v<Sink>)
  : sink_(std::move(sink))
  {}

  explicit merge_concat_push(const Sink& sink)
  noexcept(std::is_nothrow_copy_constructible_v<Sink>)
  : sink_(sink)
  {}

  auto operator()(merge_batch_type<T>&& batch)
  -> objpipe_errc {
    assert(!batch.empty());

    if constexpr(!Multithreaded) {
      return push_(sink_, std::move(batch));
    } else {
      using std::swap;

      Sink dst = sink_; // Copy.
      swap(dst, sink_); // Reorder sink_ after dst.

      thread_pool::default_pool().publish(
          make_task(
              [](Sink&& dst, merge_batch_type<T>&& batch) {
                try {
                  push_(dst, std::move(batch));
                } catch (...) {
                  dst.push_exception(std::current_exception());
                }
              },
              std::move(dst),
              std::move(batch)));
      return objpipe_errc::success;
    }
  }

  auto push_exception(std::exception_ptr exptr)
  noexcept
  -> void {
    sink_.push_exception(std::move(exptr));
  }

 private:
  static auto push_(Sink& dst, merge_batch_type<T>&& batch)
  -> objpipe_errc {
    objpipe_errc error_code = objpipe_errc::success;

    if constexpr(is_invocable_v<Sink&, T&&>) {
      auto b = std::make_move_iterator(batch.begin()),
           e = std::make_move_iterator(batch.end());
      while (b != e && error_code == objpipe_errc::success)
        error_code = dst(*b++);
    } else {
      auto b = batch.begin(),
           e = batch.end();
      while (b != e && error_code == objpipe_errc::success)
        error_code = dst(*b++);
    }

    return error_code;
  }

  Sink sink_;
};

///\brief Converts a batched stream into its reduction.
template<typename T, typename Sink, typename MergeOp, bool Multithreaded>
class merge_reduce_push {
 public:
  merge_reduce_push(Sink&& sink, MergeOp&& do_merge)
  noexcept(std::is_nothrow_move_constructible_v<Sink>
      && std::is_nothrow_move_constructible_v<MergeOp>)
  : sink_(std::move(sink)),
    do_merge_(std::move(do_merge))
  {}

  merge_reduce_push(const Sink& sink, MergeOp&& do_merge)
  noexcept(std::is_nothrow_copy_constructible_v<Sink>
      && std::is_nothrow_move_constructible_v<MergeOp>)
  : sink_(sink),
    do_merge_(std::move(do_merge))
  {}

  auto operator()(merge_batch_type<T>&& batch)
  -> objpipe_errc {
    assert(!batch.empty());

    if constexpr(!Multithreaded) {
      return push_(sink_, std::move(batch), do_merge_);
    } else {
      using std::swap;

      Sink dst = sink_; // Copy.
      swap(dst, sink_); // Reorder sink_ after dst.

      thread_pool::default_pool().publish(
          make_task(
              [](Sink&& dst, merge_batch_type<T>&& batch, MergeOp&& do_merge) {
                try {
                  push_(dst, std::move(batch), do_merge);
                } catch (...) {
                  dst.push_exception(std::current_exception());
                }
              },
              std::move(dst),
              std::move(batch),
              do_merge_));
      return objpipe_errc::success;
    }
  }

  auto push_exception(std::exception_ptr exptr)
  noexcept
  -> void {
    sink_.push_exception(std::move(exptr));
  }

 private:
  static auto push_(Sink& dst, merge_batch_type<T>&& batch, const MergeOp& do_merge)
  -> objpipe_errc {
    auto front = transport<T>(std::in_place_index<0>, std::move(batch.front()));
    auto b = ++std::make_move_iterator(batch.begin());
    auto e = std::make_move_iterator(batch.end());

    while (b != e)
      do_merge(front, transport<T&&>(std::in_place_index<0>, *b++));

    if constexpr(is_invocable_v<Sink&, T&&>) {
      return dst(std::move(front).value());
    } else {
      return dst(front.ref());
    }
  }

  Sink sink_;
  MergeOp do_merge_;
};

///\brief Converts a single stream of elements into batches that compare neither less, nor greater.
template<typename T, typename BatchSink, typename Less>
class merge_batch_push {
 public:
  using batch_type = merge_batch_type<T>;

  merge_batch_push(BatchSink&& sink, Less&& less)
  noexcept(std::is_nothrow_move_constructible_v<BatchSink>
      && std::is_nothrow_move_constructible_v<Less>)
  : sink_(std::move(sink)),
    less_(std::move(less))
  {}

  merge_batch_push(BatchSink&& sink, const Less& less)
  noexcept(std::is_nothrow_move_constructible_v<BatchSink>
      && std::is_nothrow_copy_constructible_v<Less>)
  : sink_(std::move(sink)),
    less_(less)
  {}

  merge_batch_push(merge_batch_push&& rhs)
  noexcept(std::is_nothrow_move_constructible_v<batch_type>
      && std::is_nothrow_move_constructible_v<BatchSink>
      && std::is_nothrow_move_constructible_v<Less>)
  : batch_(std::move(rhs.batch_)),
    sink_(std::move(rhs.sink_)),
    less_(std::move(rhs.less_))
  {}

  template<typename Arg>
  auto operator()(Arg&& v)
  -> objpipe_errc {
    if (!batch_.empty() && !equiv_(batch_.front(), v)) {
      objpipe_errc e = sink_(std::move(batch_));
      batch_.clear();
      if (e != objpipe_errc::success) return e;
    }

    batch_.push_back(std::forward<Arg>(v));
    return objpipe_errc::success;
  }

  auto push_exception(std::exception_ptr exptr)
  noexcept
  -> void {
    sink_.push_exception(exptr);
  }

  ~merge_batch_push() noexcept {
    if (!batch_.empty())
      sink_(std::move(batch_));
  }

 private:
  auto equiv_(const T& x, const T& y) const
  noexcept(is_nothrow_invocable_v<const Less&, const T&, const T&>)
  -> bool {
    return !std::invoke(less_, x, y) && !std::invoke(less_, y, x);
  }

  batch_type batch_;
  BatchSink sink_;
  Less less_;
};

///\brief Converts multiple streams of elements into batches that compare neither less, nor greater.
///\details Implemented in terms of merging merge_batch_push.
template<typename T, typename Sink, typename Less>
class multiple_merge_batch_push {
 private:
  class storage {
   private:
    using data_type = std::deque<merge_batch_type<T>>;

   public:
    auto close(multiple_merge_batch_push& impl)
    noexcept
    -> void {
      std::unique_lock<std::mutex> lck{ impl.mtx_ };

      if (closed_) return;
      closed_ = true;
      if (data_.empty())
        impl.notify_ready(*this, std::move(lck));
    }

    // Must be called with multiple_merge_batch_push lock held.
    auto closed() const
    noexcept
    -> bool {
      return closed_;
    }

    ///\note Called with mtx held.
    auto push_back(merge_batch_type<T>&& v)
    -> void {
      data_.push_back(std::move(v));
    }

    ///\note Called with mtx held.
    auto empty() const
    noexcept
    -> bool {
      return data_.empty();
    }

    ///\note Called with mtx held.
    auto front() const
    noexcept
    -> const merge_batch_type<T>& {
      assert(!data_.empty());
      return data_.front();
    }

    ///\note Called with mtx held.
    auto front()
    noexcept
    -> merge_batch_type<T>& {
      assert(!data_.empty());
      return data_.front();
    }

    ///\note Called with mtx held.
    auto pop_front()
    noexcept
    -> void {
      assert(!data_.empty());
      data_.pop_front();
    }

    mutable std::mutex mtx;

   private:
    data_type data_;
    bool closed_ = false;
  };

  class greater {
   public:
    explicit greater(const Less& less)
    noexcept
    : less_(less)
    {}

    auto operator()(const storage* x, const storage* y) const
    noexcept(is_nothrow_invocable_v<const Less&, const T&, const T&>)
    -> bool {
      if (x == y) return false;

#if __cplusplus >= 201703
      std::scoped_lock<std::mutex, std::mutex> lck{ x->mtx, y->mtx };
#else
      std::lock_guard<std::mutex> lck_1{ (x < y ? x->mtx : y->mtx) };
      std::lock_guard<std::mutex> lck_2{ (x > y ? x->mtx : y->mtx) };
#endif

      assert(!x->empty() && !y->empty());
      assert(!x->front().empty() && !y->front().empty());
      return std::invoke(less_, x->front().front(), y->front().front());
    }

   private:
    const Less& less_;
  };

  using storage_vector = std::vector<storage>;
  using data_type = std::vector<storage*>;

  class storage_sink {
   public:
    storage_sink(std::shared_ptr<multiple_merge_batch_push> impl, storage& store)
    : impl_(std::move(impl)),
      store_(&store)
    {
      assert(impl_ != nullptr);
    }

    storage_sink(storage_sink&& rhs)
    noexcept
    : impl_(std::move(rhs.impl_)),
      store_(std::exchange(rhs.store_, nullptr))
    {}

    auto operator()(merge_batch_type<T>&& v)
    -> objpipe_errc {
      bool do_notify;
      {
        std::lock_guard<std::mutex> lck{ store_->mtx };
        do_notify = store_->empty();
        store_->push_back(std::move(v));
      }

      objpipe_errc e = objpipe_errc::success;
      if (do_notify) {
        std::unique_lock<std::mutex> lck{ impl_->mtx_ };
        e = impl_->notify_ready(*store_, std::move(lck));
      }
      return e;
    }

    auto push_exception(std::exception_ptr exptr)
    noexcept
    -> void {
      std::lock_guard<std::mutex> lck{ impl_->mtx_ };
      if (!std::exchange(impl_->bad_, true))
        impl_->sink_.push_exception(std::move(exptr));
    }

    ~storage_sink() {
      if (impl_ != nullptr) store_->close(*impl_);
    }

   private:
    std::shared_ptr<multiple_merge_batch_push> impl_;
    storage* store_;
  };

 private:
  ///\brief Construct a container for member variable ready_.
  ///\details Ensures the container has sz space reserved.
  ///\param[in] sz Max number of elements that will appear in the container.
  ///\returns A container on which reserve(sz) has been called.
  static auto make_ready_container_(typename storage_vector::size_type sz)
  -> data_type {
    data_type result;
    result.reserve(sz);
    return result;
  }

 public:
  // Use start() instead.
  // This is only public because std::make_shared requires it.
  template<typename LessArg>
  explicit multiple_merge_batch_push(Sink&& sink, LessArg&& less, typename storage_vector::size_type sz)
  : sink_(std::move(sink)),
    ready_(make_ready_container_(sz)),
    store_(sz),
    pending_(sz),
    less_(std::forward<LessArg>(less))
  {}

  template<typename PushTag, typename LessArg, typename SrcCollection>
  static auto start(Sink&& sink, PushTag push_tag, LessArg&& less, SrcCollection&& sources)
  -> void {
    if (sources.empty()) return; // Empty set of soures emits no values.

    // Handle trivial case of single source.
    if (sources.size() == 1u) {
      adapt::ioc_push(
          std::move(sources.front()).underlying(),
          push_tag,
          merge_batch_push<T, Sink, Less>(std::move(sink), std::forward<LessArg>(less)));
      return;
    }

    auto ptr = std::make_shared<multiple_merge_batch_push>(std::move(sink), std::forward<LessArg>(less), sources.size());

    auto storage_iter = ptr->store_.begin();
    auto sources_iter = std::make_move_iterator(sources.begin());

    static_assert(std::is_move_constructible_v<T>,
        "Value type must be move constructible.");
    static_assert(std::is_move_constructible_v<storage_sink>,
        "Storage sink must be move constructible.");
    static_assert(std::is_move_constructible_v<merge_batch_push<T, storage_sink, Less>>,
        "Merge_batch_push must be move constructible.");

    while (storage_iter != ptr->store_.end()) {
      assert(sources_iter != std::make_move_iterator(sources.end()));

      // We use (*sources_iter) instead of operator->,
      // because the iterator will hold an rvalue reference.
      // operator-> would not allow the rvalue method to be found
      // (because there is no such think as a pointer to rvalue reference).
      adapt::ioc_push(
          (*sources_iter).underlying(),
          push_tag,
          merge_batch_push<T, storage_sink, Less>(storage_sink(ptr, *storage_iter), std::forward<LessArg>(less)));

      ++sources_iter;
      ++storage_iter;
    }
    assert(sources_iter == std::make_move_iterator(sources.end()));
  }

 private:
  auto notify_ready(storage& s, std::unique_lock<std::mutex> lck)
  noexcept
  -> objpipe_errc {
    assert(lck.owns_lock() && lck.mutex() == &mtx_);
    assert(std::find(ready_.begin(), ready_.end(), &s) != ready_.end());

    try {
      std::unique_lock<std::mutex> store_lck{ s.mtx };
      if (!s.empty()) {
        ready_.push_back(&s);
        store_lck.unlock();
        std::push_heap(ready_.begin(), ready_.end(), greater(less_));
      } else {
        assert(s.closed());
      }
    } catch (...) {
      sink_.push_exception(std::current_exception());
      bad_ = true;
      return objpipe_errc::closed;
    }
    --pending_;

    return maybe_emit_(std::move(lck));
  }

  auto maybe_emit_(std::unique_lock<std::mutex> lck)
  -> objpipe_errc {
    assert(lck.owns_lock() && lck.mutex() == &mtx_);

    try {
      for (;;) {
        if (bad_) return objpipe_errc::closed;
        if (pending_ != 0) return objpipe_errc::success;

        // Create values using heap head.
        merge_batch_type<T> values;
        {
          std::pop_heap(ready_.begin(), ready_.end(), greater(less_));
          std::unique_lock<std::mutex> store_lck{ ready_.back()->mtx };
          values = std::move(ready_.back()->front());
          ready_.back()->pop_front();

          if (!ready_.back()->empty()) {
            store_lck.unlock();
            std::push_heap(ready_.begin(), ready_.end(), greater(less_));
          } else {
            if (!ready_.back()->closed())
              ++pending_;
            store_lck.unlock();
            ready_.pop_back();
          }
        }

        // Merge in other heap heads with similar values.
        while (!ready_.empty()) {
          {
            std::lock_guard<std::mutex> store_lck{ ready_.front()->mtx };
            if (std::invoke(less_, values.front(), ready_.front()->front().front())
                || std::invoke(less_, ready_.front()->front().front(), values.front())) {
              break;
            }
          }

          std::pop_heap(ready_.begin(), ready_.end(), greater(less_));
          std::unique_lock<std::mutex> store_lck{ ready_.back()->mtx };
          values.splice(
              values.end(),
              ready_.back()->front());
          ready_.back()->pop_front();

          if (!ready_.back()->empty()) {
            store_lck.unlock();
            std::push_heap(ready_.begin(), ready_.end(), greater(less_));
          } else {
            if (!ready_.back()->closed())
              ++pending_;
            store_lck.unlock();
            ready_.pop_back();
          }
        }

        // Emit merged set of values.
        objpipe_errc e = sink_(std::move(values));
        if (e != objpipe_errc::success) return e;
      }
    } catch (...) {
      bad_ = true;
      sink_.push_exception(std::current_exception());
      return objpipe_errc::closed;
    }
  }

  std::mutex mtx_;
  Sink sink_;
  data_type ready_;
  storage_vector store_; // Iterators may not be invalidated.
  typename storage_vector::size_type pending_;
  bool bad_ = false;
  Less less_;
};


///\brief Adapter for source, that maintains a reference to source front.
///\tparam Underlying objpipe source.
template<typename Source>
class merge_queue_elem_ {
 public:
  ///\brief Transport type for get().
  using transport_type = transport<adapt::front_type<Source>>;
  ///\brief Value type of the source.
  using value_type = adapt::value_type<Source>;

  constexpr merge_queue_elem_(merge_queue_elem_&& other)
  noexcept(std::is_nothrow_move_constructible_v<transport_type>
      && std::is_nothrow_move_constructible_v<Source>)
  : front_val_(std::move(other.front_val_)),
    src_(std::move(other.src_))
  {}

  merge_queue_elem_(const merge_queue_elem_&) = delete;
  auto operator=(merge_queue_elem_&&) = delete;
  auto operator=(const merge_queue_elem_&) = delete;

  ///\brief Construct a new source.
  constexpr merge_queue_elem_(Source&& src)
  noexcept(std::is_nothrow_move_constructible_v<Source>)
  : src_(std::move(src))
  {}

  auto is_pullable() noexcept
  -> bool {
    if (front_val_.has_value())
      return front_val_->errc() != objpipe_errc::closed;
    return src_.is_pullable();
  }

  ///\brief Returns a reference to Source.front().
  ///\note In constrast with Source.front(), repeated invocations are fine.
  auto get()
  -> transport_type& {
    if (!front_val_.has_value())
      front_val_.emplace(src_.front());
    return *front_val_;
  }

  ///\brief Invokes src.pop_front().
  ///\note Consequently, this invalidates the reference from get().
  auto reset()
  noexcept(std::is_nothrow_destructible_v<transport_type>
      && noexcept(std::declval<Source&>().pop_front()))
  -> objpipe_errc {
    assert(front_val_.has_value());
    assert(front_val_->errc() == objpipe_errc::success);

    front_val_.reset();
    return src_.pop_front();
  }

  ///\brief Returns get(), but destructively.
  auto release()
  noexcept(std::is_nothrow_destructible_v<transport_type>
      && std::is_nothrow_move_constructible_v<transport_type>
      && noexcept(std::declval<Source&>().pop_front()))
  -> transport_type&& {
    return std::move(get());
  }

  ///\brief Raw access to source.
  auto underlying() const &
  noexcept
  -> const Source& {
    assert(!front_val_.has_value()); // Only allowed on unused source.
    return src_;
  }

  ///\brief Raw access to source.
  auto underlying() &&
  noexcept
  -> Source&& {
    assert(!front_val_.has_value()); // Only allowed on unused source.
    return std::move(src_);
  }

 private:
  std::optional<transport_type> front_val_;
  Source src_;
};

template<typename Source, typename Less>
class merge_pipe_base {
 public:
  using queue_elem = merge_queue_elem_<Source>;
  using transport_type = typename queue_elem::transport_type;
  using value_type = typename queue_elem::value_type;

 private:
  using queue_container_type = std::vector<queue_elem>;

  ///\brief Comparator for queue_elem, using the greater() comparison.
  ///\details Intended for heap sorting.
  ///\note This comparator sorts errors to the front, allowing for early bail out.
  class greater {
   public:
    greater() = delete;

    constexpr greater(const Less& less)
    : less_(less)
    {}

    auto operator()(
        typename queue_container_type::iterator& x,
        typename queue_container_type::iterator& y) const
    -> bool {
      transport_type x_front = x->get(),
                     y_front = y->get();

      // Sort errors to the front.
      if (x_front.errc() != objpipe_errc::success
          || y_front.errc() != objpipe_errc::success)
        return x_front.errc() > y_front.errc();

      // Both must have values, so compare those.
      assert(x_front.has_value() && y_front.has_value());
      // Operate on constants.
      const value_type& x_val = static_cast<const value_type&>(x_front.value());
      const value_type& y_val = static_cast<const value_type&>(y_front.value());
      return std::invoke(less_, y_val, x_val);
    }

   private:
    const Less& less_;
  };

 public:
  template<typename Iter>
  merge_pipe_base(Iter src_begin, Iter src_end, Less&& less)
  : less_(std::move(less))
  {
    if constexpr(std::is_base_of_v<std::random_access_iterator_tag, typename std::iterator_traits<Iter>::iterator_category>)
      data_store_.reserve(src_end - src_begin);

    std::transform(src_begin, src_end,
        std::back_inserter(data_store_),
        [](auto&& src) { return std::move(src).underlying(); });

    data_.reserve(data_store_.size());
    for (auto i = data_store_.begin(); i != data_store_.end(); ++i)
      data_.emplace_back(i);
  }

  template<typename Iter>
  merge_pipe_base(Iter src_begin, Iter src_end, const Less& less)
  : less_(less)
  {
    if constexpr(std::is_base_of_v<std::random_access_iterator_tag, typename std::iterator_traits<Iter>::iterator_category>)
      data_store_.reserve(src_end - src_begin);

    std::transform(src_begin, src_end,
        std::back_inserter(data_store_),
        [](auto&& src) { return std::move(src).underlying(); });

    data_.reserve(data_store_.size());
    for (auto i = data_store_.begin(); i != data_store_.end(); ++i)
      data_.emplace_back(i);
  }

  merge_pipe_base(merge_pipe_base&&) = default;
  merge_pipe_base(const merge_pipe_base&) = delete;
  merge_pipe_base& operator=(const merge_pipe_base&) = delete;
  merge_pipe_base& operator=(merge_pipe_base&&) = delete;

  auto is_pullable() noexcept {
    if (need_init_) {
      return std::any_of(
          data_store_.begin(),
          data_store_.end(),
          std::mem_fn(&queue_elem::is_pullable));
    }

    const auto data_size = data_.size();
    switch (data_.size()) {
      case 0:
        break;
      default:
        assert(std::all_of(
                data_.begin(),
                data_.end() - 1,
                [](const auto& ptr) { return ptr->is_pullable(); }));
        return true;
      case 1:
        if (data_.back()->is_pullable()) return true;
        break;
    }
    return false;
  }

  auto wait()
  noexcept(noexcept(std::declval<queue_elem&>().get()))
  -> objpipe_errc {
    if (need_init_) { // Need to traverse all elements.
      bool all_closed = true;
      objpipe_errc e = objpipe_errc::success;
      for (queue_elem& elem : data_store_) {
        objpipe_errc elem_e = elem.get().errc();
        if (elem_e != objpipe_errc::closed) {
          all_closed = false;
          if (elem_e > e) e = elem_e;
        }
      }
      return (e == objpipe_errc::success && all_closed
          ? objpipe_errc::closed
          : e);
    }

    // Need to check current and front elements:
    // - current element may have stored error code.
    // - front element is next and may compare higher in next get_front_source() call, if it has an error.
    objpipe_errc e = (data_.empty()
        ? objpipe_errc::closed
        : data_.back()->get().errc());
    if (e == objpipe_errc::closed && data_.size() >= 2)
      e = data_.front()->get().errc();
    return e;
  }

  /**
   * \brief Find the front element to be emitted.
   * \details Invalidates previous references to data_ elements.
   *
   * Irrespective if the previous result from get_front_source() was
   * modified, this function may return a difference queue element.
   *
   * \note Address of returned elements for previous invocations may
   * be the same, but this does not indicate they are the same.
   *
   * \returns A pointer to the least queue_elem in data_,
   * that is not empty.
   * If there are no more elements, nullptr is returned.
   */
  auto get_front_source()
  -> queue_elem* {
    // Make data_ be heap sorted in its entirety.
    if (need_init_) {
      std::make_heap(data_.begin(), data_.end(), greater(less_));
      need_init_ = false;
    } else if (!data_.empty()) {
      std::push_heap(data_.begin(), data_.end(), greater(less_));
    }

    // Pop one element from the heap.
    while (!data_.empty()) {
      std::pop_heap(data_.begin(), data_.end(), greater(less_));
      if (data_.back()->get().errc() == objpipe_errc::closed)
        data_.pop_back();
      else
        return &*data_.back();
    }
    return nullptr;
  }

  auto get_queue() &&
  noexcept
  -> queue_container_type&& {
    return std::move(data_store_);
  }

  auto get_queue() const &
  noexcept
  -> const queue_container_type& {
    return data_store_;
  }

  auto is_less(const value_type& x, const value_type& y) const
  noexcept(is_nothrow_invocable_v<Less&, const value_type&, const value_type&>)
  -> bool {
    return std::invoke(less_, x, y);
  }

 private:
  /**
   * \brief All sources.
   * \details data_.back() is the element we operate upon, that can be freely modified.
   *
   * The \ref get_queue_front_() method reorders the queue each time it is called.
   * \invariant
   * \code
   * need_init_
   * || data_.empty()
   * || is_heap_sorted(data_.begin(), data_.end() - 1)
   * \endcode
   */
  queue_container_type data_store_;
  /**
   * \brief Indices into data_store_.
   * \details Since objpipe are not move-assignable or swappable, we can't
   * use heap operations on them directly.
   */
  std::vector<typename queue_container_type::iterator> data_;

  bool need_init_ = true;

 protected:
  Less less_;
};

/**
 * \brief Merges multiple \ref SourceConcept sources together using a less comparison.
 * \implements SourceConcept
 * \implements IocPushConcept
 * \ingroup objpipe_detail
 *
 * \details
 * Used to combine zero or more ordered sources, maintaining ordering.
 * \note This operation is not stable.
 * \bug This operation should be made stable.
 *
 * \tparam Source
 * The wrapped source type.
 * Must implement the \ref SourceConcept "source concept".
 * The source shall have elements ordered according to the \p Less comparator.
 * \tparam Less
 * A less comparator.
 * Each time an element is requested, the least front element in all sources is selected.
 *
 * \sa \ref merge_reduce_pipe
 */
template<typename Source, typename Less>
class merge_pipe
: private merge_pipe_base<Source, Less>
{
 private:
  using queue_elem = typename merge_pipe_base<Source, Less>::queue_elem;
  using transport_type = typename merge_pipe_base<Source, Less>::transport_type;
  using value_type = typename merge_pipe_base<Source, Less>::value_type;

 public:
  using merge_pipe_base<Source, Less>::merge_pipe_base;
  using merge_pipe_base<Source, Less>::operator=;

  using merge_pipe_base<Source, Less>::is_pullable;
  using merge_pipe_base<Source, Less>::wait;

  auto front()
  -> transport_type {
    assert(recent == nullptr);

    recent = this->get_front_source();
    if (recent == nullptr)
      return transport_type(std::in_place_index<1>, objpipe_errc::closed);
    return recent->release();
  }

  auto pop_front()
  -> objpipe_errc {
    if (recent == nullptr) {
      objpipe_errc e = front().errc();
      if (e != objpipe_errc::success)
        return e;
    }
    std::exchange(recent, nullptr)->reset();
    return objpipe_errc::success;
  }

  template<bool Enable = adapt::has_ioc_push<Source, existingthread_push>>
  auto can_push(const existingthread_push& tag) const
  noexcept
  -> std::enable_if_t<Enable, bool> {
    const auto& q = this->get_queue();
    return std::all_of(
        q.begin(),
        q.end(),
        [&tag](const auto& qelem) {
          return qelem.underlying().can_push(tag);
        });
  }

  constexpr auto can_push(const singlethread_push& tag) const
  noexcept
  -> bool {
    return true;
  }

  constexpr auto can_push(const multithread_push& tag) const
  noexcept
  -> bool {
    return true;
  }

  template<typename Acceptor, bool Enable = adapt::has_ioc_push<Source, existingthread_push>>
  auto ioc_push(existingthread_push tag, Acceptor&& acceptor) &&
  -> std::enable_if_t<Enable> {
    using sink_type = merge_concat_push<value_type, std::decay_t<Acceptor>, false>;
    using impl_type = multiple_merge_batch_push<value_type, sink_type, Less>;

    impl_type::start(
        sink_type(std::forward<Acceptor>(acceptor)),
        tag,
        std::move(this->less_),
        std::move(*this).get_queue());
  }

  template<typename Acceptor>
  auto ioc_push(singlethread_push tag, Acceptor&& acceptor) &&
  -> void {
    using sink_type = merge_concat_push<value_type, std::decay_t<Acceptor>, false>;
    using impl_type = multiple_merge_batch_push<value_type, sink_type, Less>;

    impl_type::start(
        sink_type(std::forward<Acceptor>(acceptor)),
        tag,
        std::move(this->less_),
        std::move(*this).get_queue());
  }

  template<typename Acceptor>
  auto ioc_push(multithread_push tag, Acceptor&& acceptor) &&
  -> void {
    using sink_type = merge_concat_push<value_type, std::decay_t<Acceptor>, true>;
    using impl_type = multiple_merge_batch_push<value_type, sink_type, Less>;

    impl_type::start(
        sink_type(std::forward<Acceptor>(acceptor)),
        singlethread_push(),
        std::move(this->less_),
        std::move(*this).get_queue());
  }

  template<bool Enable = adapt::has_ioc_push<Source, multithread_unordered_push>>
  auto can_push(const multithread_unordered_push& tag) const
  noexcept
  -> std::enable_if_t<Enable, bool> {
    const auto& q = this->get_queue();
    return std::all_of(
        q.begin(),
        q.end(),
        [&tag](const auto& qelem) {
          return qelem.underlying().can_push(tag);
        });
  }

  template<typename Acceptor, bool Enable = adapt::has_ioc_push<Source, multithread_unordered_push>>
  auto ioc_push(multithread_unordered_push tag, Acceptor&& acceptor) &&
  -> std::enable_if_t<Enable> {
    // We drop the ordering constraint from merge:
    // 1. merging does not drop elements, it only needs ordering for output ordering
    // 2. the tag specifies we don't care about output ordering
    // Therefore, we can simply emit each element in unspecified order.
    auto&& q = std::move(*this).get_queue();
    std::for_each(
        std::make_move_iterator(q.begin()),
        std::make_move_iterator(q.end()),
        [&tag, &acceptor](auto&& qelem) {
          return std::move(qelem).underlying().ioc_push(tag, std::decay_t<Acceptor>(acceptor));
        });
  }

 private:
  queue_elem* recent = nullptr;
};

template<typename Type, typename ReduceOp> class do_merge_t;

/**
 * \brief Merges multiple \ref SourceConcept sources together using a less comparison, combining those that are equal.
 * \implements SourceConcept
 * \implements IocPushConcept
 * \ingroup objpipe_detail
 *
 * \details
 * Used to combine zero or more ordered sources, maintaining ordering.
 *
 * Adjecent duplicates are reduced according to the \p ReduceOp, prior to being emitted.
 *
 * \note This operation is not stable.
 * \bug This operation should be made stable.
 *
 * \tparam Source
 * The wrapped source type.
 * Must implement the \ref SourceConcept "source concept".
 * The source shall have elements ordered according to the \p Less comparator.
 * \tparam Less
 * A less comparator.
 * Each time an element is requested, the least front element in all sources is selected.
 *
 * \sa \ref merge_pipe
 */
template<typename Source, typename Less, typename ReduceOp>
class merge_reduce_pipe
: private merge_pipe_base<Source, Less>
{
 private:
  using queue_elem = typename merge_pipe_base<Source, Less>::queue_elem;
  using value_type = typename merge_pipe_base<Source, Less>::value_type;
  using transport_type = transport<value_type>;

 public:
  template<typename Iter, typename LessInit>
  merge_reduce_pipe(Iter src_begin, Iter src_end, LessInit&& less, ReduceOp&& reduce_op)
  : merge_pipe_base<Source, Less>(src_begin, src_end, std::forward<LessInit>(less)),
    do_merge_(std::move(reduce_op))
  {}

  template<typename Iter, typename LessInit>
  merge_reduce_pipe(Iter src_begin, Iter src_end, LessInit&& less, const ReduceOp& reduce_op)
  : merge_pipe_base<Source, Less>(src_begin, src_end, std::forward<LessInit>(less)),
    do_merge_(reduce_op)
  {}

  using merge_pipe_base<Source, Less>::is_pullable;
  using merge_pipe_base<Source, Less>::wait;

  auto front()
  -> transport_type {
    assert(!pending_pop);

    queue_elem* head = this->get_front_source();
    if (head == nullptr)
      return transport_type(std::in_place_index<1>, objpipe_errc::closed);

    transport_type val = head->release();
    if (val.errc() != objpipe_errc::success) return val;
    objpipe_errc e = head->reset();
    if (e != objpipe_errc::success) {
      val.emplace(std::in_place_index<1>, e);
      return val;
    }
    assert(val.has_value());

    // Keep merging in successive values, until less comparison holds.
    for (;;) {
      head = this->get_front_source();
      if (head == nullptr) break;
      auto& head_val = head->get();
      if (head_val.errc() != objpipe_errc::success) {
        val.emplace(std::in_place_index<1>, head_val.errc());
        return val;
      }
      if (this->is_less(val.value(), head_val.value())) break;

      do_merge_(val, head->release());
      e = head->reset();
      if (e != objpipe_errc::success) {
        val.emplace(std::in_place_index<1>, e);
        return val;
      }
    }

    pending_pop = true;
    return val;
  }

  auto pop_front()
  -> objpipe_errc {
    if (!pending_pop) {
      objpipe_errc e = front().errc();
      if (e != objpipe_errc::success)
        return e;
    }

    pending_pop = false;
    return objpipe_errc::success;
  }

  template<bool Enable = adapt::has_ioc_push<Source, existingthread_push>>
  auto can_push(const existingthread_push& tag) const
  noexcept
  -> std::enable_if_t<Enable, bool> {
    const auto& q = this->get_queue();
    return std::all_of(
        q.begin(),
        q.end(),
        [&tag](const auto& qelem) {
          return qelem.underlying().can_push(tag);
        });
  }

  constexpr auto can_push(const singlethread_push& tag) const
  noexcept
  -> bool {
    return true;
  }

  constexpr auto can_push(const multithread_push& tag) const
  noexcept
  -> bool {
    return true;
  }

  template<typename Acceptor, bool Enable = adapt::has_ioc_push<Source, existingthread_push>>
  auto ioc_push(existingthread_push tag, Acceptor&& acceptor) &&
  -> std::enable_if_t<Enable> {
    using sink_type = merge_reduce_push<value_type, std::decay_t<Acceptor>, do_merge_t<value_type, ReduceOp>, false>;
    using impl_type = multiple_merge_batch_push<value_type, sink_type, Less>;

    impl_type::start(
        sink_type(std::forward<Acceptor>(acceptor), std::move(do_merge_)),
        tag,
        std::move(this->less_),
        std::move(*this).get_queue());
  }

  template<typename Acceptor>
  auto ioc_push(singlethread_push tag, Acceptor&& acceptor) &&
  -> void {
    using sink_type = merge_reduce_push<value_type, std::decay_t<Acceptor>, do_merge_t<value_type, ReduceOp>, false>;
    using impl_type = multiple_merge_batch_push<value_type, sink_type, Less>;

    impl_type::start(
        sink_type(std::forward<Acceptor>(acceptor), std::move(do_merge_)),
        tag,
        std::move(this->less_),
        std::move(*this).get_queue());
  }

  template<typename Acceptor>
  auto ioc_push(multithread_push tag, Acceptor&& acceptor) &&
  -> void {
    using sink_type = merge_reduce_push<value_type, std::decay_t<Acceptor>, do_merge_t<value_type, ReduceOp>, true>;
    using impl_type = multiple_merge_batch_push<value_type, sink_type, Less>;

    impl_type::start(
        sink_type(std::forward<Acceptor>(acceptor), std::move(do_merge_)),
        singlethread_push(),
        std::move(this->less_),
        std::move(*this).get_queue());
  }

 private:
  bool pending_pop = false;
  do_merge_t<value_type, ReduceOp> do_merge_;
};

///\brief Functor that invokes reduce_op on transports of value_type.
///\details The result of the reduction is stored in the first argument to this functor.
template<typename Type, typename ReduceOp>
class do_merge_t {
 private:
  using value_type = Type;
  static_assert(!std::is_const_v<Type> && !std::is_volatile_v<Type> && !std::is_reference_v<Type>,
      "Type must be unqualified.");

  static constexpr bool is_cc_invocable =
      is_invocable_v<const ReduceOp&, const Type&, const Type&>;
  static constexpr bool is_cl_invocable =
      is_invocable_v<const ReduceOp&, const Type&, Type&>;
  static constexpr bool is_cr_invocable =
      is_invocable_v<const ReduceOp&, const Type&, Type&&>;

  static constexpr bool is_lc_invocable =
      is_invocable_v<const ReduceOp&, Type&, const Type&>;
  static constexpr bool is_ll_invocable =
      is_invocable_v<const ReduceOp&, Type&, Type&>;
  static constexpr bool is_lr_invocable =
      is_invocable_v<const ReduceOp&, Type&, Type&&>;

  static constexpr bool is_rc_invocable =
      is_invocable_v<const ReduceOp&, Type&&, const Type&>;
  static constexpr bool is_rl_invocable =
      is_invocable_v<const ReduceOp&, Type&&, Type&>;
  static constexpr bool is_rr_invocable =
      is_invocable_v<const ReduceOp&, Type&&, Type&&>;

  static_assert(
      is_cc_invocable
      || is_cl_invocable
      || is_cr_invocable
      || is_lc_invocable
      || is_ll_invocable
      || is_lr_invocable
      || is_rc_invocable
      || is_rl_invocable
      || is_rr_invocable,
      "Reducer must be invocable with value type.");

 public:
  constexpr do_merge_t(ReduceOp&& op)
  noexcept(std::is_nothrow_move_constructible_v<ReduceOp>)
  : op_(std::move(op))
  {}

  constexpr do_merge_t(const ReduceOp& op)
  noexcept(std::is_nothrow_copy_constructible_v<ReduceOp>)
  : op_(op)
  {}

  auto operator()(transport<value_type>& x, transport<value_type>&& y) const
  -> void {
    if constexpr(is_rr_invocable)
      this->assign_(x, std::invoke(op_, std::move(x).value(), std::move(y).value()));
    else if constexpr(is_lr_invocable || is_cr_invocable)
      this->assign_(x, std::invoke(op_, x.value(), std::move(y).value()));
    else if constexpr(is_rl_invocable || is_rc_invocable)
      this->assign_(x, std::invoke(op_, std::move(x).value(), y.value()));
    else
      this->assign_(x, std::invoke(op_, x.value(), y.value()));
  }

  auto operator()(transport<value_type>& x, transport<value_type&&>&& y) const
  -> void {
    if constexpr(is_rr_invocable)
      this->assign_(x, std::invoke(op_, std::move(x).value(), std::move(y).value()));
    else if constexpr(is_lr_invocable || is_cr_invocable)
      this->assign_(x, std::invoke(op_, x.value(), std::move(y).value()));
    else if constexpr(is_rl_invocable || is_rc_invocable)
      this->assign_(x, std::invoke(op_, std::move(x).value(), lref(y.value())));
    else
      this->assign_(x, std::invoke(op_, x.value(), lref(y.value())));
  }

  auto operator()(transport<value_type>& x, transport<const value_type&>&& y) const
  -> void {
    if constexpr(is_rc_invocable)
      this->assign_(x, std::invoke(op_, std::move(x).value(), y.value()));
    else if constexpr(is_lc_invocable || is_cc_invocable)
      this->assign_(x, std::invoke(op_, x.value(), y.value()));
    else if constexpr(is_rr_invocable)
      this->assign_(x, std::invoke(op_, std::move(x).value(), value_type(y.value())));
    else if constexpr(is_cr_invocable || is_lr_invocable)
      this->assign_(x, std::invoke(op_, x.value(), value_type(y.value())));
    else {
      value_type y_lvalue = y.value();
      if constexpr(is_rl_invocable)
        this->assign_(x, std::invoke(op_, std::move(x).value(), y_lvalue));
      else // is_ll_invocable || is_cl_invocable
        this->assign_(x, std::invoke(op_, x.value(), y_lvalue));
    }
  }

 private:
  template<typename Transport>
  static value_type& lref(value_type& x) noexcept {
    return x.value();
  }

  template<typename Transport>
  static value_type& lref(value_type&& x) noexcept {
    return x.value();
  }

  template<typename V>
  void assign_(transport<value_type>& x, V&& v) const {
    if (std::addressof(x.value()) != std::addressof(v))
      x.emplace(std::in_place_index<0>, std::forward<V>(v));
  }

  ReduceOp op_;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_MERGE_PIPE_H */
