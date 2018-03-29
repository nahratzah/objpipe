#ifndef OBJPIPE_DETAIL_ADAPTER_H
#define OBJPIPE_DETAIL_ADAPTER_H

///\file
///\ingroup objpipe_detail

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>
#include <stdexcept>
#include <limits>
#include <objpipe/reader.h>
#include <objpipe/errc.h>
#include <objpipe/detail/fwd.h>
#include <objpipe/detail/adapt.h>
#include <objpipe/push_policies.h>
#include <objpipe/detail/filter_op.h>
#include <objpipe/detail/flatten_op.h>
#include <objpipe/detail/peek_op.h>
#include <objpipe/detail/transform_op.h>
#include <objpipe/detail/deref_op.h>
#include <objpipe/detail/select_op.h>
#include <objpipe/detail/virtual.h>
#include <objpipe/detail/transport.h>
#include <objpipe/detail/push_op.h>

namespace objpipe::detail {


///\brief Wrap a source inside an adapter.
///\ingroup objpipe_detail
///\relates adapter_t
template<typename Source>
constexpr auto adapter(Source&& src)
noexcept(std::is_nothrow_constructible_v<adapter_t<std::decay_t<Source>>, Source>)
-> adapter_t<std::decay_t<Source>> {
  return adapter_t<std::decay_t<Source>>(std::forward<Source>(src));
}


/**
 * \brief Iterator for source.
 * \ingroup objpipe_detail
 *
 * \details
 * An input iterator, iterating over the elements of the given objpipe.
 */
template<typename Source>
class adapter_iterator {
 public:
  using value_type = adapt::value_type<Source>;
  using reference = adapt::pull_type<Source>;
  using difference_type = std::ptrdiff_t;
  using pointer = void;
  using iterator_category = std::input_iterator_tag;

  constexpr adapter_iterator() = default;

  constexpr adapter_iterator(Source& src)
  noexcept
  : src_(std::addressof(src))
  {}

  auto operator*() const
  -> reference {
    auto t = adapt::raw_pull(*src_);
    if (!t.has_value()) {
      assert(t.errc() != objpipe_errc::success);
      throw objpipe_error(t.errc());
    }
    return std::move(t).value();
  }

  auto operator++()
  -> adapter_iterator& {
    return *this;
  }

  auto operator++(int)
  noexcept
  -> void {
    ++*this;
  }

  constexpr auto operator==(const adapter_iterator& other) const
  noexcept(noexcept(std::declval<Source&>().wait()))
  -> bool {
    return (src_ == nullptr || src_->wait() == objpipe_errc::closed)
        && (other.src_ == nullptr || other.src_->wait() == objpipe_errc::closed);
  }

  constexpr auto operator!=(const adapter_iterator& other) const
  noexcept(noexcept(std::declval<Source&>().wait()))
  -> bool {
    return !(*this == other);
  }

