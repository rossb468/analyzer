# Linked with the allocation trap, so the callback-path tests can prove that
# building buffers and invoking a wrapped callback allocate nothing. The
# coreaudio and ios test files wrap their contents in the platform guard and
# compile to nothing everywhere else.
analyzer_module_tests(audio DEPS analyzer::alloc_trap)
