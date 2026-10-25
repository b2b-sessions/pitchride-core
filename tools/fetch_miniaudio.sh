#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Re-download miniaudio (single-header audio library, public domain / MIT-0)
# used by the demo for real speaker output. A copy is already included.
set -euo pipefail
VERSION="${1:-0.11.21}"
DIR="$(cd "$(dirname "$0")/.." && pwd)/third_party/miniaudio"
mkdir -p "$DIR"
curl -sSfL -o "$DIR/miniaudio.h" "https://raw.githubusercontent.com/mackron/miniaudio/$VERSION/miniaudio.h"
echo "miniaudio $VERSION downloaded to $DIR"
