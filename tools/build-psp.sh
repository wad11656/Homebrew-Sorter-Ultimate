#!/usr/bin/env bash
set -euo pipefail

# Default to the older SDK shared with Spybotics on this Windows/WSL machine.
# Set PSPDEV explicitly to use another installation.
export PSPDEV="${PSPDEV:-$HOME/pspdev-machybrid}"
export PATH="$PSPDEV/bin:$PATH"

if [[ ! -x "$PSPDEV/bin/psp-gcc" ]]; then
    echo "PSP compiler missing: $PSPDEV/bin/psp-gcc. Set PSPDEV to your SDK directory." >&2
    exit 1
fi

cd "$(dirname "${BASH_SOURCE[0]}")/.."
echo "PSPDEV=$PSPDEV"
psp-gcc --version | head -n 1
make "$@"
