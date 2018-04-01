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
      auto b = std::make_move_iterator(flatten_op_begin_(c));
      auto e = std::make_move_iterator(flatten_op_end_(c));
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
                  auto b = std::make_move_iterator(flatten_op_begin_(c));
                  auto e = std::make_move_iterator(flatten_op_end_(c));

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


template<typename Collection, bool IsReference = std::is_reference_v<Collection>>
class flatten_op_store_data {
  static_assert(std::is_reference_v<Collection>);

 private:
  using begin_iterator =
      std::decay_t<decltype(flatten_op_begin_(std::declval<std::remove_reference_t<Collection>&>()))>;
  using end_iterator =
      std::decay_t<decltype(flatten_op_end_(std::declval<std::remove_reference_t<Collection>&>()))>;

  static_assert(
      !(std::is_const_v<std::remove_reference_t<typename std::iterator_traits<begin_iterator>::reference>>
          && std::is_rvalue_reference_v<typename std::iterator_traits<begin_iterator>::reference>),
      "Iterator reference may not be an const-qualified rvalue-reference.");

  // If iterator yields non-const lvalue-references, convert them to rvalue-references.
  static constexpr bool convert_to_move_iterator =
      !std::is_const_v<std::remove_reference_t<typename std::iterator_traits<begin_iterator>::reference>>;

  static_assert(
      std::is_base_of_v<std::input_iterator_tag, typename std::iterator_traits<begin_iterator>::iterator_category>,
      "Collection iterator must be an input iterator");

 public:
  using iterator = std::conditional_t<
      convert_to_move_iterator,
      std::move_iterator<begin_iterator>,
      begin_iterator>;

  flatten_op_store_data(Collection c)
  : begin_(flatten_op_begin_(c)),
    end_(flatten_op_end_(c))
  {}

  flatten_op_store_data(flatten_op_store_data&&) = default;

  auto empty() const
  noexcept(noexcept(std::declval<const begin_iterator&>() == std::declval<const end_iterator&>()))
  -> bool {
    if constexpr(convert_to_move_iterator)
      return begin_.base() == end_;
    else
      return begin_ == end_;
  }

  auto iter()
  -> iterator& {
    return begin_;
  }

 private:
  iterator begin_;
  end_iterator end_;
};

template<typename Collection>
class flatten_op_store_data<Collection, false> {
 private:
  using impl_type = flatten_op_store_data<Collection&&, true>;

 public:
  using iterator = typename impl_type::iterator;

  flatten_op_store_data(Collection&& c)
  : c_(std::make_unique<Collection>(std::move(c))),
    impl_(std::move(*this->c_))
  {}

  flatten_op_store_data(flatten_op_store_data&&) = default;

  auto empty() const
  noexcept(noexcept(impl_.empty()))
  -> bool {
    return impl_.empty();
  }

  auto iter()
  -> iterator& {
    return impl_.iter();
  }

 private:
  ///\brief Pointer to collection.
  ///\details
  ///Store collection using a pointer, so we don't try to move it
  ///when we are moved.
  ///The logic being that moving a collection may cause iterator invalidation
  ///(example: std::array).
  std::unique_ptr<Collection> c_;

  impl_type impl_;
};

template<typename Collection, typename = void>
class flatten_op_store {
 private:
  using collection = Collection;
  using data_type = flatten_op_store_data<collection>;
  using iter_ref_t = typename std::iterator_traits<typename data_type::iterator>::reference;

 public:
  using transport_type = transport<iter_ref_t>;

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
      !std::is_reference_v<typename std::iterator_traits<typename data_type::iterator>::value_type>
      || std::is_base_of_v<std::forward_iterator_tag, typename std::iterator_traits<typename data_type::iterator>::iterator_category>;

  flatten_op_store(std::add_rvalue_reference_t<collection> c)
  noexcept(std::is_nothrow_constructible_v<data_type, std::add_rvalue_reference_t<collection>>)
  : data_(std::forward<collection>(c))
  {}

  auto wait() const
  noexcept(noexcept(std::declval<const data_type&>().empty()))
  -> objpipe_errc {
    return (data_.empty() ? objpipe_errc::closed : objpipe_errc::success);
  }

  auto front()
  noexcept(noexcept(std::declval<const data_type&>().empty())
      && noexcept(transport_type(std::in_place_index<0>, *std::declval<typename data_type::iterator&>())))
  -> transport_type {
    if (data_.empty())
      return transport_type(std::in_place_index<1>, objpipe_errc::closed);
    return transport_type(std::in_place_index<0>, *data_.iter());
  }

  auto pop_front()
  noexcept(noexcept(std::declval<const data_type&>().empty())
      && noexcept(++std::declval<typename data_type::iterator&>()))
  -> objpipe_errc {
    if (data_.empty())
      return objpipe_errc::closed;
    ++data_.iter();
    return objpipe_errc::success;
  }

  template<bool Enable = enable_pull>
  auto pull()
  noexcept(noexcept(std::declval<const data_type&>().empty())
      && noexcept(transport_type(std::in_place_index<0>, *std::declval<typename data_type::iterator&>()))
      && noexcept(++std::declval<typename data_type::iterator&>()))
  -> transport_type {
    if (data_.empty())
      return transport_type(std::in_place_index<1>, objpipe_errc::closed);

    auto tx = transport_type(std::in_place_index<0>, *data_.iter());
    ++data_.iter();
    return tx;
  }

