#!/bin/sh
# Regenerates libretro/ap/ap_mbedtls_prefix.h from a finished Linux build
# (run `make -C libretro` first).
set -e
cd "$(dirname "$0")/../../libretro"
{
  echo "/* Generated: gives this core's mbedTLS copy its own symbol names, so it can be"
  echo " * linked into programs that contain another mbedTLS (RetroArch on the 3DS"
  echo " * statically links cores and bundles mbedTLS 2.x). Regenerate after updating"
  echo " * mbedTLS with tests/archipelago/gen_prefix.sh. */"
  echo "#ifndef AP_MBEDTLS_PREFIX_H"
  echo "#define AP_MBEDTLS_PREFIX_H"
  { nm -g --defined-only ap/mbedtls/library/*.o | awk '{print $3}' | sed 's/^ap_//' | grep -E '^(mbedtls_|psa_)'; echo mbedtls_hardware_poll; } \
    | sort -u | awk '{print "#define " $1 " ap_" $1}'
  echo "#endif"
} > ap/ap_mbedtls_prefix.h.new
mv ap/ap_mbedtls_prefix.h.new ap/ap_mbedtls_prefix.h
