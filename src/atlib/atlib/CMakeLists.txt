file(GLOB_RECURSE sources CONFIGURE_DEPENDS *.h *.cpp)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${sources})

add_library(atlib STATIC ${sources})

target_include_directories(atlib
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

target_link_libraries(atlib
        PUBLIC
            meadow::meadow
        PRIVATE
            CURL::libcurl
            reflectcpp::reflectcpp
)