 private:
  data_type data_;
};

template<typename Source>
class flatten_op_store<Source, std::enable_if_t<is_adapter_v<std::decay_t<Source>>>> {
 public:
  using underlying_source = adapter_underlying_type_t<std::decay_t<Source>>;
  using transport_type = transport<adapt::front_type<underlying_source>>;

  static constexpr auto enable_pull = adapt::has_pull<underlying_source>;

  flatten_op_store(Source src)
  noexcept(std::is_nothrow_constructible_v<std::decay_t<Source>, Source>)
  : src_(std::forward<Source>(src))
  {}

  auto wait()
  noexcept(noexcept(std::declval<std::decay_t<Source>&>().underlying().wait()))
  -> objpipe_errc {
    return src_.underlying().wait();
  }

  auto front()
  noexcept(noexcept(std::declval<std::decay_t<Source>&>().underlying().front()))
  -> auto {
    return src_.underlying().front();
  }

  auto pop_front()
  noexcept(noexcept(std::declval<std::decay_t<Source>&>().underlying().pop_front()))
  -> auto {
    return src_.underlying().pop_front();
  }

  template<bool Enable = enable_pull>
  auto pull()
  noexcept(noexcept(std::declval<underlying_source&>().pull()))
  -> transport<std::enable_if_t<Enable, adapt::pull_type<underlying_source>>> {
    return src_.underlying().pull();
  }

 private:
  Source src_; // This may be a reference.
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
  using transport_type = typename store_type::transport_type;

  static constexpr bool ensure_avail_noexcept =
      noexcept(std::declval<Source&>().front())
      && std::is_nothrow_constructible_v<store_type, raw_collection_type>;

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
    return (active_.has_value() || src_.is_pullable());
  }

  auto wait()
  noexcept(ensure_avail_noexcept)
  -> objpipe_errc {
    objpipe_errc e;
    for (e = ensure_avail_();
        e == objpipe_errc::success;
        e = ensure_avail_()) {
      e = active_->wait();
      if (e == objpipe_errc::success) return e;
      if (e == objpipe_errc::closed)
        e = consume_active_();
      if (e != objpipe_errc::success)
        return e;
    }

    return e;
  }

  auto front()
  noexcept(noexcept(std::declval<flatten_op&>().ensure_avail_())
      && noexcept(std::declval<flatten_op&>().consume_active_())
      && noexcept(std::declval<store_type&>().front())
      && std::is_nothrow_move_constructible_v<transport_type>)
  -> transport_type {
    objpipe_errc e;
    for (e = ensure_avail_();
        e == objpipe_errc::success;
        e = ensure_avail_()) {
      transport_type result = active_->front();
      if (result.errc() == objpipe_errc::closed) {
        e = consume_active_();
        if (e != objpipe_errc::success)
          return transport_type(std::in_place_index<1>, e);
      } else
        return result;
    }

    return transport_type(std::in_place_index<1>, e);
  }

  auto pop_front()
  noexcept(noexcept(std::declval<flatten_op&>().ensure_avail_())
      && noexcept(std::declval<flatten_op&>().consume_active_())
      && noexcept(std::declval<store_type&>().pop_front()))
  -> objpipe_errc {
    objpipe_errc e;
    for (e = ensure_avail_();
        e == objpipe_errc::success;
        e = ensure_avail_()) {
      e = active_->pop_front();
      if (e == objpipe_errc::closed) {
        e = consume_active_();
        if (e != objpipe_errc::success) return e;
      } else {
        return e;
      }
    }

    return e;
  }

  template<bool Enable = store_type::enable_pull>
  auto pull()
  noexcept(noexcept(std::declval<flatten_op&>().ensure_avail_())
      && noexcept(std::declval<flatten_op&>().consume_active_())
      && noexcept(std::declval<store_type&>().front())
      && noexcept(std::declval<store_type&>().pop_front())
      && std::is_nothrow_move_constructible_v<transport_type>)
  -> decltype(std::declval<std::enable_if_t<Enable, store_type>&>().pull()) {
    objpipe_errc e;
    for (e = ensure_avail_();
        e == objpipe_errc::success;
        e = ensure_avail_()) {
      auto result = active_->pull();
      if (result.errc() == objpipe_errc::closed) {
        e = consume_active_();
        if (e != objpipe_errc::success) break;
      } else {
        return result;
      }
    }

    return transport_type(std::in_place_index<1>, e);
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
    if (!active_.has_value()) {
      transport<raw_collection_type> front_val = src_.front();
      if (!front_val.has_value()) {
        assert(front_val.errc() != objpipe_errc::success);
        return front_val.errc();
      }
      active_.emplace(std::move(front_val).value());
    }
    return objpipe_errc::success;
  }

  auto consume_active_()
  noexcept(std::is_nothrow_destructible_v<store_type>
      && noexcept(std::declval<Source&>().pop_front()))
  -> objpipe_errc {
    active_.reset();
    return src_.pop_front();
  }

  Source src_;
  std::optional<store_type> active_;
};


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_FLATTEN_OP_H */