 private:
  Source* src_ = nullptr;
};


/**
 * \brief Adapt a source to have all the objpipe functions.
 * \ingroup objpipe_detail
 *
 * \details
 * This class basically provides the throwing versions of each method.
 * It also applies transformations and handles various algorithms.
 *
 * \tparam Source The source that is to be wrapped.
 * \sa \ref objpipe::reader
 *
 * \bug
 * The adapter provides the front(), pop_front(), pull(), etc methods.
 * This necessitates it containing temporary \ref adapter_t::store_ "store variable".
 * Because of this, each intermediate operation creates and destroys this temporary
 * variable.
 * It dilutes the purpose of the adapter as a move-only builder and may make users
 * erroneously think they are allowed to mix \ref adapter_t::front() and \ref adapter_t::transform() for instance.
 * Consider splitting off those functions into a dedicated type that can be created by a call to ``as_queue()``.
 */
template<typename Source>
class adapter_t {
  // Validate requirements on Source.
  static_assert(std::is_move_constructible_v<Source>,
      "Source implementation must be move constructible.");
  static_assert(!std::is_swappable_v<Source>
      || std::is_same_v<virtual_pipe<adapt::value_type<Source>>, Source>
      || std::is_same_v<interlock_pipe<std::remove_reference_t<adapt::front_type<Source>>>, Source>,
      "Source implementation must not be swappable.");
  static_assert((!std::is_move_assignable_v<Source> && !std::is_copy_assignable_v<Source>)
      || std::is_same_v<virtual_pipe<adapt::value_type<Source>>, Source>
      || std::is_same_v<interlock_pipe<std::remove_reference_t<adapt::front_type<Source>>>, Source>,
      "Source implementation must not be assignable.");
  static_assert(std::is_same_v<
      adapt::value_type<Source>,
      std::remove_cv_t<std::remove_reference_t<adapt::front_type<Source>>>>,
      "front() must return a transport of value_type (with any const and reference qualifier).");
  static_assert(std::is_same_v<
      adapt::value_type<Source>,
      std::remove_cv_t<std::remove_reference_t<adapt::pull_type<Source>>>>,
      "pull() must return a transport of value_type (with any const and reference qualifier).");
  static_assert(std::is_same_v<
      adapt::value_type<Source>,
      std::remove_cv_t<std::remove_reference_t<adapt::try_pull_type<Source>>>>,
      "try_pull() must return a transport of value_type (with any const and reference qualifier).");
  static_assert(std::is_same_v<
      objpipe_errc,
      decltype(std::declval<Source&>().wait())>,
      "Source must implement wait() method, returning objpipe_errc.");
  static_assert(std::is_same_v<
      bool,
      decltype(std::declval<Source&>().is_pullable())>,
      "Source must implement is_pullable() method, returning bool.");

 private:
  using store_type = transport<adapt::front_type<Source>>;

 public:
  using value_type = adapt::value_type<Source>;
  using iterator = adapter_iterator<Source>;

  constexpr adapter_t() = default;

  ///\brief Constructor, wraps the given source.
  explicit constexpr adapter_t(Source&& src)
  noexcept(std::is_nothrow_move_constructible_v<Source>)
  : src_(std::move(src))
  {}

  ///\brief Swap objpipes.
  friend auto swap(adapter_t& x, adapter_t& y)
  noexcept(std::is_nothrow_swappable_v<Source>
      && std::is_nothrow_swappable_v<store_type>)
  -> void {
    using std::swap;
    swap(x.src_, y.src_);
    swap(x.store_, y.store_);
  }

  /**
   * \brief Tests if the objpipe is pullable.
   *
   * \details
   * An objpipe is pullable if it has remaining elements.
   *
   * Contrary to empty() or wait(), a pullable objpipe may still fail to yield a value.
   * This because the objpipe is also considered pullable if it has a writer attached,
   * which may detach between testing is_pullable and retrieval of a value.
   * In this scenario, the value retrieval would fail, due to the writer not having
   * supplied any.
   *
   * Best used to decide if an objpipe is ready for discarding:
   * false will be returned iff the objpipe has no pending elements and no
   * writers that can add future pending elements.
   *
   * \return False if the objpipe is empty and has no writers. True otherwise.
   */
  auto is_pullable()
  noexcept
  -> bool {
    return adapt::is_pullable(src_);
  }

  /**
   * \brief Wait until a new value is available.
   *
   * \details
   * If wait returns successfully, \ref try_pull() is certain not to yield an empty optional.
   *
   * \return An \ref ojbpipe::errc "objpipe_errc" indicating if the call succeeded.
   */
  auto wait() const
  noexcept(noexcept(adapt::wait(std::declval<Source&>())))
  -> objpipe_errc {
    if (store_.errc() != objpipe_errc::success)
      return store_.errc();
    return adapt::wait(src_);
  }

  /**
   * \brief Test if the objpipe is empty.
   *
   * \return True if the objpipe is empty, false if the objpipe has a pending value.
   */
  auto empty() const
  noexcept(noexcept(std::declval<Source&>().wait()))
  -> bool {
    if (store_.has_value()) return false;
    if (store_.errc() != objpipe_errc::success)
      return store_.errc() == objpipe_errc::closed;
    const objpipe_errc e = src_.wait();
    return e == objpipe_errc::closed;
  }

