# Vendored from https://github.com/richgel999/bc7enc_rdo @ b9438627eef73a1157e84201b6fa6eb2ffd6d9f0,
# used under the MIT option (ALTERNATIVE A) of its dual MIT / public-domain licence.
# Minimal set: BC7 encode (bc7enc), BC7 decode (bc7decomp) and the entropy reduction transform
# (ert), plus the two headers ert.cpp includes (utils.h, dds_defs.h). bc7decomp_ref.cpp is vendored
# with bc7decomp but not built: nothing calls the reference decoder.
# bc7e.ispc (Apache 2.0), LodePNG, rgbcx, miniz and the reference driver are NOT vendored.
# Local patches: `#pragma once` added to bc7enc.h, which upstream ships without an include guard, and
# `#include <stdint.h>` added to ert.h, which uses the fixed-width types without it (GNU build).
set(SRCS
    bc7enc.cpp
    bc7decomp.cpp
    ert.cpp
)
add_library(bc7enc_rdo STATIC ${SRCS})
target_compile_features(bc7enc_rdo PRIVATE cxx_std_17)
target_include_directories(bc7enc_rdo PUBLIC .)
