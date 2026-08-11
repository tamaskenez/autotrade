add_executable(playground main.cpp)
target_link_libraries(playground PRIVATE atlib)

# Where the Python tools keep everything they generate, the raw payload cache
# included -- see src/marketdata/config.py. Baked in at configure time rather
# than resolved from the working directory, so the playground reads the same
# cache from wherever it is run.
target_compile_definitions(playground PRIVATE WORKSPACE_DIR="${PROJECT_SOURCE_DIR}/_md")
