#ifndef OBJPIPE_MERGE_H
#define OBJPIPE_MERGE_H

///\file
///\ingroup objpipe

#include <objpipe/detail/merge_pipe.h>
#include <objpipe/detail/adapter.h>
#include <functional>
#include <iterator>
#include <type_traits>

namespace objpipe {


/**
 * \brief Merge multiple objpipe together, emitting elements using the comparator.
 * \details
 * Input objpipes are not required to be unique.
 * \note The objpipes should be ordered according to the comparator.
 * \param[in] b,e Iterator pair describing a sequence of objpipes.
 *   The iterator should be a move iterator.
 * \param[in] less Comparison function, default is std::less<>.
 * \returns An objpipe that merges each of the objpipes in [b,e), ordered using the comparator function.
 */
template<typename Iter, typename Less = std::less<>>
auto merge(Iter b, Iter e, Less&& less = Less())
-> std::enable_if_t<
    detail::is_adapter_v<typename std::iterator_traits<Iter>::value_type>,
    detail::adapter_t<detail::merge_pipe<
        detail::adapter_underlying_type_t<typename std::iterator_traits<Iter>::value_type>,
        std::decay_t<Less>>>> {
  static_assert(std::is_rvalue_reference_v<typename std::iterator_traits<Iter>::reference>
      || !std::is_reference_v<typename std::iterator_traits<Iter>::reference>,
      "Please use a move iterator.");
  using impl = detail::merge_pipe<
      detail::adapter_underlying_type_t<typename std::iterator_traits<Iter>::value_type>,
      std::decay_t<Less>>;

  return detail::adapter(impl(b, e, std::forward<Less>(less)));
}

/**
 * \brief Merge multiple objpipe together, combining them.
 * \details The elements are compared using the \p less comparator.
 * Elements that are equal under this comparison, are merged together using
 * \p reduce_op.
 *
 * Input objpipes are not required to be unique.
 *
 * \note The objpipes should be ordered according to the comparator.
 * \param[in] b,e Iterator pair describing a sequence of objpipes.
 *   The iterator should be a move iterator.
 * \param[in] less Comparison function, default is std::less<>.
 * \param[in] reduce_op Reducer that is used to merge two adjecent, equal values together.
 * \returns An objpipe that merges and combines each of the objpipes in [b,e), ordered using the comparator function.
 */
template<typename Iter, typename Less = std::less<>, typename ReduceOp = std::plus<>>
auto merge_combine(Iter b, Iter e, Less&& less = Less(), ReduceOp&& reduce_op = ReduceOp())
-> std::enable_if_t<
    detail::is_adapter_v<typename std::iterator_traits<Iter>::value_type>,
    detail::adapter_t<detail::merge_reduce_pipe<
        detail::adapter_underlying_type_t<typename std::iterator_traits<Iter>::value_type>,
        std::decay_t<Less>,
        std::decay_t<ReduceOp>>>> {
  static_assert(std::is_rvalue_reference_v<typename std::iterator_traits<Iter>::reference>
      || !std::is_reference_v<typename std::iterator_traits<Iter>::reference>,
      "Please use a move iterator.");
  using impl = detail::merge_reduce_pipe<
      detail::adapter_underlying_type_t<typename std::iterator_traits<Iter>::value_type>,
      std::decay_t<Less>,
      std::decay_t<ReduceOp>>;

  return detail::adapter(impl(b, e,
          std::forward<Less>(less),
          std::forward<ReduceOp>(reduce_op)));
}


} /* namespace objpipe */

#endif /* OBJPIPE_MERGE_H */