  /**
   * \brief Retrieve the next value in the objpipe, without advancing.
   *
   * \details
   * Retrieves the next value in the objpipe.
   *
   * Front will yield the same element, until \ref pop_front() or \ref pull() has been invoked.
   *
   * \return The next value in the objpipe.
   * \throw objpipe_error if the objpipe has no values.
   */
  auto front() const
  -> std::add_lvalue_reference_t<std::remove_reference_t<typename store_type::type>> {
    if (!store_.has_value() && store_.errc() == objpipe_errc::success)
      store_ = src_.front();
    assert(store_.has_value() || store_.errc() != objpipe_errc::success);

    if (store_.errc() != objpipe_errc::success)
      throw objpipe_error(store_.errc());
    return store_.ref();
  }

  /**
   * \brief Remove the next value from the objpipe.
   *
   * \details
   * This function advances the objpipe without returning the value.
   * \throw objpipe_error if the objpipe has no values.
   */
  auto pop_front()
  -> void {
    if (store_.errc() == objpipe_errc::success)
      store_.emplace(std::in_place_index<1>, src_.pop_front());

    if (store_.errc() != objpipe_errc::success)
      throw objpipe_error(store_.errc());
  }

  /**
   * \brief Try pulling a value from the objpipe.
   *
   * \details
   * Pulling removes a value from the objpipe, returning it.
   *
   * \return The next value in the objpipe, or an empty optional if the value is not (yet) available.
   * \throw objpipe_error if the objpipe is in an error state.
   *
   * \note If the objpipe is closed, try_pull will not throw an objpipe_error,
   * but instead return an empty optional.
   */
  auto try_pull()
  -> std::optional<value_type> {
    if (store_.errc() != objpipe_errc::success) {
      if (store_.errc() == objpipe_errc::closed) return {};
      throw objpipe_error(store_.errc());
    }

    if (store_.has_value()) {
      std::optional<value_type> rv = std::optional<value_type>(std::move(store_).value());
      store_.emplace(std::in_place_index<1>, src_.pop_front());
      if (store_.errc() != objpipe_errc::success) {
        assert(store_.errc() != objpipe_errc::closed);
        throw objpipe_error(store_.errc());
      }
      return rv;
    } else {
      auto t = adapt::raw_try_pull(src_);
      store_.emplace(std::in_place_index<1>, t.errc());
      if (store_.errc() != objpipe_errc::success
          && store_.errc() != objpipe_errc::closed) {
        throw objpipe_error(store_.errc());
      }

      if (t.has_value())
        return std::move(t).value();
      else
        return {};
    }
  }

  /**
   * \brief Pull a value from the objpipe.
   *
   * \details
   * Pulling removes a value from the objpipe, returning it.
   *
   * \return The next value in the objpipe.
   * \throw objpipe_error if the objpipe has no remaining values.
   */
  auto pull()
  -> value_type {
    if (store_.errc() != objpipe_errc::success)
      throw objpipe_error(store_.errc());

    if (store_.has_value()) {
      value_type rv = std::move(store_).value();
      store_.emplace(std::in_place_index<1>, src_.pop_front());
      if (store_.errc() != objpipe_errc::success)
        throw objpipe_error(store_.errc());
      return rv;
    } else {
      auto t = adapt::raw_pull(src_);
      assert(t.has_value() || t.errc() != objpipe_errc::success);
      store_.emplace(std::in_place_index<1>, t.errc());
      if (store_.errc() != objpipe_errc::success)
        throw objpipe_error(store_.errc());

      return std::move(t).value();
    }
  }

  /**
   * \brief Pull a value from the objpipe.
   *
   * \details
   * Pulling removes a value from the objpipe, returning it.
   *
   * If the pull fails, the error code argument will be set to indicate.
   * Otherwise, the error code argument will be set to objpipe_errc::success.
   *
   * \return The next value in the objpipe, if one is available.
   */
  auto pull(objpipe_errc& e)
  noexcept(std::is_nothrow_move_constructible_v<value_type>
      && std::is_nothrow_destructible_v<value_type>)
  -> std::optional<value_type> {
    if (store_.errc() != objpipe_errc::success) {
      e = store_.errc();
      return {};
    }

    if (store_.has_value()) {
      std::optional<value_type> rv = std::move(store_).value();
      store_.emplace(std::in_place_index<1>, src_.pop_front());
      e = store_.errc();
      if (store_.errc() != objpipe_errc::success) rv.reset();
      return rv;
    } else {
      auto t = adapt::raw_pull(src_);
      assert(t.has_value() || t.errc() != objpipe_errc::success);
      e = t.errc();
      store_.emplace(std::in_place_index<1>, t.errc());

      if (t.has_value() && store_.errc() == objpipe_errc::success)
        return std::move(t).value();
      else
        return {};
    }
  }

