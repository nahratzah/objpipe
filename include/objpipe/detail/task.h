#ifndef OBJPIPE_DETAIL_TASK_H
#define OBJPIPE_DETAIL_TASK_H

///\file
///\ingroup objpipe_detail

#include <type_traits>

namespace objpipe::detail {


/**
 * \brief A task is a no-argument functor that is to be invoked at most once.
 * \ingroup objpipe_detail
 * \details
 * Tasks are what objpipe uses to split a job across multiple threads.
 *
 * Arguments to the functor are passed by rvalue reference, since they may not
 * be copyable.
 *
 * Tasks are movable, but not copy constructible or assignable.
 * \tparam Fn An invocable that is to be run.
 * \tparam Args Arguments to be passed to the invocable.
 */
template<typename Fn, typename... Args>
class task {
 public:
  ///\brief Constructor, creates a new task.
  template<typename FnArg, typename... ArgsArg, typename = std::enable_if_t<sizeof...(ArgsArg) == sizeof...(Args)>>
  explicit task(FnArg&& fn, ArgsArg&&... args)
  noexcept(std::conjunction_v<
      std::is_nothrow_constructible<Fn, FnArg>,
      std::is_nothrow_constructible<Args, ArgsArg>...>)
  : fn_(std::forward<FnArg>(fn)),
    args_(std::forward<ArgsArg>(args)...)
  {}

  ///\brief Move constructor.
  task(task&& rhs)
  noexcept(std::conjunction_v<
      std::is_nothrow_move_constructible<Fn>,
      std::is_nothrow_move_constructible<Args>...>)
  : fn_(std::move(rhs.fn_)),
    args_(std::move(rhs.args_))
  {}

  ///\brief Invocation operator.
  ///\details Invokes the functor by passing it the arguments by rvalue reference.
  ///
  ///If this method is run a second time, the behaviour is undefined.
  auto operator()()
  noexcept(noexcept(std::apply(std::declval<Fn>(), std::declval<std::tuple<Args...>>())))
  -> auto {
    return std::apply(std::move(fn_), std::move(args_));
  }

 private:
  ///\brief Functor to invoke.
  Fn fn_;
  ///\brief Arguments to invoke the functor with.
  std::tuple<Args...> args_;
};

///\brief Construct a task from a functor and arguments.
///\ingroup objpipe_detail
///\relates task
template<typename Fn, typename... Args>
auto make_task(Fn&& fn, Args&&... args)
-> task<std::decay_t<Fn>, std::remove_cv_t<std::remove_reference_t<Args>>...> {
  return task<std::decay_t<Fn>, std::remove_cv_t<std::remove_reference_t<Args>>...>(
      std::forward<Fn>(fn),
      std::forward<Args>(args)...);
}


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_TASK_H */
