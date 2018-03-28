# objpipe

Objpipe is a library for iterating over collections, performing transformations,
and doing all that in a type agnostic manner.

## Requirements

C++17, although you'll probably get by with anything higher than C++14 (clang-4.0.0 with -std=c++1z works fine for me).

## Usage

An objpipe has three components:
1. A source of data.
2. A series of transformations on the data.
3. Accessing the data, for example via iteration, reduction, or pushing it onward.

Objpipe allows interface boundaries between each of those.

## Rationale

I have a lot of files that model collections of data.
And there are different file formats.
And I need to iterate over all this data.

A collection interface wouldn't work,
because it would pin down the iterator type.
In the example below, the iterator type necessitates all files
to contain a vector:

    class FileInterface {
     public:
      // The interface now imposes a specific collection on all
      // implementations, in this case std::vector<int>.
      virtual std::vector<int>::iterator begin() const = 0;
      virtual std::vector<int>::iterator end() const = 0;
    };

The standard way of dealing with this, is to use a callback function.
However, this makes it very hard to do composition, as each callback
needs to wrap the composing callback, leading to hard to understand code.

    class FileInterface {
     public:
      virtual void visit(std::function<void(int)> callback) = 0;
    };

    class OnlyEvenNumbersFile
    : public FileInterface
    {
     public:
      // This function becomes rather hard to understand, if there
      // is a more complex set of operations going on!
      virtual void visit(std::function<void(int)> callback) override {
        file->visit(
            [callback](int x) {
              if (x % 2 == 0)
                callback(x);
            });
      }

     private:
      std::shared_ptr<FileInterface> file;
    };

Instead of this, I wanted to decouple the interface and implementation,
while still keeping the code readable.
Objpipe does this, by allowing functional (read-only) access to data,
abstracting away the implementation and life time considerations.

This way, each implementation can deal with data in the way that's best
suited for it.
While composition can be achieved by applying transformations on the objpipe.

    class FileInterface {
     public:
      virtual objpipe::reader<int> getData() const = 0;
    };

    class VectorFile
    : public std::enable_shared_from_this<VectorFile>,
      public FileInterface
    {
     public:
      // Not a file, just a collection with data.
      virtual objpipe::reader<int> getData() const override {
        return objpipe::of(this->shared_from_this())
            .deref()
            .transform(&VectorFile::data)
            .iterate();
      }

     private:
      std::vector<int> data;
    };

    class StreamFile
    : public FileInterface
    {
     public:
      // Read directly from a file.
      virtual objpipe::reader<int> getData() const override {
        std::fstream file(fileName, std::ios_base::in);

        // objpipe::new_array iterates over the range at construction time.
        // It's possible to delay this until the objpipe is evaluated,
        // but since file is not iterable by itself, this would require some
        // code, which is beyond the scope of this example.
        return objpipe::new_array(
            std::istream_iterator<int>(file)
            std::istream_iterator<int>());
      }

     private:
      std::string fileName;
    };

    class ManyFiles
    : public FileInterface
    {
     public:
      // Composition pattern, that concatenates contents from multiple files.
      virtual objpipe::reader<int> getData() const override {
        objpipe::of(files) // Copy, since passed by lvalue reference.
            .iterate()
            .transform(
                [](const std::unique_ptr<FileInterface>& file_ptr) {
                  return file_ptr->getData();
                })
            .iterate(); // Unpack the collections from getData().
      }

     private:
      std::vector<std::unique_ptr<FileInterface>> files;
    };

As can be seen, this allows for a wide variety of implementations, all of
which are hidden behind a common interface.

In addition, the caller of the ``FileInterface::getData()`` method is free
to decide how to handle the data:
1. they could iterate over it
2. they could copy it into a new collection
3. they could decide to go for a callback after all (but now, the choice is not imposed by the interface)
4. etc.

### Example: simple transformation

This example computes the squares of 1, 2, and 3.

    #include <objpipe/of.h>

    objpipe::of(1, 2, 3) // Create a source with three numbers.
        .transform([](int x) { return x * x; }) // Compute squares.
        .to_vector(); // returns std::vector<int>({ 1, 4, 9 }).

### Example: traversing interface boundaries

This example shows how the source can be abstracted away.

