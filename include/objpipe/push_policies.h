#ifndef OBJPIPE_PUSH_POLICIES_H
#define OBJPIPE_PUSH_POLICIES_H

///\file
///\ingroup objpipe
///\brief Policy types for push operations.

#include <functional>
#include <objpipe/detail/thread_pool.h>
#include <objpipe/detail/task.h>

namespace objpipe {


///\brief Tag indicating IOC should use an existing thread.
///\ingroup objpipe
///\details
///If the objpipe source has a thread that emits values, that thread
///will be instructed to perform the push operations.
///
///Otherwise, a lazy future will be constructed to generate the result.
class existingthread_push {};

///\brief Tag indicating IOC should be single threaded.
///\ingroup objpipe
class singlethread_push
: public existingthread_push {
 public:
  ///\brief Task functions used by the policy.
  ///\details These tasks, when posted, must be invoked exactly once.
  using task_function_type = std::function<void()>;

  singlethread_push()
  : singlethread_push(&singlethread_push::default_post_impl)
  {}

  explicit singlethread_push(std::function<void(task_function_type)> post_impl)
  : post_impl_(std::move(post_impl))
  {
    if (post_impl_ == nullptr)
      throw std::invalid_argument("nullptr post_impl");
  }

  ///\brief Start a new task.
  ///\details The task shall be run exactly once.
  auto post(task_function_type fn) const -> void {
    post_impl_(std::move(fn));
  }

  ///\brief Start a new task.
  ///\details The task shall be run exactly once.
  template<typename Fn, typename... Args>
  auto post(detail::task<Fn, Args...>&& fn) const -> void {
    post(std::move(fn).as_function());
  }

 private:
  static void default_post_impl(task_function_type task) {
    detail::thread_pool::default_pool().publish(std::move(task));
  }

  std::function<void(task_function_type)> post_impl_;
};

///\brief Tag indicating multi threaded IOC should partition its data, preserving ordering.
///\ingroup objpipe
///\details
///Partitions are subsets of ordered iteration.
///\note
///Inherits from singlethread_push, since single threaded pushing fulfills
///the multithread constraint.
class multithread_push
: public singlethread_push
{
 public:
  multithread_push() = default;

  explicit multithread_push(std::function<void(task_function_type)> post_impl)
  : singlethread_push(std::move(post_impl))
  {}
};

///\brief Tag indicating multi threaded IOC doesn't have to maintain an ordering constraint.
///\ingroup objpipe
///\details
///The source can supply elements in any order.
///\note Inherits from multithread_push, since (ordered) partitions
///fulfill the unordered constraint.
class multithread_unordered_push
: public multithread_push
{
 public:
  multithread_unordered_push() = default;

  explicit multithread_unordered_push(std::function<void(task_function_type)> post_impl)
  : multithread_push(std::move(post_impl))
  {}
};


} /* namespace objpipe */

#endif /* OBJPIPE_PUSH_POLICIES_H */
