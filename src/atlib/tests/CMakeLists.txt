file(GLOB_RECURSE sources CONFIGURE_DEPENDS *.h *.cpp)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${sources})

add_executable(atlib_tests ${sources})

target_link_libraries(atlib_tests
        PRIVATE
            atlib
            GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(atlib_tests)
