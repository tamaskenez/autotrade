file(GLOB_RECURSE sources CONFIGURE_DEPENDS *.h *.cpp)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${sources})

add_executable(p001_dual_mom_fixed_etf ${sources})

target_link_libraries(p001_dual_mom_fixed_etf PRIVATE
    atlib
)