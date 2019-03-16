#ifndef OBJPIPE_DETAIL_TRANSFORM_OP_H
#define OBJPIPE_DETAIL_TRANSFORM_OP_H

///\file
///\ingroup objpipe_detail

#include <functional>
#include <type_traits>
#include <utility>
#include <objpipe/detail/fwd.h>
#include <objpipe/detail/adapt.h>
#include <objpipe/detail/invocable_.h>
#include <objpipe/detail/transport.h>
#include <objpipe/detail/push_op.h>

namespace objpipe::detail {


template<typename Sink, typename Fn>
class transform_push {
  static_assert(std::is_copy_constructible_v<Fn>,
      "Functor must be copy constructible.");

 public:
  transform_push(Sink&& sink, Fn&& fn)
  noexcept(std::is_nothrow_move_constructible_v<Sink>
      && std::is_nothrow_move_constructible_v<Fn>)
  : sink_(std::move(sink)),
    fn_(std::move(fn))
  {}

  template<typename T>
  auto operator()(T&& v)
  -> objpipe_errc {
    return sink_(fn_(std::forward<T>(v)));
  }

  auto push_exception(std::exception_ptr exptr)
  noexcept {
    sink_.push_exception(std::move(exptr));
  }

 private:
  Sink sink_;
  Fn fn_; // transform_fn_adapter.
};


struct transform_identity_fn {
  template<typename Arg>
  constexpr auto operator()(Arg&& arg) const
  noexcept
  -> std::add_rvalue_reference_t<Arg> {
    return std::forward<Arg>(arg);
  }
};

template<typename Arg, typename Fn, typename... Tail>
class transform_fn_adapter {
 private:
  using inner_fn_type = transform_fn_adapter<Arg, Fn>;
  using outer_fn_type = transform_fn_adapter<
      std::remove_cv_t<std::remove_reference_t<invoke_result_t<const inner_fn_type&, Arg>>>,
      Tail...>;

 public:
  template<typename Arg0, typename... Args>
  explicit constexpr transform_fn_adapter(Arg0&& arg0, Args&&... args)
  noexcept(std::is_nothrow_constructible_v<inner_fn_type, std::add_rvalue_reference_t<Arg0>>
      && std::is_nothrow_constructible_v<outer_fn_type, std::add_rvalue_reference_t<Args>...>)
  : inner_(std::forward<Arg0>(arg0)),
    outer_(std::forward<Args>(args)...)
  {}

  template<typename X>
  constexpr auto operator()(X&& x) const
  noexcept(is_nothrow_invocable_v<const inner_fn_type&, std::add_rvalue_reference_t<X>, const outer_fn_type&>)
  -> invoke_result_t<const inner_fn_type&, std::add_rvalue_reference_t<X>, const outer_fn_type&> {
    return std::invoke(inner_, std::forward<X>(x), outer_);
  }

  template<typename NextFn>
  constexpr auto extend(NextFn&& next_fn) &&
  noexcept(std::conjunction_v<
      std::is_nothrow_move_constructible<Fn>,
      std::is_nothrow_move_constructible<Tail>...,
      std::is_nothrow_constructible<std::decay_t<NextFn>, std::add_rvalue_reference_t<NextFn>>>)
  -> decltype(auto) {
    return transform_fn_adapter<Arg, Fn, Tail..., std::decay_t<NextFn>>(
        std::move(inner_),
        std::move(outer_).extend(std::forward<NextFn>(next_fn)));
  }

 private:
  inner_fn_type inner_;
  outer_fn_type outer_;
};

/**
 * \brief Propagate references for another type, only if T is a non-volatile reference.
 * \ingroup objpipe_detail
 *
 * \details
 * This helper type takes a template argument T,
 * including its const, volatile, and reference modifiers.
 *
 * It then uses those to decide if a related type is allowed to have its
 * modifiers preserved across function return.
 *
 * \tparam T The input type for a function.
 */
template<typename T>
struct propagate_copy {
  static constexpr bool is_volatile = std::is_volatile_v<T>;
  static constexpr bool is_lref = std::is_lvalue_reference_v<T>;
  static constexpr bool is_rref = std::is_rvalue_reference_v<T>;
  ///\brief Decide if modifiers are to be propagated.
  static constexpr bool propagate = !is_volatile && (is_lref || is_rref);