The ``objpipe::reader<T>`` allows us to abstract away a specific implementation
behind its interface.
Without conversion to ``objpipe::reader<T>``, the entire template of operations
would be visible.

    #include <vector>
    #include <cstdint>
    #include <objpipe/reader.h>
    #include <objpipe/array.h>
    #include <objpipe/callback.h>

    class MyIntf {
     public:
      virtual auto makeData() -> objpipe::reader<int> = 0;
    };

    class MyVectorImpl
    : public MyIntf
    {
     public:
      virtual auto makeData() -> objpipe::reader<int> override {
        return objpipe::new_array(data.begin(), data.end());
      }

     private:
      std::vector<int> data;
    };

    class MyCallbackImpl
    : public MyIntf {
     public:
      virtual auto makeData() -> objpipe::reader<int> override {
        return objpipe::new_callback<int>(
            [](auto& cb) {
              for (int i = 0; i < 42; ++i)
                cb(i);
            });
      }
    };

    std::uintmax_t countEvenNumbers(MyIntf& src) {
      return src.makeData()
          .filter([](int x) { return x % 2 == 0; })
          .count();
    }

### Example: map reduce

Various ways of computing the maximum value.

    #include <future>
    #include <optional>
    #include <objpipe/of.h>
    #include <objpipe/push_policies.h>

    // Compute immediately.
    std::optional<int> max = objpipe::of(3, 1, 2)
        .max();

    // Use asynchronous reduction.
    std::future<std::optional<int>> max_future = objpipe::of(3, 1, 2)
        .async()
        .max();

    // Use parallel reduction.
    std::future<std::optional<int>> max_future = objpipe::of(3, 1, 2)
        .async(objpipe::multithread_push())
        .max();

Another map reduce, that collects all elements into a set.

    #include <set>
    #include <utility>

    // Generic reduction.
    std::future<std::shared_ptr<std::set<int>>> int_set = objpipe::of(3, 1, 2)
        .async(objpipe::multithread_unordered_push())
        .reduce(
            []() -> { // Factory for initial state of reducer.
              return std::set<int>();
            },
            [](std::set<int>& state, int&& element) { // Value acceptor functor.
              state.insert(std::move(element));
            },
            [](std::set<int>& dst, std::set<int>&& to_add) { // Merge operation across states.
              state.merge(std::move(to_add));
            },
            [](std::set<int>&& state) { // Extract result from reduction.
              return std::make_shared<std::set<int>>(std::move(state));
            });

## Linking

This is a header-only library.

To use it, you can simple supply ``-I/path/to/objpipe/include`` to the
compiler.
The callback requires that boost/context be linked in and
that boost/coroutines2 is on the include path.

If you're using CMake, you can instead import the library using (for instance)
a git submodule:

    add_subdirectory(path/to/objpipe)
    target_link_libraries(my_target objpipe)

If objpipe is properly installed, the following should work:

    find_package(objpipe 0.0 REQUIRED)
    target_link_libraries(my_target objpipe)

(objpipe is an interface library, so it does not have an actual library,
but does contain options to link correctly (thread-safe) and get the correct
includes via the target.)
In both cases, the include directories are set correctly.

## Related work

1. [Boost iterator](http://www.boost.org/doc/libs/1_66_0/libs/iterator/doc/index.html)
   provides an alternative for transforming and filtering.
   Contrary to objpipe,
   it also allows for the iterator to modify the collection it iterates over (under certain conditions)
   and usually maintains the iterator category.
2. [Reactive Extensions for C++](https://github.com/Reactive-Extensions/RxCpp)
   implements transformations and iteration.
   Like objpipe's push interface, it is based on streaming values.
   In addition, its observables allow for multiple subscribers, while objpipe always uses a single subscriber.
   Unlike objpipe, it does not have a pull based interface.
3. A combination of
   [STL algorithm](http://en.cppreference.com/w/cpp/header/algorithm) calls, such as
   [std::transform](http://en.cppreference.com/w/cpp/algorithm/transform),
   [std::copy\_if](http://en.cppreference.com/w/cpp/algorithm/copy),
   can be used to achieve the same transformation effects, but would lose the streaming property.

## Additional Documentation

Documentation can be generated by the ``objpipe-doc`` target.

Or alternatively, can be [browsed online](https://www.stack.nl/~ariane/objpipe/).
