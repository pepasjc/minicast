# rcheevos v12.4.0 (external/rcheevos, unmodified upstream) + the minicast
# RetroAchievements module (ra/). Included by faux/CMakeLists.txt and
# tests/CMakeLists.txt; defines RA_SOURCES and RA_INCLUDE_DIRS.

get_filename_component(_ra_root ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
set(_rchv ${_ra_root}/external/rcheevos)

set(RCHEEVOS_SOURCES
    ${_rchv}/src/rc_client.c
    ${_rchv}/src/rc_compat.c
    ${_rchv}/src/rc_util.c
    ${_rchv}/src/rc_version.c
    ${_rchv}/src/rcheevos/alloc.c
    ${_rchv}/src/rcheevos/condition.c
    ${_rchv}/src/rcheevos/condset.c
    ${_rchv}/src/rcheevos/consoleinfo.c
    ${_rchv}/src/rcheevos/format.c
    ${_rchv}/src/rcheevos/lboard.c
    ${_rchv}/src/rcheevos/memref.c
    ${_rchv}/src/rcheevos/operand.c
    ${_rchv}/src/rcheevos/rc_validate.c
    ${_rchv}/src/rcheevos/richpresence.c
    ${_rchv}/src/rcheevos/runtime.c
    ${_rchv}/src/rcheevos/runtime_progress.c
    ${_rchv}/src/rcheevos/trigger.c
    ${_rchv}/src/rcheevos/value.c
    ${_rchv}/src/rapi/rc_api_common.c
    ${_rchv}/src/rapi/rc_api_info.c
    ${_rchv}/src/rapi/rc_api_runtime.c
    ${_rchv}/src/rapi/rc_api_user.c
    ${_rchv}/src/rhash/md5.c
    ${_rchv}/src/rhash/hash.c
    ${_rchv}/src/rhash/hash_disc.c
    ${_rchv}/src/rhash/cdreader.c
)

# Only the disc hashers are needed (Dreamcast .gdi / .cue); the ROM, zip and
# 3DS (encrypted) paths are compiled out of hash.c.
# rcheevos evaluates float memory reads and comparisons: keep it out of the
# emulator's -ffast-math (NaN / inf / exact compares must behave).
set_source_files_properties(${RCHEEVOS_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "RC_HASH_NO_ROM;RC_HASH_NO_ZIP;RC_HASH_NO_ENCRYPTED"
    COMPILE_OPTIONS "-fno-fast-math;-fno-unsafe-math-optimizations;-w")

set(RA_SOURCES
    ${_ra_root}/ra/ra_dc.cpp
    ${_ra_root}/ra/ra_http.cpp
    ${RCHEEVOS_SOURCES}
)

set(RA_INCLUDE_DIRS
    ${_ra_root}/ra
    ${_rchv}/include
)
