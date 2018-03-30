#ifndef OBJPIPE_DETAIL_EMPTY_PIPE_H
#define OBJPIPE_DETAIL_EMPTY_PIPE_H

///\file
///\ingroup objpipe_detail

#include <objpipe/errc.h>
#include <objpipe/push_policies.h>
#include <objpipe/detail/transport.h>

namespace objpipe::detail {


/**
 * \brief Empty objpipe.
 * \implements SourceConcept
 * \ingroup objpipe_detail
 *
 * \details
 * An empty objpipe of type T.
 *
 * \tparam T The type of elements in the objpipe.
 */
template<typename T>
class empty_pipe {
 public:
  constexpr empty_pipe() noexcept = default;
  empty_pipe(const empty_pipe&) = delete;
  constexpr empty_pipe(empty_pipe&&) noexcept = default;
  empty_pipe& operator=(const empty_pipe&) = delete;
  empty_pipe& operator=(empty_pipe&&) = delete;

  constexpr auto wait()
  noexcept
  -> objpipe_errc {
    return objpipe_errc::closed;
  }

  constexpr auto is_pullable()
  noexcept
  -> bool {
    return false;
  }

  constexpr auto front()
  noexcept
  -> transport<T> {
    return transport<T>(std::in_place_index<1>, objpipe_errc::closed);
  }

  constexpr auto pop_front()
  noexcept
  -> objpipe_errc {
    return objpipe_errc::closed;
  }

  constexpr auto pull()
  noexcept
  -> transport<T> {
    return transport<T>(std::in_place_index<1>, objpipe_errc::closed);
  }

  constexpr auto try_pull()
  noexcept
  -> transport<T> {
    return transport<T>(std::in_place_index<1>, objpipe_errc::closed);
  }

  constexpr auto can_push([[maybe_unused]] existingthread_push tag) const
  noexcept
  -> bool {
    return true;
  }

  template<typename Acceptor>
  auto ioc_push([[maybe_unused]] existingthread_push tag, [[maybe_unused]] Acceptor&& acceptor) &&
  noexcept
  -> void {
    /* SKIP:
     * Pushing zero objects into acceptor and then destroying it is effectively
     * a noop.
     * We leave destruction to the caller.
     */
  }
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_EMPTY_PIPE_H */
