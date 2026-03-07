#!/bin/sh
DIR="$(cd "$(dirname "$(readlink "$0" || echo "$0")")" && pwd)"
export DYLD_LIBRARY_PATH=/opt/homebrew/opt/vulkan-loader/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
export VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
exec "$DIR/vkdt" "$@"
