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
///\brief Argument pack to is_invocable family of type traits.
///\ingroup objpipe_detail
template<typename... Args>
struct invoke_result_base_ {
  template<typename Fn, typename = void> struct impl_;
};

///\brief Data for when is_invocable is false_type.
///\ingroup objpipe_detail
template<typename... Args>
template<typename Fn, typename>
struct invoke_result_base_<Args...>::impl_ {
  static constexpr bool is_invocable = false;
  static constexpr bool is_nothrow_invocable = false;
};

///\brief Data for when is_invocable is true_type.
///\ingroup objpipe_detail
template<typename... Args>
template<typename Fn>
struct invoke_result_base_<Args...>::impl_<Fn, std::void_t<decltype(std::invoke(std::declval<Fn>(), std::declval<Args>()...))>> {
  using type = decltype(std::invoke(std::declval<Fn>(), std::declval<Args>()...));
  static constexpr bool is_invocable = true;
  static constexpr bool is_nothrow_invocable = noexcept(std::invoke(std::declval<Fn>(), std::declval<Args>()...));
};

///\brief Our own std::invoke_result, if the library doesn't provide one.
///\ingroup objpipe_detail
///\note If the library does provide one, this is simply a using declaration.
template<typename Fn, typename... Args>
struct invoke_result
: public invoke_result_base_<Args...>::template impl_<Fn>
{};

///\brief Our own std::invoke_result_t, if the library doesn't provide one.
///\ingroup objpipe_detail
///\note If the library does provide one, this is simply a using declaration.
template<typename Fn, typename... Args>
using invoke_result_t = typename invoke_result<Fn, Args...>::type;

///\brief Our own std::is_invocable, if the library doesn't provide one.
///\ingroup objpipe_detail
///\note If the library does provide one, this is simply a using declaration.
template<typename Fn, typename... Args>
using is_invocable = std::integral_constant<bool, invoke_result<Fn, Args...>::is_invocable>;

///\brief Our own std::is_invocable_v, if the library doesn't provide one.
///\ingroup objpipe_detail
///\note If the library does provide one, this is simply a using declaration.
template<typename Fn, typename... Args>
constexpr bool is_invocable_v = is_invocable<Fn, Args...>::value;

///\brief Our own std::is_nothrow_invocable, if the library doesn't provide one.
///\ingroup objpipe_detail
///\note If the library does provide one, this is simply a using declaration.
template<typename Fn, typename... Args>
using is_nothrow_invocable = std::integral_constant<bool, invoke_result<Fn, Args...>::is_nothrow_invocable>;

///\brief Our own std::is_nothrow_invocable_v, if the library doesn't provide one.
///\ingroup objpipe_detail
///\note If the library does provide one, this is simply a using declaration.
template<typename Fn, typename... Args>
constexpr bool is_nothrow_invocable_v = is_nothrow_invocable<Fn, Args...>::value;
#endif


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_INVOCABLE__H */
