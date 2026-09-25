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
    git \
    python3 \
    python3-dev
