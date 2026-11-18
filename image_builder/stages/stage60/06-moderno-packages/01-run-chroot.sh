#!/bin/bash -e

# Replace the repository builds with ours. dpkg -i (not apt) so an identical version
# number still gets overwritten; dependencies are already installed by stage 02.
echo "Stage60/06: Installing crankshaft-moderno packages"

dpkg -i /tmp/moderno-debs/*.deb
rm -rf /tmp/moderno-debs

# Keep "apt upgrade" from silently swapping them back for the upstream builds.
apt-mark hold crankshaft-core crankshaft-ui-slim

dpkg-query -W crankshaft-core crankshaft-ui-slim
