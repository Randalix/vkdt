#!/bin/sh
set -e
cd "$(dirname "$0")"
make -j$(sysctl -n hw.ncpu)
ln -sf "$(pwd)/bin/vkdt.sh" ~/.local/bin/vkdt
echo "installed: ~/.local/bin/vkdt -> $(pwd)/bin/vkdt.sh"
