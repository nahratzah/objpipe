#ifndef OBJPIPE_DETAIL_TASK_H
#define OBJPIPE_DETAIL_TASK_H

#include <type_traits>

namespace objpipe::detail {


template<typename Fn, typename... Args>
class task {
 public:
  template<typename FnArg, typename... ArgsArg, typename = std::enable_if_t<sizeof...(ArgsArg) == sizeof...(Args)>>
  explicit task(FnArg&& fn, ArgsArg&&... args)
  noexcept(std::conjunction_v<
      std::is_nothrow_constructible<Fn, FnArg>,
      std::is_nothrow_constructible<Args, ArgsArg>...>)
  : fn_(std::forward<FnArg>(fn)),
    args_(std::forward<ArgsArg>(args)...)
  {}

  task(task&& rhs)
  noexcept(std::conjunction_v<
      std::is_nothrow_move_constructible<Fn>,
      std::is_nothrow_move_constructible<Args>...>)
  : fn_(std::move(rhs.fn_)),
    args_(std::move(rhs.args_))
  {}

  auto operator()()
  noexcept(noexcept(std::apply(std::declval<Fn>(), std::declval<std::tuple<Args...>>())))
  -> auto {
    return std::apply(std::move(fn_), std::move(args_));
  }

 private:
  Fn fn_;
  std::tuple<Args...> args_;
};

template<typename Fn, typename... Args>
auto make_task(Fn&& fn, Args&&... args)
-> task<std::decay_t<Fn>, std::remove_cv_t<std::remove_reference_t<Args>>...> {
  return task<std::decay_t<Fn>, std::remove_cv_t<std::remove_reference_t<Args>>...>(
      std::forward<Fn>(fn),
      std::forward<Args>(args)...);
}


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_TASK_H */