  /**
   * \brief Filter values in the objpipe.
   *
   * \details
   * The objpipe values omitted, unless the predicate returns true.
   *
   * \param[in] pred A predicate to invoke on each element.
   * \return An objpipe, with only elements for which the predicate yields true.
   */
  template<typename Pred>
  auto filter(Pred&& pred) &&
  noexcept(noexcept(adapter(adapt::filter(std::declval<Source>(), std::forward<Pred>(pred)))))
  -> decltype(auto) {
    return adapter(adapt::filter(std::move(src_), std::forward<Pred>(pred)));
  }

  /**
   * \brief Functional transformation of each element.
   *
   * \details
   * The functor is invoked for each element, and the elements are replaced by the result of the functor.
   *
   * The functor does the right thing when a reference is returned.
   *
   * \note When returning a reference, the value of the reference may be
   * mutated by subsequent transformations in the objpipe, unless the reference
   * is const.
   *
   * \param[in] fn Functor that is invoked for each element.
   * \return An objpipe, where each value is replaced by the result of the \p fn invocation.
   */
  template<typename Fn>
  auto transform(Fn&& fn) &&
  noexcept(noexcept(adapter(adapt::transform(std::declval<Source>(), std::forward<Fn>(fn)))))
  -> decltype(auto) {
    return adapter(adapt::transform(std::move(src_), std::forward<Fn>(fn)));
  }

  /**
   * \brief Transform each element by dereferencing it.
   *
   * \details
   * Invokes
   * \code
   * *element
   * \endcode
   * for each element in the objpipe.
   *
   * \returns An objpipe of the dereferenced elements from the original objpipe.
   */
  template<bool Enable = deref_op::is_valid<value_type>>
  auto deref() &&
  noexcept(noexcept(std::declval<adapter_t>().transform(deref_op())))
  -> decltype(auto) {
    return std::move(*this).transform(deref_op());
  }

  /**
   * \brief Transform each element by invoking std::get<Idx>.
   * \returns An objpipe of elements selected using the given \p Index.
   */
  template<size_t Index>
  auto select() &&
  noexcept(noexcept(std::declval<adapter_t>().transform(
              std::enable_if_t<
                  select_index_op<Index>::template is_valid<value_type>,
                  select_index_op<Index>>())))
  -> decltype(std::declval<adapter_t>().transform(
          std::enable_if_t<
              select_index_op<Index>::template is_valid<value_type>,
              select_index_op<Index>>())) {
    return std::move(*this).transform(select_index_op<Index>());
  }

  /**
   * \brief Transform each element by invoking std::get<Type>.
   * \returns An objpipe of elements selected using the given \p Type.
   */
  template<typename Type>
  auto select() &&
  noexcept(noexcept(std::declval<adapter_t>().transform(
              std::enable_if_t<
                  select_type_op<Type>::template is_valid<value_type>,
                  select_type_op<Type>>())))
  -> decltype(std::declval<adapter_t>().transform(
          std::enable_if_t<
              select_type_op<Type>::template is_valid<value_type>,
              select_type_op<Type>>())) {
    return std::move(*this).transform(select_type_op<Type>());
  }

  /**
   * \brief Transform each element pair by selecting its first member.
   * \returns An objpipe of the first member-variable of elements in the original objpipe.
   */
  template<bool Enable = select_first_op::is_valid<value_type>>
  auto select_first() &&
  noexcept(noexcept(std::declval<adapter_t>().transform(
              std::enable_if_t<Enable, select_first_op>())))
  -> decltype(std::declval<adapter_t>().transform(
          std::enable_if_t<Enable, select_first_op>())) {
    return std::move(*this).transform(select_first_op());
  }

