#include <objpipe/callback.h>
#include <objpipe/array.h>
#include <objpipe/interlock.h>
#include <objpipe/of.h>
#include <objpipe/errc.h>
#include "UnitTest++/UnitTest++.h"
#include "test_hacks.ii"
#include <vector>
#include <tuple>
#include <thread>
#include <functional>
#include <list>

using objpipe::objpipe_errc;
using objpipe::existingthread_push;
using objpipe::singlethread_push;
using objpipe::multithread_push;
using objpipe::multithread_unordered_push;

TEST(iterate_empty) {
  CHECK_EQUAL(
      std::vector<int>(),
      objpipe::of<int>()
          .to_vector());
}

TEST(push_empty) {
  CHECK_EQUAL(
      std::vector<int>(),
      objpipe::of<int>()
          .async()
          .to_vector()
          .get());

  CHECK_EQUAL(
      std::vector<int>(),
      objpipe::of<int>()
          .async(existingthread_push())
          .to_vector()
          .get());

  CHECK_EQUAL(
      std::vector<int>(),
      objpipe::of<int>()
          .async(singlethread_push())
          .to_vector()
          .get());

  CHECK_EQUAL(
      std::vector<int>(),
      objpipe::of<int>()
          .async(multithread_push())
          .to_vector()
          .get());

  CHECK_EQUAL(
      std::vector<int>(),
      objpipe::of<int>()
          .async(multithread_unordered_push())
          .to_vector()
          .get());
}

TEST(callback) {
  auto reader = objpipe::new_callback<int>(
      [](auto& cb) {
        for (int i = 0; i < 5; ++i)
          cb(i);
      });

  CHECK_EQUAL(false, reader.empty());

  // Element 0 access, using front() and pop_front().
  CHECK_EQUAL(0, reader.front());
  reader.pop_front();

  // Element 1 access, using front() and pull().
  CHECK_EQUAL(1, reader.front());
  CHECK_EQUAL(1, reader.pull());

  // Element 2 access, using pull().
  CHECK_EQUAL(2, reader.pull());

  // Element 3 access, using try_pull().
  CHECK_EQUAL(3, reader.try_pull());

  // Element 4 access, using wait(), then pull().
  CHECK_EQUAL(objpipe_errc::success, reader.wait());
  CHECK_EQUAL(4, reader.pull());

  // No more elements.
  CHECK_EQUAL(false, reader.is_pullable());
  CHECK_EQUAL(true, reader.empty());
  CHECK_EQUAL(objpipe_errc::closed, reader.wait());

  objpipe_errc e;
  std::optional<int> failed_pull = reader.pull(e);
  CHECK_EQUAL(false, failed_pull.has_value());
  CHECK_EQUAL(objpipe_errc::closed, e);
}

TEST(callback_empty_without_pulling) {
  auto reader = objpipe::new_callback<int>(
      [](auto& cb) {
        for (int i = 0; i < 2; ++i)
          cb(i);
      });

  CHECK(!reader.empty());
  CHECK(!reader.empty()); // Check that result is stable.
  CHECK_EQUAL(0, reader.pull());

  CHECK(!reader.empty());
  CHECK(!reader.empty()); // Check that result is stable.
  CHECK_EQUAL(1, reader.pull());

  CHECK(reader.empty());
  CHECK(reader.empty()); // Check that result is stable.
}

TEST(array_using_iterators) {
  std::vector<int> il = { 0, 1, 2, 3, 4 };
  auto reader = objpipe::new_array(il.begin(), il.end());

  CHECK_EQUAL(false, reader.empty());

  // Element 0 access, using front() and pop_front().
  CHECK_EQUAL(0, reader.front());
  reader.pop_front();

  // Element 1 access, using front() and pull().
  CHECK_EQUAL(1, reader.front());
  CHECK_EQUAL(1, reader.pull());

  // Element 2 access, using pull().
  CHECK_EQUAL(2, reader.pull());

  // Element 3 access, using try_pull().
  CHECK_EQUAL(3, reader.try_pull());

  // Element 4 access, using wait(), then pull().
  CHECK_EQUAL(objpipe_errc::success, reader.wait());
  CHECK_EQUAL(4, reader.pull());

  // No more elements.
  CHECK_EQUAL(false, reader.is_pullable());
  CHECK_EQUAL(true, reader.empty());
  CHECK_EQUAL(objpipe_errc::closed, reader.wait());

  objpipe_errc e;
  std::optional<int> failed_pull = reader.pull(e);
  CHECK_EQUAL(false, failed_pull.has_value());
  CHECK_EQUAL(objpipe_errc::closed, e);
}

