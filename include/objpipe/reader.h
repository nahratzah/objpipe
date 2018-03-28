#ifndef OBJPIPE_READER_H
#define OBJPIPE_READER_H

///\file
///\ingroup objpipe

#include <objpipe/detail/fwd.h>
#include <objpipe/detail/virtual.h>

namespace objpipe {


/**
 * \brief An object pipe reader.
 * \ingroup objpipe
 *
 * \tparam T The type of objects emitted by the object pipe.
 */
template<typename T>
class reader
: public detail::adapter_t<detail::virtual_pipe<T>>
{
 public:
  using detail::adapter_t<detail::virtual_pipe<T>>::adapter_t;
  using detail::adapter_t<detail::virtual_pipe<T>>::operator=;
};


} /* namespace objpipe */

#include <objpipe/detail/adapter.h>

#endif /* OBJPIPE_READER_H */