  /**
   * \brief Propagate reference only if the input type is a reference.
   *
   * \details
   * Clears reference decorations from U, if those are not to be propagated.
   * Otherwise, U is passed through unchanged.
   *
   * \tparam U A type, including its const, volatile, and reference modifiers,
   * which are to be cleared iff they are not to be propagated.
   */
  template<typename U>
  using type = std::conditional_t<
      propagate,
      U,
      std::remove_cv_t<std::remove_reference_t<U>>>;
};

template<typename Arg, typename Fn>
class transform_fn_adapter<Arg, Fn> {
 private:
  using argument_type = Arg;
  using functor_type = Fn;

  static_assert(!std::is_const_v<functor_type>,
      "Functor may not be a const type.");
  static_assert(!std::is_volatile_v<functor_type>,
      "Functor may not be a volatile type.");
  static_assert(!std::is_lvalue_reference_v<functor_type>
      && !std::is_rvalue_reference_v<functor_type>,
      "Functor may not be a reference.");

  static_assert(!std::is_const_v<argument_type>,
      "Argument may not be a const type.");
  static_assert(!std::is_volatile_v<argument_type>,
      "Argument may not be a volatile type.");
  static_assert(!std::is_lvalue_reference_v<argument_type>
      && !std::is_rvalue_reference_v<argument_type>,
      "Argument may not be a reference.");

  using arg_cref = std::add_lvalue_reference_t<std::add_const_t<argument_type>>;
  using arg_lref = std::add_lvalue_reference_t<argument_type>;
  using arg_rref = std::add_rvalue_reference_t<argument_type>;

  static constexpr bool is_cref_invocable =
      is_invocable_v<const functor_type&, arg_cref>;
  static constexpr bool is_lref_invocable =
      is_invocable_v<const functor_type&, arg_lref>;
  static constexpr bool is_rref_invocable =
      is_invocable_v<const functor_type&, arg_rref>;

  static_assert(is_cref_invocable || is_lref_invocable || is_rref_invocable,
      "Functor must be invocable with the argument type");

  ///\brief Helper that decides if the returned value from an invocation chain is to be copied into the return value.
  template<typename NextFn, typename DecoratedArg>
  using propagate_type = typename propagate_copy<invoke_result_t<const functor_type&, DecoratedArg>>::template type<invoke_result_t<NextFn, invoke_result_t<const functor_type&, DecoratedArg>>>;

 public:
  explicit constexpr transform_fn_adapter(const functor_type& fn)
  noexcept(std::is_nothrow_copy_constructible_v<functor_type>)
  : fn_(fn)
  {}

  explicit constexpr transform_fn_adapter(functor_type&& fn)
  noexcept(std::is_nothrow_move_constructible_v<functor_type>)
  : fn_(std::move(fn))
  {}

  template<typename NextFn = transform_identity_fn>
  constexpr auto operator()(arg_cref v,
      const NextFn& next_fn = transform_identity_fn()) const
  noexcept(noexcept(
          std::invoke(next_fn,
              std::invoke(fn_,
                  std::declval<std::conditional_t<
                      is_cref_invocable,
                      arg_cref,
                      std::conditional_t<
                          is_rref_invocable,
                          arg_rref,
                          arg_lref
                      >
                  >>()
              )
          )))
  -> typename propagate_copy<std::conditional_t<is_cref_invocable, arg_cref, argument_type>>::template type<
      propagate_type<
          const NextFn&,
          typename std::conditional_t<
              is_cref_invocable,
              arg_cref,
              std::conditional_t<
                  is_rref_invocable,
                  arg_rref,
                  arg_lref
              >
          >
      >> {
    if constexpr(is_cref_invocable) {
      return std::invoke(next_fn, std::invoke(fn_, v));
    } else if constexpr(is_rref_invocable) {
      argument_type tmp = v;
      return std::invoke(next_fn, std::invoke(fn_, std::move(tmp)));
    } else {
      argument_type tmp = v;
      return std::invoke(next_fn, std::invoke(fn_, tmp));
    }
  }

