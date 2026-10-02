# Linked with the allocation trap, so the rt tests can prove it fires and every
# other engine test runs with it armed.
analyzer_module_tests(engine DEPS analyzer::alloc_trap)
