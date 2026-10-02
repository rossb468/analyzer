analyzer_module(engine
    SOURCES engine.cpp ring.cpp rt.cpp
    DEPS analyzer::base analyzer::dsp)

# The allocation trap: replacement operator new/delete. An object library, so
# that whatever links it gets the object file itself - see alloc_trap.cpp.
add_library(analyzer_alloc_trap OBJECT alloc_trap.cpp)
add_library(analyzer::alloc_trap ALIAS analyzer_alloc_trap)
target_link_libraries(analyzer_alloc_trap PUBLIC analyzer::engine)
analyzer_warnings(analyzer_alloc_trap)