  template<bool Enable = is_lref_invocable, typename NextFn = transform_identity_fn>
  constexpr auto operator()(arg_lref v,
      const NextFn& next_fn = transform_identity_fn()) const
  noexcept(is_nothrow_invocable_v<const functor_type&, arg_lref>
      && is_nothrow_invocable_v<const NextFn&, invoke_result_t<const functor_type&, arg_lref>>)
  -> std::enable_if_t<Enable, propagate_type<const NextFn&, arg_lref>> {
    return std::invoke(next_fn, std::invoke(fn_, v));
  }

  template<bool Enable = is_rref_invocable, typename NextFn = transform_identity_fn>
  constexpr auto operator()(arg_rref v,
      const NextFn& next_fn = transform_identity_fn()) const
  noexcept(is_nothrow_invocable_v<const functor_type&, arg_rref>)
  -> std::enable_if_t<Enable, propagate_type<const NextFn&, arg_rref>> {
    return std::invoke(next_fn, std::invoke(fn_, std::move(v)));
  }

  template<typename NextFn>
  constexpr auto extend(NextFn&& next_fn) &&
  noexcept(std::conjunction_v<
      std::is_nothrow_move_constructible<Fn>,
      std::is_nothrow_constructible<std::decay_t<NextFn>, std::add_rvalue_reference_t<NextFn>>>)
  -> decltype(auto) {
    return transform_fn_adapter<Arg, Fn, std::decay_t<NextFn>>(
        std::move(*this),
        std::forward<NextFn>(next_fn));
  }

 private:
  functor_type fn_;
};

/**
 * \brief Implements the transform operation.
 * \implements TransformationConcept
 * \implements IocPushConcept
 * \ingroup objpipe_detail
 *
 * \tparam Source The source on which to operate.
 * \tparam Fn Functors to be invoked on the values.
 * Inner most function is mentioned first, outer most function is mentioned last.
 *
 * \sa \ref objpipe::detail::adapter_t::transform
 */
template<typename Source, typename... Fn>
class transform_op {
 private:
  using fn_type =
      transform_fn_adapter<adapt::value_type<Source>, Fn...>;

  using raw_src_front_type = adapt::front_type<Source>;
  using raw_front_type = invoke_result_t<const fn_type&, raw_src_front_type>;
  using front_type = typename propagate_copy<raw_src_front_type>::template type<raw_front_type>;

  using raw_src_try_pull_type = adapt::try_pull_type<Source>;
  using raw_try_pull_type = invoke_result_t<const fn_type&, raw_src_try_pull_type>;
  using try_pull_type = typename propagate_copy<raw_src_try_pull_type>::template type<raw_try_pull_type>;

  using raw_src_pull_type = adapt::pull_type<Source>;
  using raw_pull_type = invoke_result_t<const fn_type&, raw_src_pull_type>;
  using pull_type = typename propagate_copy<raw_src_pull_type>::template type<raw_pull_type>;

  static_assert(std::is_same_v<std::decay_t<raw_src_front_type>, std::decay_t<raw_src_try_pull_type>>
      && std::is_same_v<std::decay_t<raw_src_front_type>, std::decay_t<raw_src_pull_type>>,
      "Decayed return types of front(), try_pull() and pull() in source must be the same");

