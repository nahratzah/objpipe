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


} /* namespace objpipe::detail */

#endif /* OBJPIPE_DETAIL_FWD_H */
