cmake_minimum_required(VERSION 3.3)
project (objpipe)

set(OBJPIPE_VERSION_MAJOR 0 CACHE STRING "major version" FORCE)
set(OBJPIPE_VERSION_MINOR 0 CACHE STRING "minor version" FORCE)
set(OBJPIPE_VERSION ${OBJPIPE_VERSION_MAJOR}.${OBJPIPE_VERSION_MINOR} CACHE STRING "version" FORCE)

enable_testing()

include (CheckCXXCompilerFlag)
check_cxx_compiler_flag ("-std=c++17" STD_CXX17)
check_cxx_compiler_flag ("-std=c++1z" STD_CXX1Z)
if(STD_CXX17)
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
elseif(STD_CXX1Z)
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++1z")
else()
  message(WARNING "Cannot figure out how to enable C++17 on compiler.")
endif()

set(headers
    include/objpipe/array.h
    include/objpipe/callback.h
    include/objpipe/doc_.h
    include/objpipe/errc.h
    include/objpipe/interlock.h
    include/objpipe/merge.h
    include/objpipe/of.h
    include/objpipe/push_policies.h
    include/objpipe/reader.h
    )
set(headers_detail
    include/objpipe/detail/adapt.h
    include/objpipe/detail/adapter.h
    include/objpipe/detail/array_pipe.h
    include/objpipe/detail/callback_pipe.h
    include/objpipe/detail/deref_op.h
    include/objpipe/detail/empty_pipe.h
    include/objpipe/detail/filter_op.h
    include/objpipe/detail/flatten_op.h
    include/objpipe/detail/fwd.h
    include/objpipe/detail/interlock_pipe.h
    include/objpipe/detail/invocable_.h
    include/objpipe/detail/merge_pipe.h
    include/objpipe/detail/of_pipe.h
    include/objpipe/detail/peek_op.h
    include/objpipe/detail/push_op.h
    include/objpipe/detail/select_op.h
    include/objpipe/detail/task.h
    include/objpipe/detail/thread_pool.h
    include/objpipe/detail/transform_op.h
    include/objpipe/detail/transport.h
    include/objpipe/detail/virtual.h
    )

add_library (objpipe INTERFACE)

find_package (Boost COMPONENTS context REQUIRED)
target_link_libraries (objpipe INTERFACE ${Boost_CONTEXT_LIBRARY})
find_package (Threads REQUIRED)
if(CMAKE_USE_PTHREADS_INIT)
  target_link_libraries(objpipe INTERFACE ${CMAKE_THREAD_LIBS_INIT})
endif()

target_include_directories(objpipe INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${Boost_INCLUDE_DIR})

install(FILES ${headers} DESTINATION "include/objpipe")
install(FILES ${headers_detail} DESTINATION "include/objpipe/detail")
install(TARGETS objpipe EXPORT objpipe DESTINATION "lib")
install(EXPORT objpipe DESTINATION "lib/cmake/objpipe")

configure_file(objpipe-config-version.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/objpipe-config-version.cmake @ONLY)
install(FILES objpipe-config.cmake ${CMAKE_CURRENT_BINARY_DIR}/objpipe-config-version.cmake DESTINATION "lib/cmake/objpipe")

add_subdirectory (test)

find_package(Doxygen REQUIRED mscgen OPTIONAL_COMPONENTS dot)

if(DOXYGEN_FOUND)
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "README.md")
  doxygen_add_docs(objpipe-doc ${DOXYGEN_USE_MDFILE_AS_MAINPAGE} include)
endif()
