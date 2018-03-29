#ifndef OBJPIPE_DETAIL_FWD_H
#define OBJPIPE_DETAIL_FWD_H

///\file
///\ingroup objpipe_detail

namespace objpipe::detail {


template<typename Source, typename... Pred>
class filter_op;

template<typename Source, typename... Fn>
class transform_op;

template<typename Source, typename Fn>
class assertion_op;

template<typename Source>
class flatten_op;

template<typename Source>
class adapter_t;

template<typename T>
class interlock_pipe;


namespace {
template<typename Source> void adapter_fn_(const adapter_t<Source>&) {}
template<typename T, typename = void>
struct is_adapter_
: std::false_type
{};
template<typename T>
struct is_adapter_<T, std::void_t<decltype(adapter_fn_(std::declval<T>()))>>
: std::true_type
{};
}
///\brief Tests if T is an adapter.
template<typename T>
using is_adapter = typename is_adapter_<std::decay_t<T>>::type;
///\brief Tests if T is an adapter.
template<typename T>
constexpr bool is_adapter_v = is_adapter<T>::value;

template<typename T, bool = is_adapter_v<T>>
struct adapter_underlying_type_ {};
template<typename T>
struct adapter_underlying_type_<T, true> {
  using type = std::decay_t<decltype(std::declval<T>().underlying())>;
};
///\brief The source type used by an adapter.
///\details The member type ``type`` is the unqualified source implementation.
///If the type is not an adapter type, the ``type`` element shall be omitted.
///\tparam T An adapter implementation.
template<typename T>
struct adapter_underlying_type
: adapter_underlying_type_<T>
{};
///\brief The underlying source type of an adapter.
///\relates adapter_underlying_type
template<typename T>
using adapter_underlying_type_t = typename adapter_underlying_type<T>::type;


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_FWD_H */
