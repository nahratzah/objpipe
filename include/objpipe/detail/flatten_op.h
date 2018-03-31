#ifndef OBJPIPE_DETAIL_FLATTEN_OP_H
#define OBJPIPE_DETAIL_FLATTEN_OP_H

///\file
///\ingroup objpipe_detail

#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>
#include <objpipe/detail/fwd.h>
#include <objpipe/detail/transport.h>
#include <objpipe/detail/adapt.h>

namespace objpipe::detail {


using std::make_move_iterator;
using std::begin;
using std::end;

///\brief Invoke begin(collection), using ADL.
///\ingroup objpipe_detail
///\relates objpipe::detail::flatten_op
///\relates objpipe::detail::flatten_push
template<typename Collection, typename = std::void_t<decltype(begin(std::declval<Collection&>()))>>
constexpr auto flatten_op_begin_(Collection& c)
noexcept(noexcept(begin(std::declval<Collection&>())))
-> std::enable_if_t<
    std::is_base_of_v<
        std::input_iterator_tag,
        typename std::iterator_traits<decltype(begin(c))>::iterator_category>,
    decltype(begin(c))> {
  return begin(c); // ADL, with fallback to std::begin
}

///\brief Invoke end(collection), using ADL.
///\ingroup objpipe_detail
///\relates objpipe::detail::flatten_op
///\relates objpipe::detail::flatten_push
template<typename Collection, typename = std::void_t<decltype(end(std::declval<Collection&>()))>>
constexpr auto flatten_op_end_(Collection& c)
noexcept(noexcept(end(std::declval<Collection&>())))
-> decltype(end(c)) {
  return end(c); // ADL, with fallback to std::end
}

///\brief Trait that tests if the elements in Source can be iterated over.
///\ingroup objpipe_detail
///\details Default case is that it can not be iterated over.
///\tparam An objpipe source.
template<typename Source, typename = void>
struct can_flatten_
: std::false_type
{};

///\brief Trait that tests if the elements in Source can be iterated over.
///\ingroup objpipe_detail
///\details Specialization for the case where it can be iterated over.
///\tparam An objpipe source.
template<typename Source>
struct can_flatten_<Source,
    std::void_t<decltype(flatten_op_begin_(std::declval<adapt::value_type<Source>&>())),
        decltype(flatten_op_end_(std::declval<adapt::value_type<Source>&>()))>>
: std::true_type
{};

///\brief Trait, that tests if the elements in Source can be iterated over.
///\ingroup objpipe_detail
///\details
///An object can be iterated over, if begin(Element) and end(Element) are invocable, when located via ADL.
///And the type returned by begin(Element) has an iterator category of at least input_iterator_tag.
///\tparam An objpipe source.
template<typename Source>
constexpr bool can_flatten = can_flatten_<Source>::value;


template<typename Collection, typename Sink, typename TagType>
class flatten_push {
 public:
  explicit flatten_push(Sink&& sink, TagType&& tag)
  noexcept(std::is_nothrow_move_constructible_v<Sink>)
  : sink_(std::move(sink)),
    tag_(std::move(tag))
  {}

  explicit flatten_push(Sink&& sink, const TagType& tag)
  noexcept(std::is_nothrow_move_constructible_v<Sink>)
  : sink_(std::move(sink)),
    tag_(tag)
  {}

  auto operator()(Collection&& c)
  -> objpipe_errc {
    using std::swap;

    if constexpr(!std::is_base_of_v<multithread_push, TagType>) {
      auto b = make_move_iterator(flatten_op_begin_(c));
      auto e = make_move_iterator(flatten_op_end_(c));
      objpipe_errc error_code = objpipe_errc::success;

      while (b != e && error_code == objpipe_errc::success) {
        error_code = sink_(*b);
        ++b;
      }
      return error_code;
    } else {
      Sink dst = sink_; // Copy.
      swap(dst, sink_); // Keep hold of successor.

      // If collection is an adapter and we can use ioc_push on it,
      // use that in favour of normal iteration.
      if constexpr(is_adapter_v<std::decay_t<Collection>>) {
        if constexpr(adapt::has_ioc_push<adapter_underlying_type_t<Collection>, TagType>) {
          if (c.underlying().can_push(tag_)) {
            std::move(c).underlying().ioc_push(
                tag_,
                std::move(dst));
            return objpipe_errc::success;
          }
        }
      }

      tag_.post(
          make_task(
              [](Sink&& dst, Collection&& c) {
                try {
                  auto b = make_move_iterator(flatten_op_begin_(c));
                  auto e = make_move_iterator(flatten_op_end_(c));

                  objpipe_errc error_code = objpipe_errc::success;
                  while (b != e && error_code == objpipe_errc::success) {
                    error_code = dst(*b);
                    ++b;
                  }
                } catch (...) {
                  dst.push_exception(std::current_exception());
                }
              },
              std::move(dst),
              std::move(c)));
      return objpipe_errc::success;
    }
  }