  /**
   * \brief Transform each element pair by selecting its second member.
   * \returns An objpipe of the second member-variable of elements in the original objpipe.
   */
  template<bool Enable = select_second_op::is_valid<value_type>>
  auto select_second() &&
  noexcept(noexcept(std::declval<adapter_t>().transform(
              std::enable_if_t<Enable, select_second_op>())))
  -> decltype(std::declval<adapter_t>().transform(
          std::enable_if_t<Enable, select_second_op>())) {
    return std::move(*this).transform(select_second_op());
  }

  /**
   * \brief Mutate or inspect values as they are passed through the objpipe.
   *
   * \details
   *
   * \param[in] fn A functor that is invoked for each element.
   * The functor is allowed, but not required to, modify the element in place.
   * \return An objpipe, where \p fn is invoked for each element.
   */
  template<typename Fn>
  auto peek(Fn&& fn) &&
  noexcept(noexcept(adapter(adapt::peek(std::declval<Source>(), std::forward<Fn>(fn)))))
  -> decltype(auto) {
    return adapter(adapt::peek(std::move(src_), std::forward<Fn>(fn)));
  }

  /**
   * \brief Assert that the given predicate holds for values in the objpipe.
   *
   * \param[in] fn A predicate returning a boolean.
   * \return The same objpipe.
   *
   * \bug Assertions are not implemented.
   */
  template<typename Fn>
  auto assertion(Fn&& fn) &&
  noexcept(noexcept(adapter(adapt::assertion(std::declval<Source>(), std::forward<Fn>(fn)))))
  -> decltype(auto) {
    return adapter(adapt::assertion(std::move(src_), std::forward<Fn>(fn)));
  }

  /**
   * \brief Transformation operation that replaces each iterable element in the objpipe with its elements.
   *
   * \details
   * If the objpipe holds a type that has an iterator begin() and end() function,
   * the operation will replace the collection with the values in it.
   *
   * Ojbpipes, as well as all the STL collection types, are iterable.
   *
   * \return An objpipe where each collection element is replaced with its elements.
   */
  template<bool Enable = can_flatten<Source>>
  auto iterate()
  noexcept(noexcept(adapter(adapt::flatten(std::declval<Source>()))))
  -> decltype(adapter(adapt::flatten(std::declval<std::enable_if_t<Enable, Source>>()))) {
    return adapter(adapt::flatten(std::move(src_)));
  }

  /**
   * \brief Retrieve begin iterator.
   */
  constexpr auto begin() -> iterator {
    return iterator(src_);
  }

  /**
   * \brief Retrieve end iterator.
   */
  constexpr auto end() -> iterator {
    return iterator();
  }

  /**
   * \brief Apply functor on each value of the objpipe.
   *
   * \param[in] fn The functor to apply to each element.
   * \return \p fn
   */
  template<typename Fn>
  auto for_each(Fn&& fn) &&
  -> decltype(std::for_each(begin(), end(), std::forward<Fn>(fn))) {
    return std::for_each(begin(), end(), std::forward<Fn>(fn));
  }

  /**
   * \brief Perform a reduction.
   *
   * \note Accumulate uses a left-fold operation.
   * \param[in] fn A binary function.
   * \return The result of the accumulation, or an empty optional if the objpipe has no values.
   */
  template<typename Fn>
  constexpr auto accumulate(Fn&& fn) &&
  -> std::optional<value_type> {
    std::optional<value_type> result;
    objpipe_errc e = objpipe_errc::success;

    {
      transport<adapt::pull_type<Source>> v =
          adapt::raw_pull(src_);
      e = v.errc();

      if (e == objpipe_errc::success)
        result.emplace(std::move(v).value());
    }

    while (e == objpipe_errc::success) {
      transport<adapt::pull_type<Source>> v =
          adapt::raw_pull(src_);
      e = v.errc();

      if (e == objpipe_errc::success)
        result.emplace(std::invoke(fn, *std::move(result), std::move(v).value()));
    }

    if (e != objpipe_errc::success && e != objpipe_errc::closed)
      throw objpipe_error(e);
    return result;
  }