 public:
  template<typename... FnAdapterArgs>
  explicit constexpr transform_op(Source&& src, FnAdapterArgs&&... fn_adapter_args)
  noexcept(std::is_nothrow_move_constructible_v<Source>
      && std::is_nothrow_constructible_v<fn_type, FnAdapterArgs&&...>)
  : src_(std::move(src)),
    fn_(std::forward<FnAdapterArgs>(fn_adapter_args)...)
  {}

  transform_op(const transform_op&) = delete;

  constexpr transform_op(transform_op&&)
  noexcept(std::conjunction_v<
      std::is_nothrow_move_constructible<Source>,
      std::is_nothrow_move_constructible<Fn>...>) = default;

  transform_op& operator=(const transform_op&) = delete;
  transform_op& operator=(transform_op&&) = delete;

  constexpr auto is_pullable()
  noexcept
  -> bool {
    return src_.is_pullable();
  }

  constexpr auto wait()
  noexcept(noexcept(src_.wait()))
  -> objpipe_errc {
    return src_.wait();
  }

  constexpr auto front()
  noexcept(noexcept(invoke_fn_(src_.front()))
      && (std::is_lvalue_reference_v<front_type>
          || std::is_rvalue_reference_v<front_type>
          || std::is_nothrow_constructible_v<std::decay_t<front_type>, raw_front_type>))
  -> transport<front_type> {
    return invoke_fn_(src_.front());
  }

  constexpr auto pop_front()
  noexcept(noexcept(src_.pop_front()))
  -> objpipe_errc {
    return src_.pop_front();
  }

  template<bool Enable = adapt::has_try_pull<Source>>
  constexpr auto try_pull()
  noexcept(noexcept(invoke_fn_(adapt::raw_try_pull(src_))))
  -> std::enable_if_t<Enable, transport<try_pull_type>> {
    return invoke_fn_(adapt::raw_try_pull(src_));
  }

  template<bool Enable = adapt::has_pull<Source>>
  constexpr auto pull()
  noexcept(noexcept(invoke_fn_(adapt::raw_pull(src_))))
  -> std::enable_if_t<Enable, transport<pull_type>> {
    return invoke_fn_(adapt::raw_pull(src_));
  }

  template<typename NextFn>
  constexpr auto transform(NextFn&& next_fn) &&
  noexcept(noexcept(
          transform_op<Source, Fn..., NextFn>(
              std::move(src_),
              std::move(fn_).extend(std::forward<NextFn>(next_fn)))))
  -> transform_op<Source, Fn..., NextFn> {
    return transform_op<Source, Fn..., NextFn>(
        std::move(src_),
        std::move(fn_).extend(std::forward<NextFn>(next_fn)));
  }

  template<typename PushTag>
  auto can_push(PushTag tag) const
  noexcept
  -> std::enable_if_t<adapt::has_ioc_push<Source, PushTag>, bool> {
    return src_.can_push(tag);
  }

  template<typename PushTag, typename Acceptor>
  auto ioc_push(PushTag tag, Acceptor&& acceptor) &&
  noexcept
  -> std::enable_if_t<adapt::has_ioc_push<Source, PushTag>> {
    adapt::ioc_push(
        std::move(src_),
        tag,
        transform_push<std::decay_t<Acceptor>, fn_type>(std::forward<Acceptor>(acceptor), std::move(fn_)));
  }

 private:
  template<typename T>
  constexpr auto invoke_fn_(transport<T>&& v) const
  noexcept(is_nothrow_invocable_v<const fn_type&, decltype(std::declval<transport<T>>().value())>)
  -> transport<invoke_result_t<const fn_type&, decltype(std::declval<transport<T>>().value())>> {
    using result_type = transport<invoke_result_t<const fn_type&, decltype(std::declval<transport<T>>().value())>>;

    if (v.has_value())
      return result_type(std::in_place_index<0>, std::invoke(fn_, std::move(v).value()));
    else
      return result_type(std::in_place_index<1>, std::move(v).errc());
  }

  Source src_;
  fn_type fn_;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_TRANSFORM_OP_H */
