# Extracts sq_pref()/rw_related()/constprop() verbatim from shil.cpp so the test
# compiles the real source text rather than a hand-copied version that could drift.
# Usage: cmake -DIN=<shil.cpp> -DOUT=<out> -P extract_constprop.cmake
file(READ "${IN}" content)

set(extracted "")

# Pulls [begin_marker, end_marker) out of shil.cpp and appends it. Each range has
# to be a contiguous, self-contained run of functions.
macro(extract_range what begin_marker end_marker)
    string(FIND "${content}" "${begin_marker}" _begin)
    string(FIND "${content}" "${end_marker}" _end)

    if(_begin EQUAL -1 OR _end EQUAL -1 OR _end LESS _begin)
        message(FATAL_ERROR
            "could not locate ${what} in ${IN} -- did the function names or "
            "their order change? Update extract_constprop.cmake.")
    endif()

    math(EXPR _len "${_end} - ${_begin}")
    string(SUBSTRING "${content}" ${_begin} ${_len} _chunk)
    set(extracted "${extracted}${_chunk}")
endmacro()

# sq_reg_is_read() + both sq_pref() overloads, ending before the read groups
extract_range("sq_reg_is_read()..sq_pref()"
              "//The store queue is write only"
              "//Read Groups")

# rw_related()..constprop(), ending before read_v4m3z1
extract_range("rw_related()..constprop()"
              "void rw_related(RuntimeBlockInfo* blk)"
              "//read_v4m3z1")

file(WRITE "${OUT}" "// GENERATED from shil.cpp -- do not edit.\n#include \"constprop_mocks.h\"\n\n${extracted}")
