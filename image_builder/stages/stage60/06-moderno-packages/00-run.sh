#!/bin/bash -e

# Copy the locally built crankshaft-core / crankshaft-ui-slim packages (pinch-zoom,
# phone-driven day/night, new home screen) into the image. The workflow puts them in files/debs.
echo "Stage60/06: Copying crankshaft-moderno packages into the image"

shopt -s nullglob
debs=(files/debs/*.deb)
if [ ${#debs[@]} -eq 0 ]; then
    echo "No .deb packages in $(pwd)/files/debs - refusing to build an image without them" >&2
    exit 1
fi

install -d "${ROOTFS_DIR}/tmp/moderno-debs"
install -m 644 "${debs[@]}" "${ROOTFS_DIR}/tmp/moderno-debs/"
