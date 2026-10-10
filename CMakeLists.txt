cmake_minimum_required(VERSION 3.20)
project(nanomatch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(nanomatch INTERFACE)
target_include_directories(nanomatch INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(nanomatch INTERFACE cxx_std_20)

function(nanomatch_apply_flags tgt)
  target_compile_options(${tgt} PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall;-Wextra;-Wpedantic>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang,AppleClang,GNU>>:-O3;-march=native;-flto>
    $<$<AND:$<CONFIG:RelWithDebInfo>,$<CXX_COMPILER_ID:Clang,AppleClang,GNU>>:-O3;-march=native>
  )
  target_link_options(${tgt} PRIVATE
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang,AppleClang,GNU>>:-flto>
  )
endfunction()

add_executable(nanomatch_demo src/main.cpp)
target_link_libraries(nanomatch_demo PRIVATE nanomatch)
nanomatch_apply_flags(nanomatch_demo)

add_executable(nanomatch_bench src/benchmark/matching_bench.cpp)
target_link_libraries(nanomatch_bench PRIVATE nanomatch)
nanomatch_apply_flags(nanomatch_bench)

add_executable(order_book_tests tests/order_book_tests.cpp)
target_link_libraries(order_book_tests PRIVATE nanomatch)
nanomatch_apply_flags(order_book_tests)

enable_testing()
add_test(NAME order_book_tests COMMAND order_book_tests)

install(TARGETS nanomatch_demo nanomatch_bench order_book_tests
        RUNTIME DESTINATION bin)
install(DIRECTORY src/core DESTINATION include/nanomatch)
