# Applied to the Unicorn sources after download.
#
# Unicorn translates guest code as if only one CPU existed: curr_cflags() returns 0, so
# LDREX/STREX become plain loads/stores and DMB generates nothing. ARM64Bridge runs every
# guest thread on its own engine at the same time, so the code must be generated for a
# parallel context (CF_PARALLEL): exclusives become host compare-and-swap and barriers
# become host barriers, exactly as in QEMU's multi-threaded TCG.
set(f "${UNICORN_SRC}/qemu/include/exec/exec-all.h")
file(READ "${f}" text)
set(old "static inline uint32_t curr_cflags(void)\n{\n    return 0;")
set(new "static inline uint32_t curr_cflags(void)\n{\n    return CF_PARALLEL; /* arm64bridge */")
string(FIND "${text}" "${new}" already)
if(already EQUAL -1)
  string(FIND "${text}" "${old}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "patch_unicorn: curr_cflags() not found in ${f}")
  endif()
  string(REPLACE "${old}" "${new}" text "${text}")
  file(WRITE "${f}" "${text}")
  message(STATUS "patch_unicorn: translation for parallel guest threads enabled")
endif()
