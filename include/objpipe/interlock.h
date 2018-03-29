#ifndef OBJPIPE_INTERLOCK_H
#define OBJPIPE_INTERLOCK_H

///\file
///\ingroup objpipe

#include <tuple>
#include <objpipe/detail/interlock_pipe.h>
#include <objpipe/detail/adapter.h>

namespace objpipe {


/**
 * \brief Reader side of the interlock objpipe.
 */
template<typename T>
using interlock_reader = detail::adapter_t<detail::interlock_pipe<T>>;

/**
 * \brief Writer side of the interlock objpipe.
 */
template<typename T>
using interlock_writer = detail::interlock_writer<T>;

/**
 * \brief Create a new interlocked objpipe.
 * \ingroup objpipe
 *
 * \tparam T The type of elements used in the interlocked pipe.
 * \return A tuple pair,
 * with a \ref objpipe::interlock_reader "reader"
 * and a \ref objpipe::interlock_writer "writer",
 * both sharing the same interlocked object pipe.
 * \sa \ref objpipe::detail::interlock_pipe<T>
 */
template<typename T>
auto new_interlock()
-> std::tuple<interlock_reader<T>, interlock_writer<T>> {
  detail::interlock_impl<T>*const ptr = new detail::interlock_impl<T>();
  return std::make_tuple( // Never throws.
      interlock_reader<T>(detail::interlock_pipe<T>(ptr)),
      interlock_writer<T>(ptr));
}


} /* namespace objpipe */

#endif /* OBJPIPE_INTERLOCK_H */
