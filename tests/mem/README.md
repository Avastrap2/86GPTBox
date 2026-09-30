# Memory write-lookup regressions

Enable with `-DMEM_LOOKUP_TESTS=ON` in an existing CMake build, then build:

```text
cmake --build build --target mem_write_lookup_old_test mem_write_lookup_new_test mem_write_lookup_old_interpreter_test mem_write_lookup_new_interpreter_test
ctest --test-dir build -R ^mem_write_lookup_ --output-on-failure
```

The tests include the production `src/mem/mem.c`, not a rewritten lookup
algorithm. Interprocedural optimization is required to eliminate unused
platform and device dependencies. The option is off by default and does not
enable tracing or alter the emulator's selected CPU backend.

Four targets cover the old and new page layouts, each with and without
`USE_DYNAREC`. The interpreter-named targets test the non-dynarec compile
branches; they do not execute an interpreter or boot a guest.

Checks cover ordinary direct RAM access, start-only and end-only code-block
presence, the page currently being compiled, all four old-backend code-list
slots, dirty-word tracking in the old layout, multiple virtual aliases,
unrelated pages with the same translation bias, tracked aliases, the highest
virtual page, and replacement-ring wraparound. Physical-page invalidation
must work with an unaligned physical address and with a zero or unrelated
caller-supplied virtual address.

These are lookup/dirty-tracking tests, not a full self-modifying-code guest
test. They do not reproduce the complete Windows 95 ATI Properties failure,
execute ATIDIAG or HWiNFO32, or establish a cause for ATI rendering artifacts.