TEST(array_push) {
  constexpr int COUNT = 1 * 1000 * 1000;
  std::vector<int> il;
  for (int i = 0; i < COUNT; ++i)
    il.push_back(i);

  auto result = objpipe::new_array(il.begin(), il.end())
      .async(multithread_push())
      .to_vector()
      .get();

  CHECK_EQUAL(il, result);
}

TEST(array_using_initializer_list) {
  auto reader = objpipe::new_array({ 0, 1, 2, 3, 4 });

  CHECK_EQUAL(false, reader.empty());

  // Element 0 access, using front() and pop_front().
  CHECK_EQUAL(0, reader.front());
  reader.pop_front();

  // Element 1 access, using front() and pull().
  CHECK_EQUAL(1, reader.front());
  CHECK_EQUAL(1, reader.pull());

  // Element 2 access, using pull().
  CHECK_EQUAL(2, reader.pull());

  // Element 3 access, using try_pull().
  CHECK_EQUAL(3, reader.try_pull());

  // Element 4 access, using wait(), then pull().
  CHECK_EQUAL(objpipe_errc::success, reader.wait());
  CHECK_EQUAL(4, reader.pull());

  // No more elements.
  CHECK_EQUAL(false, reader.is_pullable());
  CHECK_EQUAL(true, reader.empty());
  CHECK_EQUAL(objpipe_errc::closed, reader.wait());

  objpipe_errc e;
  std::optional<int> failed_pull = reader.pull(e);
  CHECK_EQUAL(false, failed_pull.has_value());
  CHECK_EQUAL(objpipe_errc::closed, e);
}

TEST(filter_operation) {
  auto reader = objpipe::new_array({ 0, 1, 2, 3, 4 })
      .filter([](int x) { return x % 2 == 0; });

  CHECK_EQUAL(false, reader.empty());

  // Element 0 access, using front() and pop_front().
  CHECK_EQUAL(0, reader.front());
  reader.pop_front();

  // Element 1 is filtered out

  // Element 2 access, using pull().
  CHECK_EQUAL(2, reader.pull());

  // Element 3 is filtered out

  // Element 4 access, using wait(), then pull().
  CHECK_EQUAL(objpipe_errc::success, reader.wait());
  CHECK_EQUAL(4, reader.pull());

  // No more elements.
  CHECK_EQUAL(false, reader.is_pullable());
  CHECK_EQUAL(true, reader.empty());
  CHECK_EQUAL(objpipe_errc::closed, reader.wait());

  objpipe_errc e;
  std::optional<int> failed_pull = reader.pull(e);
  CHECK_EQUAL(false, failed_pull.has_value());
  CHECK_EQUAL(objpipe_errc::closed, e);
}

TEST(transform_operation) {
  auto reader = objpipe::new_array({ 0, 1, 2, 3, 4 })
      .transform([](int x) { return 2 * x; });

  CHECK_EQUAL(false, reader.empty());

  // Element 0 access, using front() and pop_front().
  CHECK_EQUAL(0, reader.front());
  reader.pop_front();

  // Element 1 access, using front() and pull().
  CHECK_EQUAL(2, reader.front());
  CHECK_EQUAL(2, reader.pull());

  // Element 2 access, using pull().
  CHECK_EQUAL(4, reader.pull());

  // Element 3 access, using try_pull().
  CHECK_EQUAL(6, reader.try_pull());

  // Element 4 access, using wait(), then pull().
  CHECK_EQUAL(objpipe_errc::success, reader.wait());
  CHECK_EQUAL(8, reader.pull());

  // No more elements.
  CHECK_EQUAL(false, reader.is_pullable());
  CHECK_EQUAL(true, reader.empty());
  CHECK_EQUAL(objpipe_errc::closed, reader.wait());

  objpipe_errc e;
  std::optional<int> failed_pull = reader.pull(e);
  CHECK_EQUAL(false, failed_pull.has_value());
  CHECK_EQUAL(objpipe_errc::closed, e);
}

