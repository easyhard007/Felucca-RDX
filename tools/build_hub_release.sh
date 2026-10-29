#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the one normal package published to The FM-1 Firmware Hub.  The clean
# package remains available only from the Felucca-RDX installer.
set -eu

version=${1:?usage: tools/build_hub_release.sh VERSION}
source_version=$(sed -n 's/^#define RDX_VERSION "\([^"]*\)"$/\1/p' firmware/src/rdx_version.h)
if [ -z "$source_version" ] || [ "$version" != "$source_version" ]; then
    echo "release tag version $version does not match RDX_VERSION $source_version" >&2
    exit 1
fi

FELUCCA_RDX=1 \
FELUCCA_FM6_ONLY=1 \
FELUCCA_CPU2=1 \
JIELI_DOCKER=0 \
    ./build.sh

mkdir -p build/hub
rm -f build/hub/Felucca-RDX.fwsc
cp build/felucca.fwsc build/hub/Felucca-RDX.fwsc

