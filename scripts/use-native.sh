#!/usr/bin/env bash
# Compatibility entry point: all new installations are native.
set -euo pipefail
if [[ $# != 0 ]]; then
  echo 'Legacy opt-in/rollback is retired. See docs/migration.md for backup recovery.' >&2
  exit 2
fi
exec bash "$(dirname "${BASH_SOURCE[0]}")/install.sh"
