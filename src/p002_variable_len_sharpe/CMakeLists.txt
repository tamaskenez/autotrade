file(GLOB_RECURSE sources CONFIGURE_DEPENDS *.h *.cpp)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${sources})

add_executable(p002_variable_len_sharpe ${sources})

target_link_libraries(p002_variable_len_sharpe PRIVATE
        atlib
)

target_compile_definitions(p002_variable_len_sharpe PRIVATE WORKSPACE_DIR="${PROJECT_SOURCE_DIR}/_md")
