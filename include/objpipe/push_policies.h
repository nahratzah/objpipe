#ifndef OBJPIPE_PUSH_POLICIES_H
#define OBJPIPE_PUSH_POLICIES_H

///\file
///\ingroup objpipe
///\brief Policy types for push operations.

namespace objpipe {


///\brief Tag indicating IOC should use an existing thread.
///\ingroup objpipe
///\details
///If the objpipe source has a thread that emits values, that thread
///will be instructed to perform the push operations.
///
///Otherwise, a lazy future will be constructed to generate the result.
struct existingthread_push {};

///\brief Tag indicating IOC should be single threaded.
///\ingroup objpipe
struct singlethread_push
: existingthread_push
{};

///\brief Tag indicating multi threaded IOC should partition its data, preserving ordering.
///\ingroup objpipe
///\details
///Partitions are subsets of ordered iteration.
///\note
///Inherits from singlethread_push, since single threaded pushing fulfills
///the multithread constraint.
struct multithread_push
: public singlethread_push
{};

///\brief Tag indicating multi threaded IOC doesn't have to maintain an ordering constraint.
///\ingroup objpipe
///\details
///The source can supply elements in any order.
///\note Inherits from multithread_push, since (ordered) partitions
///fulfill the unordered constraint.
struct multithread_unordered_push
: public multithread_push
{};


} /* namespace objpipe */

#endif /* OBJPIPE_PUSH_POLICIES_H */
