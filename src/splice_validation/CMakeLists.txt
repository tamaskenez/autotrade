file(GLOB_RECURSE sources CONFIGURE_DEPENDS *.h *.cpp)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${sources})

add_executable(splice_validation ${sources})

target_link_libraries(splice_validation PRIVATE
    atlib
)

target_compile_definitions(splice_validation PRIVATE WORKSPACE_DIR="${PROJECT_SOURCE_DIR}/_md")