TEST(interlock) {
  objpipe::interlock_reader<int> reader;
  objpipe::interlock_writer<int> writer;
  std::tie(reader, writer) = objpipe::new_interlock<int>();

  auto th = std::thread(std::bind(
          [](objpipe::interlock_writer<int>& writer) {
            for (int i = 0; i < 5; ++i)
              writer(i);
          },
          std::move(writer)));

  CHECK_EQUAL(objpipe_errc::success, reader.wait());
  CHECK_EQUAL(false, reader.empty());

  // Element 0 access, using front() and pop_front().
  CHECK_EQUAL(0, reader.front());
  reader.pop_front();

  // Element 1 access, using front() and pull().
  CHECK_EQUAL(1, reader.front());
  CHECK_EQUAL(1, reader.pull());

  // Element 2 access, using pull().
  CHECK_EQUAL(2, reader.pull());

  // Element 3 access, using try_pull().
  // Note: try_pull may yield empty optional, if the writer thread is not fast
  // enough. So we spin on that.
  std::optional<int> three = reader.try_pull();
  while (!three.has_value()) three = reader.try_pull();
  CHECK_EQUAL(3, three);

  // Element 4 access, using wait(), then pull().
  CHECK_EQUAL(objpipe_errc::success, reader.wait());
  CHECK_EQUAL(4, reader.pull());

  // No more elements.
  CHECK_EQUAL(true, reader.empty());
  CHECK_EQUAL(objpipe_errc::closed, reader.wait());
  CHECK_EQUAL(false, reader.is_pullable());

  objpipe_errc e;
  std::optional<int> failed_pull = reader.pull(e);
  CHECK_EQUAL(false, failed_pull.has_value());
  CHECK_EQUAL(objpipe_errc::closed, e);

  th.join();
}

TEST(interlock_push) {
  constexpr int COUNT = 1 * 1000 * 1000;
  objpipe::interlock_reader<std::pair<int, int>> reader;
  objpipe::interlock_writer<std::pair<int, int>> writer;
  std::tie(reader, writer) = objpipe::new_interlock<std::pair<int, int>>();

  std::vector<int> expect;
  for (int i = 0; i < COUNT; ++i)
    expect.push_back(i);

  auto th1 = std::thread(std::bind(
          [](objpipe::interlock_writer<std::pair<int, int>>& writer) {
            for (int i = 0; i < COUNT; ++i)
              writer(std::make_pair(1, i));
          },
          writer));
  auto th2 = std::thread(std::bind(
          [](objpipe::interlock_writer<std::pair<int, int>>& writer) {
            for (int i = 0; i < COUNT; ++i)
              writer(std::make_pair(2, i));
          },
          std::move(writer)));

  // Method under test.
  const std::vector<std::pair<int, int>> result = std::move(reader)
      .async(singlethread_push([](auto fn) { CHECK(!"Must use existing thread, not spawn a new one."); }))
      .to_vector()
      .get();

  // Split result into components, as ordering is not guaranteed.
  std::vector<int> th1_result;
  std::vector<int> th2_result;
  std::for_each(result.begin(), result.end(),
      [&th1_result, &th2_result](const std::pair<int, int> pair) {
        CHECK(pair.first == 1 || pair.first == 2);
        if (pair.first == 1)
          th1_result.push_back(pair.second);
        else
          th2_result.push_back(pair.second);
      });

  CHECK_EQUAL(expect, th1_result);
  CHECK_EQUAL(expect, th2_result);

  th1.join();
  th2.join();
}

TEST(interlock_push_unordered) {
  constexpr int COUNT = 1 * 1000 * 1000;
  objpipe::interlock_reader<std::pair<int, int>> reader;
  objpipe::interlock_writer<std::pair<int, int>> writer;
  std::tie(reader, writer) = objpipe::new_interlock<std::pair<int, int>>();

  std::vector<int> expect;
  for (int i = 0; i < COUNT; ++i)
    expect.push_back(i);

  auto th1 = std::thread(std::bind(
          [](objpipe::interlock_writer<std::pair<int, int>>& writer) {
            for (int i = 0; i < COUNT; ++i)
              writer(std::make_pair(1, i));
          },
          writer));
  auto th2 = std::thread(std::bind(
          [](objpipe::interlock_writer<std::pair<int, int>>& writer) {
            for (int i = 0; i < COUNT; ++i)
              writer(std::make_pair(2, i));
          },
          std::move(writer)));

  // Method under test.
  const std::vector<std::pair<int, int>> result = std::move(reader)
      .async(multithread_unordered_push([](auto fn) { CHECK(!"Must use existing thread, not spawn a new one."); }))
      .to_vector()
      .get();

  // Split result into components, as ordering is not guaranteed.
  std::vector<int> th1_result;
  std::vector<int> th2_result;
  std::for_each(result.begin(), result.end(),
      [&th1_result, &th2_result](const std::pair<int, int> pair) {
        CHECK(pair.first == 1 || pair.first == 2);
        if (pair.first == 1)
          th1_result.push_back(pair.second);
        else
          th2_result.push_back(pair.second);
      });

  // Unordered means the reductions may happen in an unordered fashion,
  // thus we can not even guarantee that even the elements in a
  // single thread are reduced in ordered fashion.
  //
  // All we can check is that everything is present.
  std::sort(th1_result.begin(), th1_result.end());
  std::sort(th2_result.begin(), th2_result.end());

  CHECK_EQUAL(expect, th1_result);
  CHECK_EQUAL(expect, th2_result);

  th1.join();
  th2.join();
}

