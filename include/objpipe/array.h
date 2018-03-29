#ifndef OBJPIPE_ARRAY_H
#define OBJPIPE_ARRAY_H

///\file
///\ingroup objpipe

#include <objpipe/detail/array_pipe.h>
#include <objpipe/detail/adapter.h>

namespace objpipe {


/**
 * \brief Create an objpipe that iterates over the given range.
 * \ingroup objpipe
 *
 * \details The elements in the range are copied at construction time.
 *
 * \note If you want to iterate an entire collection,
 * \ref objpipe::of "objpipe::of"
 * followed by
 * \ref objpipe::detail::adapter_t::iterate ".iterate()"
 * will have better performance.
 * \code
 * of(collection).iterate()
 * \endcode
 *
 * \param[in] b,e The values to iterate over.
 * \param[in] alloc The allocator used to allocate internal storage.
 * \return An objpipe that iterates the values in the given range.
 * \sa \ref ojbpipe::detail::array_pipe
 */
template<typename Iter, typename Alloc = std::allocator<typename std::iterator_traits<Iter>::value_type>>
auto new_array(Iter b, Iter e, Alloc alloc = Alloc())
-> decltype(auto) {
  return detail::adapter(detail::array_pipe<typename std::iterator_traits<Iter>::value_type, Alloc>(b, e, alloc));
}

/**
 * \brief Create an objpipe that iterates over the given values.
 * \ingroup objpipe
 *
 * \details The elements in the range are copied at construction time.
 *
 * \note If you want to iterate an entire collection,
 * \ref objpipe::of "objpipe::of"
 * followed by
 * \ref objpipe::detail::adapter_t::iterate ".iterate()"
 * will have better performance.
 * \code
 * of(collection).iterate()
 * \endcode
 *
 * \param[in] values The values to iterate over.
 * \param[in] alloc The allocator used to allocate internal storage.
 * \return An objpipe that iterates the values in the given range.
 * \sa \ref ojbpipe::detail::array_pipe
 */
template<typename T, typename Alloc = std::allocator<T>>
auto new_array(std::initializer_list<T> values, Alloc alloc = Alloc())
-> decltype(auto) {
  return detail::adapter(detail::array_pipe<T, Alloc>(values, alloc));
}


} /* namespace objpipe */

#endif /* OBJPIPE_ARRAY_H */