  auto operator()(const Collection& c)
  -> objpipe_errc {
    auto b = flatten_op_begin_(c);
    auto e = flatten_op_end_(c);
    objpipe_errc error_code = objpipe_errc::success;

    while (b != e && error_code == objpipe_errc::success) {
      error_code = sink_(*b);
      ++b;
    }
    return error_code;
  }

  auto push_exception(std::exception_ptr exptr)
  noexcept
  -> void {
    sink_.push_exception(std::move(exptr));
  }

 private:
  Sink sink_;
  TagType tag_;
};


template<typename Collection>
class flatten_op_store {
 private:
  using collection = Collection;
  using begin_iterator =
      std::decay_t<decltype(make_move_iterator(flatten_op_begin_(std::declval<collection&>())))>;
  using end_iterator =
      std::decay_t<decltype(make_move_iterator(flatten_op_end_(std::declval<collection&>())))>;

  static_assert(
      std::is_base_of_v<std::input_iterator_tag, typename std::iterator_traits<begin_iterator>::iterator_category>,
      "Collection iterator must be an input iterator");

  using data_type = std::conditional_t<
      std::is_reference_v<collection>,
      std::tuple<begin_iterator, end_iterator>,
      std::tuple<begin_iterator, end_iterator, collection>>;

 public:
  ///\brief Enable pull only when its safe to do so.
  ///\details
  ///We can enable pull, if deref() returns by value.
  ///
  ///Or we can enable pull, if deref returns a reference and the iterator is
  ///at least a forward iterator.
  ///This because ``*iterator++`` is a valid expression for forward iterators.
  ///
  ///Otherwise, we conservatively disable pull, in the event that input
  ///iterator advancement invalidates the returned value.
  static constexpr bool enable_pull =
      !std::is_reference_v<typename std::iterator_traits<begin_iterator>::value_type>
      || std::is_base_of_v<std::forward_iterator_tag, typename std::iterator_traits<begin_iterator>::value_type>;

  template<bool OmitCollection = std::is_reference_v<collection>>
  flatten_op_store(std::enable_if_t<OmitCollection, collection> c)
  noexcept(noexcept(begin_iterator(make_move_iterator(flatten_op_begin_(std::declval<std::remove_reference_t<collection>&>()))))
      && noexcept(end_iterator(make_move_iterator(flatten_op_end_(std::declval<std::remove_reference_t<collection>&>())))))
  : data_(make_move_iterator(flatten_op_begin_(c)), make_move_iterator(flatten_op_end_(c)))
  {}

  template<bool OmitCollection = std::is_reference_v<collection>>
  flatten_op_store(std::enable_if_t<!OmitCollection, collection>&& c)
  noexcept(std::is_nothrow_move_constructible_v<collection>
      && noexcept(begin_iterator(make_move_iterator(flatten_op_begin_(std::declval<std::remove_reference_t<collection>&>()))))
      && noexcept(end_iterator(make_move_iterator(flatten_op_end_(std::declval<std::remove_reference_t<collection>&>())))))
  : data_(make_move_iterator(flatten_op_begin_(c)), make_move_iterator(flatten_op_end_(c)), std::move(c))
  {}

  constexpr auto empty() const
  noexcept(noexcept(std::declval<const begin_iterator&>() == std::declval<const end_iterator&>()))
  -> bool {
    return std::get<0>(data_) == std::get<1>(data_);
  }

  auto deref() const
  noexcept(noexcept(*std::declval<const begin_iterator&>()))
  -> decltype(*std::declval<const begin_iterator&>()) {
    assert(std::get<0>(data_) != std::get<1>(data_));
    return *std::get<0>(data_);
  }

  auto advance()
  noexcept(noexcept(++std::declval<begin_iterator&>()))
  -> void {
    assert(std::get<0>(data_) != std::get<1>(data_));
    ++std::get<0>(data_);
  }

 private:
  data_type data_;
};


/**
 * \brief Implements the flatten operation, that iterates over each element of a collection value.
 * \implements TransformationConcept
 * \implements IocPushConcept
 * \ingroup objpipe_detail
 *
 * \details
 * Replaces each collection element in the nested objpipe by the sequence of its elements.
 *
 * Requires that std::begin() and std::end() are valid for the given collection type.
 *
 * \tparam Source The nested source.
 * \sa \ref objpipe::detail::adapter::flatten
 */
template<typename Source>
class flatten_op {
 private:
  using raw_collection_type = adapt::front_type<Source>;
  using store_type = flatten_op_store<raw_collection_type>;
  using item_type = decltype(std::declval<const store_type&>().deref());