  /**
   * \brief Perform a reduction.
   *
   * \note Accumulate uses a left-fold operation.
   * \param[in] init The initial value of the accumulation.
   * \param[in] fn A binary function.
   * \return The result of the accumulation.
   */
  template<typename Init, typename Fn>
  constexpr auto accumulate(Init init, Fn&& fn) &&
  -> Init {
    objpipe_errc e = objpipe_errc::success;

    while (e == objpipe_errc::success) {
      transport<adapt::pull_type<Source>> v =
          adapt::raw_pull(src_);
      e = v.errc();

      if (e == objpipe_errc::success)
        init = std::invoke(fn, std::move(init), std::move(v).value());
    }

    if (e != objpipe_errc::success && e != objpipe_errc::closed)
      throw objpipe_error(e);
    return init;
  }

  /**
   * \brief Perform a reduction.
   *
   * \note The order in which the reduction is performed is unspecified.
   * \param[in] fn A binary function.
   * \return The result of the reduction, or an empty optional if the objpipe is empty.
   */
  template<typename Fn>
  constexpr auto reduce(Fn&& fn) &&
  -> std::optional<value_type> {
    return std::move(*this).accumulate(std::forward<Fn>(fn));
  }

  /**
   * \brief Perform a reduction.
   *
   * \note The order in which the reduction is performed is unspecified.
   * \param[in] init The initial value of the reduction.
   * \param[in] fn A binary function.
   * \return The result of the reduction.
   */
  template<typename Init, typename Fn>
  constexpr auto reduce(Init&& init, Fn&& fn) &&
  -> Init {
    return std::move(*this).accumulate(std::forward<Init>(init),
        std::forward<Fn>(fn));
  }

  /**
   * \brief Count the number of elements in the objpipe.
   * \return The number of elements in the objpipe.
   */
  constexpr auto count() &&
  -> std::uintmax_t {
    std::uintmax_t result = 0;
    objpipe_errc e = src_.pop_front();
    while (e == objpipe_errc::success) {
      if (result == std::numeric_limits<std::uintmax_t>::max())
        throw std::overflow_error("objpipe count overflow");
      ++result;
      e = src_.pop_front();
    }

    if (e != objpipe_errc::closed)
      throw objpipe_error(e);
    return result;
  }

  /**
   * \brief Copy the values in the objpipe into the given output iterator.
   *
   * \param[out] out An output iterator to which the values are to be copied.
   * \return \p out
   */
  template<typename OutputIterator>
  auto copy(OutputIterator&& out) &&
  -> decltype(std::copy(begin(), end(), std::forward<OutputIterator>(out))) {
    return std::copy(begin(), end(), std::forward<OutputIterator>(out));
  }

  /**
   * \brief Retrieve the min element from the objpipe.
   *
   * \param[in] pred A predicate to compare values.
   * \return The min value.
   * If the objpipe has no elements, an empty optional is returned.
   */
  template<typename Pred = std::less<value_type>>
  auto min(Pred&& pred = Pred()) &&
  -> std::optional<value_type> {
    std::optional<value_type> result;

    for (;;) {
      transport<adapt::pull_type<Source>> v = src_.pull();
      if (!v.has_value()) {
        assert(v.errc() != objpipe_errc::success);
        if (v.errc() == objpipe_errc::closed) break;
        throw objpipe_error(v.errc());
      }

      if (!result.has_value()) {
        result.emplace(std::move(v).value());
      } else {
        const auto& result_value = *result;
        const auto& v_value = v.value();
        if (std::invoke(pred, v_value, result_value))
          result.emplace(std::move(v).value());
      }
    }
    return result;
  }

  /**
   * \brief Retrieve the max element from the objpipe.
   *
   * \param[in] pred A predicate to compare values.
   * \return The max value.
   * If the objpipe has no elements, an empty optional is returned.
   */
  template<typename Pred = std::less<value_type>>
  auto max(Pred&& pred = Pred()) &&
  -> std::optional<value_type> {
    std::optional<value_type> result;

    for (;;) {
      transport<adapt::pull_type<Source>> v = src_.pull();
      if (!v.has_value()) {
        assert(v.errc() != objpipe_errc::success);
        if (v.errc() == objpipe_errc::closed) break;
        throw objpipe_error(v.errc());
      }

      if (!result.has_value()) {
        result.emplace(std::move(v).value());
      } else {
        const auto& result_value = *result;
        const auto& v_value = v.value();
        if (std::invoke(pred, result_value, v_value))
          result.emplace(std::move(v).value());
      }
    }
    return result;
  }

