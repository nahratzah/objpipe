#ifndef OBJPIPE_DETAIL_ARRAY_PIPE_H
#define OBJPIPE_DETAIL_ARRAY_PIPE_H

///\file
///\ingroup objpipe_detail

#include <algorithm>
#include <cstddef>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>
#include <objpipe/errc.h>
#include <objpipe/push_policies.h>
#include <objpipe/detail/transport.h>
#include <objpipe/detail/adapt.h>
#include <objpipe/detail/task.h>

namespace objpipe::detail {


/**
 * \brief Iterate over a copied sequence of elements.
 * \implements SourceConcept
 * \implements IocPushConcept
 * \ingroup objpipe_detail
 *
 * \details
 * Elements are copied into a std::deque and released during iteration.
 * \tparam T The type of elements to iterate over.
 * \tparam Alloc The allocator to use for storing the elements.
 * \sa \ref objpipe::new_array
 */
template<typename T, typename Alloc>
class array_pipe {
 private:
  static constexpr std::ptrdiff_t batch_size = 10000;
  using data_type = std::deque<T, Alloc>;

 public:
  template<typename Iter>
  array_pipe(Iter b, Iter e, Alloc alloc)
  : data_(b, e, alloc)
  {}

  array_pipe(std::initializer_list<T> init, Alloc alloc)
  : data_(init, alloc)
  {}

  array_pipe(array_pipe&&) = default;
  array_pipe(const array_pipe&) = delete;
  array_pipe& operator=(array_pipe&&) = delete;
  array_pipe& operator=(const array_pipe&) = delete;

  auto is_pullable()
  noexcept
  -> bool {
    return !data_.empty();
  }

  auto wait()
  noexcept(noexcept(std::declval<const data_type&>().empty()))
  -> objpipe_errc {
    return (data_.empty() ? objpipe_errc::closed : objpipe_errc::success);
  }

  auto front()
  noexcept(noexcept(std::declval<const data_type&>().empty())
      && noexcept(std::declval<const data_type&>().front()))
  -> transport<T&&> {
    if (data_.empty()) return transport<T&&>(std::in_place_index<1>, objpipe_errc::closed);
    return transport<T&&>(std::in_place_index<0>, std::move(data_.front()));
  }

  auto pop_front()
  noexcept(noexcept(std::declval<data_type&>().pop_front()))
  -> objpipe_errc {
    if (data_.empty()) return objpipe_errc::closed;
    data_.pop_front();
    return objpipe_errc::success;
  }

  auto pull()
  noexcept(noexcept(std::declval<data_type&>().empty())
      && noexcept(std::declval<data_type&>().front())
      && noexcept(std::declval<data_type&>().pop_front())
      && std::is_nothrow_move_constructible_v<T>)
  -> transport<T> {
    if (data_.empty()) return transport<T>(std::in_place_index<1>, objpipe_errc::closed);
    auto rv = transport<T>(std::in_place_index<0>, std::move(data_.front()));
    data_.pop_front();
    return rv;
  }

  constexpr auto can_push([[maybe_unused]] const multithread_push& tag) const
  noexcept
  -> bool {
    return true;
  }

  template<typename Acceptor>
  auto ioc_push(const multithread_push& tag, Acceptor&& acceptor) &&
  -> void {
    const std::shared_ptr<data_type> data = std::make_shared<data_type>(std::move(data_));

    auto sink = std::forward<Acceptor>(acceptor);
    auto b = std::make_move_iterator(data->begin()),
         e = std::make_move_iterator(data->end());
    while (b != e) {
      auto slice_end = (e - b > batch_size ? std::next(b, batch_size) : e);
      auto next_sink = sink; // Copy.

      tag.post(
          make_task(
              // Note: data is passed because it has ownership of the range that b,e describe.
              []([[maybe_unused]] std::shared_ptr<data_type> data, auto&& sink, auto&& b, auto&& e) {
                try {
                  std::for_each(b, e, std::ref(sink));
                } catch (...) {
                  sink.push_exception(std::current_exception());
                }
              },
              data, std::move(sink), b, slice_end));

      b = std::move(slice_end);
      sink = std::move(next_sink);
    }
  }

  auto try_pull()
  noexcept(noexcept(pull()))
  -> decltype(pull()) {
    return pull();
  }

 private:
  data_type data_;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_ARRAY_PIPE_H */