  static constexpr bool ensure_avail_noexcept =
      noexcept(std::declval<Source&>().front())
      && noexcept(std::declval<Source&>().pop_front())
      && noexcept(std::declval<store_type>().empty())
      && std::is_nothrow_constructible_v<store_type, raw_collection_type>
      && std::is_nothrow_destructible_v<store_type>
      && (std::is_lvalue_reference_v<raw_collection_type>
          || std::is_rvalue_reference_v<raw_collection_type>
          || std::is_nothrow_move_constructible_v<raw_collection_type>);

 public:
  constexpr flatten_op(Source&& src)
  noexcept(std::is_nothrow_move_constructible_v<Source>)
  : src_(std::move(src)),
    active_()
  {}

  flatten_op(const flatten_op&) = delete;
  constexpr flatten_op(flatten_op&&) = default;
  flatten_op& operator=(const flatten_op&) = delete;
  flatten_op& operator=(flatten_op&&) = delete;

  auto is_pullable()
  noexcept(noexcept(std::declval<Source&>().is_pullable())
      && ensure_avail_noexcept)
  -> bool {
    return (active_.has_value() && !active_->empty()) || src_.is_pullable();
  }

  auto wait()
  noexcept(ensure_avail_noexcept)
  -> objpipe_errc {
    return ensure_avail_();
  }

  auto front()
  noexcept(ensure_avail_noexcept
      && noexcept(std::declval<store_type&>().deref())
      && (std::is_lvalue_reference_v<item_type>
          || std::is_rvalue_reference_v<item_type>
          || std::is_nothrow_move_constructible_v<item_type>))
  -> transport<item_type> {
    const objpipe_errc e = ensure_avail_();
    if (e == objpipe_errc::success) {
      pending_pop_ = true;
      return transport<item_type>(std::in_place_index<0>, active_->deref());
    }
    return transport<item_type>(std::in_place_index<1>, e);
  }

  auto pop_front()
  noexcept(ensure_avail_noexcept
      && noexcept(std::declval<store_type&>().advance()))
  -> objpipe_errc {
    if (!std::exchange(pending_pop_, false)) {
      objpipe_errc e = ensure_avail_();
      if (e != objpipe_errc::success) return e;
    }

    active_->advance();
    return objpipe_errc::success;
  }

  template<bool Enable = store_type::enable_pull>
  auto pull()
  noexcept(ensure_avail_noexcept
      && noexcept(std::declval<store_type&>().deref())
      && (std::is_lvalue_reference_v<item_type>
          || std::is_rvalue_reference_v<item_type>
          || std::is_nothrow_move_constructible_v<item_type>)
      && noexcept(std::declval<store_type&>().advance()))
  -> std::enable_if_t<Enable, transport<item_type>> {
    assert(!pending_pop_);
    const objpipe_errc e = ensure_avail_();
    if (e == objpipe_errc::success) {
      auto result = transport<item_type>(std::in_place_index<0>, active_->deref());
      active_->advance();
      return result;
    }
    return transport<item_type>(std::in_place_index<1>, e);
  }

  template<typename PushTag>
  constexpr auto can_push(PushTag tag) const
  noexcept
  -> std::enable_if_t<adapt::has_ioc_push<Source, PushTag>
      || std::is_base_of_v<multithread_push, PushTag>,
      bool> {
    if constexpr(std::is_base_of_v<multithread_push, PushTag>)
      return true; // We have a specialization to shard out.
    else
      return src_.can_push(tag);
  }

  template<typename PushTag, typename Acceptor>
  auto ioc_push(PushTag tag, Acceptor&& acceptor) &&
  -> std::enable_if_t<adapt::has_ioc_push<Source, PushTag>
      || std::is_base_of_v<multithread_push, PushTag>> {
    using wrapper = flatten_push<
        std::remove_cv_t<std::remove_reference_t<raw_collection_type>>,
        std::decay_t<Acceptor>,
        std::decay_t<PushTag>>;

    assert(!active_.has_value());

    // adapt::ioc_push will provide a fallback if multithread_push is unavailable.
    adapt::ioc_push(
        std::move(src_),
        tag,
        wrapper(std::forward<Acceptor>(acceptor), tag));
  }

 private:
  auto ensure_avail_()
  noexcept(ensure_avail_noexcept)
  -> objpipe_errc {
    assert(!pending_pop_);

    while (!active_.has_value() || active_->empty()) {
      if (active_.has_value()) {
        assert(active_->empty());
        objpipe_errc e = src_.pop_front();
        if (e != objpipe_errc::success)
          return e;
      }

      transport<raw_collection_type> front_val = src_.front();
      if (!front_val.has_value()) {
        assert(front_val.errc() != objpipe_errc::success);
        return front_val.errc();
      }
      active_.emplace(std::move(front_val).value());
    }
    return objpipe_errc::success;
  }

  Source src_;
  std::optional<store_type> active_;
  bool pending_pop_ = false;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_FLATTEN_OP_H */