  ///\brief Retrieve objpipe values as a vector.
  template<typename Alloc = std::allocator<value_type>>
  auto to_vector(Alloc alloc = Alloc()) &&
  -> std::vector<value_type, Alloc> {
    return std::vector<value_type, Alloc>(begin(), end(), alloc);
  }

  ///\brief Convert adapter into a reader.
  operator reader<value_type>() && {
    return std::move(*this).as_reader();
  }

  ///\brief Convert adapter into a reader.
  auto as_reader() &&
  -> reader<value_type> {
    return reader<value_type>(virtual_pipe<value_type>(std::move(src_)));
  }

  /**
   * \brief Perform a sequence of operations on an objpipe.
   *
   * \details
   * This allows a pattern of
   * \code
   * MyObjPipe
   *     .perform(SequenceOfOperationsFn)
   * \endcode
   * to be used, in favour of
   * \code
   * SequenceOfOperations(MyObjPipe)
   * \endcode
   *
   * The latter becomes very hard to read when using chains:
   * \code
   * SequenceOfOperations(
   *     of(1, 2, 3, 4)
   *         .map(...)
   *         .map(...)
   *         .filter(...)
   *         // etc
   *     )
   *     .map(...)
   *     // etc
   *     ;
   * \endcode
   *
   * The nesting of the former becomes nightmarish, especially if multiple
   * functions are done thusly.
   *
   * Instead, replace with:
   * \code
   * of(1, 2, 3, 4)
   *     .map(...)
   *     .map(...)
   *     .filter(...)
   *     // etc
   *     .perform(SequenceOfOperations)
   *     .map(...)
   *     // etc
   *     ;
   * \endcode
   * This has the advantage that it shows SequenceOfOperations at the point in
   * the call chain where it is actually used.
   *
   * \param seq_op [in] An invokable.
   * It is invoked with this objpipe passed rvalue reference.
   *
   * \note \p seq_op is not required to return an objpipe.
   * It is valid to evaluate the objpipe.
   */
  template<typename SeqOp>
  auto perform(SeqOp&& seq_op) &&
  noexcept(is_nothrow_invocable_v<std::add_rvalue_reference_t<SeqOp>, adapter_t&&>)
  -> invoke_result_t<std::add_rvalue_reference_t<SeqOp>, adapter_t&&> {
    return std::invoke(std::forward<SeqOp>(seq_op), std::move(*this));
  }

  /**
   * \brief Retrieve the underlying source.
   */
  auto underlying() const &
  -> const Source& {
    return src_;
  }

  /**
   * \brief Retrieve the underlying source.
   */
  auto underlying() &
  -> Source& {
    return src_;
  }

  /**
   * \brief Retrieve the underlying source.
   */
  auto underlying() &&
  -> Source&& {
    return std::move(src_);
  }

  auto async() &&
  -> decltype(auto) {
    return std::move(*this).async(singlethread_push());
  }

  template<typename PushTag>
  auto async(PushTag push_tag)
  -> std::enable_if_t<std::is_base_of_v<existingthread_push, std::decay_t<PushTag>>, async_adapter_t<Source, std::decay_t<PushTag>>> {
    return async_adapter_t<Source, std::decay_t<PushTag>>(std::move(src_), std::move(push_tag));
  }

 private:
  ///\brief Underlying source.
  mutable Source src_;
  ///\brief Stored result of front().
  ///details When front() is called, store_ will be set to its result.
  ///pop_front(), as well as pull() and try_pull(), will clear it again.
  ///
  ///This enables us to allow multiple calls to front() to do the right thing.
  mutable store_type store_{ std::in_place_index<1>, objpipe_errc::success };
};

static_assert(is_adapter_v<reader<int>>,
    "is_adapter does not function properly for reader");
static_assert(std::is_same_v<adapter_underlying_type_t<reader<int>>, virtual_pipe<int>>,
    "adapter_underlying_type does not function properly for reader");


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_ADAPTER_H */
