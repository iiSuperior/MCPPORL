# Runs bench_tick with default arguments and checks the bit-exact state hash,
# proving this compiler/platform simulates identically to the reference.
execute_process(COMMAND ${BENCH} ${SIN} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
message(STATUS "${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "bench_tick failed (${rc})")
endif()
if(NOT out MATCHES "state hash: a17145a8224fa1e7")
  message(FATAL_ERROR "state hash differs from the reference a17145a8224fa1e7: not bit-identical on this platform")
endif()