TEST(iteration_push) {
  constexpr int COUNT = 100 * 1000;
  std::vector<int> expect;
  for (int i = 0; i < COUNT; ++i)
    expect.push_back(i);

  std::vector<std::vector<int>> input;
  for (int i = 0; i < COUNT; i += 1000) {
    input.emplace_back();
    for (int j = i; j < i + 1000 && j < COUNT; ++j)
      input.back().push_back(j);
  }

  auto result = objpipe::of(std::move(input))
      .iterate()
      .iterate()
      .async()
      .to_vector()
      .get();

  CHECK_EQUAL(expect, result);
}

TEST(iteration_existingthread_push) {
  constexpr int COUNT = 100 * 1000;
  std::vector<int> expect;
  for (int i = 0; i < COUNT; ++i)
    expect.push_back(i);

  std::vector<std::vector<int>> input;
  for (int i = 0; i < COUNT; i += 1000) {
    input.emplace_back();
    for (int j = i; j < i + 1000 && j < COUNT; ++j)
      input.back().push_back(j);
  }

  auto result = objpipe::of(std::move(input))
      .iterate()
      .iterate()
      .async(existingthread_push())
      .to_vector()
      .get();

  CHECK_EQUAL(expect, result);
}

TEST(iteration_singlethread_push) {
  constexpr int COUNT = 100 * 1000;
  std::vector<int> expect;
  for (int i = 0; i < COUNT; ++i)
    expect.push_back(i);

  std::vector<std::vector<int>> input;
  for (int i = 0; i < COUNT; i += 1000) {
    input.emplace_back();
    for (int j = i; j < i + 1000 && j < COUNT; ++j)
      input.back().push_back(j);
  }

  auto result = objpipe::of(std::move(input))
      .iterate()
      .iterate()
      .async(singlethread_push())
      .to_vector()
      .get();

  CHECK_EQUAL(expect, result);
}

TEST(iteration_multithread_push) {
  constexpr int COUNT = 100 * 1000;
  std::vector<int> expect;
  for (int i = 0; i < COUNT; ++i)
    expect.push_back(i);

  std::vector<std::vector<int>> input;
  for (int i = 0; i < COUNT; i += 1000) {
    input.emplace_back();
    for (int j = i; j < i + 1000 && j < COUNT; ++j)
      input.back().push_back(j);
  }

  auto result = objpipe::of(std::move(input))
      .iterate()
      .iterate()
      .async(multithread_push())
      .to_vector()
      .get();

  CHECK_EQUAL(expect, result);
}

TEST(iteration_multithread_unordered_push) {
  constexpr int COUNT = 100 * 1000;
  std::vector<int> expect;
  for (int i = 0; i < COUNT; ++i)
    expect.push_back(i);

  std::vector<std::vector<int>> input;
  for (int i = 0; i < COUNT; i += 1000) {
    input.emplace_back();
    for (int j = i; j < i + 1000 && j < COUNT; ++j)
      input.back().push_back(j);
  }

  auto result = objpipe::of(std::move(input))
      .iterate()
      .iterate()
      .async(multithread_unordered_push())
      .to_vector()
      .get();

  std::sort(result.begin(), result.end()); // Because result has no ordering.
  CHECK_EQUAL(expect, result);
}

TEST(iteration_over_objpipes) {
  CHECK_EQUAL(
      std::vector<int>({ 0, 1, 2, 3, 4 }),
      objpipe::of(objpipe::new_array<int>({ 0, 1, 2 }), objpipe::new_array<int>({ 3, 4 }))
          .iterate()
          .to_vector());
}

TEST(iteration_over_collections) {
  CHECK_EQUAL(
      std::vector<int>({ 0, 1, 2, 3, 4 }),
      objpipe::of(std::list<int>({ 0, 1, 2 }), std::list<int>({ 3, 4 }))
          .iterate()
          .to_vector());
}

TEST(reader_to_vector) {
  CHECK_EQUAL(
      std::vector<int>({ 0, 1, 2, 3, 4 }),
      objpipe::new_array({ 0, 1, 2, 3, 4 })
          .to_vector());
}

TEST(reduce_op) {
  CHECK_EQUAL(std::optional<int>(42),
      objpipe::of(7, 7, 7, 7, 7, 7)
          .reduce(std::plus<>()));
}

int main() {
  return UnitTest::RunAllTests();
}
