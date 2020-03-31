find_package(UnitTest++)

macro (do_test binary)
  add_executable (test_${binary} ${binary}.cc)
  target_link_libraries (test_${binary} objpipe)
  target_link_libraries (test_${binary} UnitTest++)
  target_include_directories(test_${binary} ${UTPP_INCLUDE_DIRS})
  add_test (${binary} test_${binary})
endmacro (do_test)

if (UnitTest++_FOUND)
  include(CTest)

  do_test (objpipe)
endif ()
