#ifndef OBJPIPE_DETAIL_INVOCABLE__H
#define OBJPIPE_DETAIL_INVOCABLE__H

///\file
///\ingroup objpipe_detail
///\brief Compatibility header that uses or implements std::is_invocable family of type traits.

#include <type_traits>
#include <functional>

namespace objpipe::detail {


#if __cpp_lib_is_invocable
using std::is_invocable;
using std::is_nothrow_invocable;
using std::invoke_result;
using std::is_invocable_v;
using std::is_nothrow_invocable_v;
using std::invoke_result_t;
#else
template<typename... Args>
struct invoke_result_base_ {
  template<typename Fn, typename = void> struct impl_;
};

template<typename... Args>
template<typename Fn, typename>
struct invoke_result_base_<Args...>::impl_ {
  static constexpr bool is_invocable = false;
  static constexpr bool is_nothrow_invocable = false;
};

template<typename... Args>
template<typename Fn>
struct invoke_result_base_<Args...>::impl_<Fn, std::void_t<decltype(std::invoke(std::declval<Fn>(), std::declval<Args>()...))>> {
  using type = decltype(std::invoke(std::declval<Fn>(), std::declval<Args>()...));
  static constexpr bool is_invocable = true;
  static constexpr bool is_nothrow_invocable = noexcept(std::invoke(std::declval<Fn>(), std::declval<Args>()...));
};

template<typename Fn, typename... Args>
struct invoke_result
: public invoke_result_base_<Args...>::template impl_<Fn>
{};

template<typename Fn, typename... Args>
using invoke_result_t = typename invoke_result<Fn, Args...>::type;

template<typename Fn, typename... Args>
using is_invocable = std::integral_constant<bool, invoke_result<Fn, Args...>::is_invocable>;

template<typename Fn, typename... Args>
constexpr bool is_invocable_v = is_invocable<Fn, Args...>::value;

template<typename Fn, typename... Args>
using is_nothrow_invocable = std::integral_constant<bool, invoke_result<Fn, Args...>::is_nothrow_invocable>;

template<typename Fn, typename... Args>
constexpr bool is_nothrow_invocable_v = is_nothrow_invocable<Fn, Args...>::value;
#endif


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_INVOCABLE__H */
