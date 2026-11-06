#!/usr/bin/env bash
#
# Packages needed to build and test maïs on Debian/Ubuntu.
#
# maïs embeds Python, so the interpreter's development files are required. No
# Vulkan SDK, OpenXR runtime, GLFW, JDK, or Android NDK is involved.
set -euo pipefail

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git

# maïs embeds Python, so a development installation is required. CI provides
# the interpreter through actions/setup-python — the same source macOS and
# Windows use — and sets MAIS_INSTALL_PYTHON_DEV=0 so a second, different
# installation does not appear next to it. Local builds keep the default.
if [ "${MAIS_INSTALL_PYTHON_DEV:-1}" != "0" ]; then
    sudo apt-get install -y --no-install-recommends \
        python3 \
        python3-dev
fi
