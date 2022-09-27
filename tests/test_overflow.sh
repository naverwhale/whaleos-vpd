#!/bin/sh
#
# Copyright 2019 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -e

. ./functions.sh

BINARY="../vpd"
TMP_DIR="$(mktemp -d)"
BIOS_PACKS="gVpdInfo.tbz"
BIOS="${TMP_DIR}/broken.vpd"

test_image() {
  local pack="$1"

  echo "  testing '${pack}' ..."
  unpack_bios "${pack}" "${TMP_DIR}"

  #
  # Test -l (without parameter) case.
  # Expect error for invalid parameter.
  RUN ${VPD_ERR_DECODE} "${BINARY} -f ${BIOS} -l"
}

main() {
  for pack in ${BIOS_PACKS}
  do
    test_image "${pack}"
  done
}

main
clean_up

exit 0
