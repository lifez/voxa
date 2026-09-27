#!/usr/bin/env bash
# Explicit opt-in for an existing Linux installation. Keeps Node and keys intact.
set -euo pipefail
umask 077
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
app="$HOME/.local/share/voxa"
launcher="$HOME/.local/bin/voxa"
dropin="$HOME/.config/systemd/user/voxa.service.d/90-native.conf"

wait_idle() {
  for ((i=0; i<30; i++)); do
    if [[ $("$launcher" status 2>/dev/null || true) == idle ]]; then return 0; fi
    sleep .1
  done
  return 1
}
restore() {
  local backup=$1
  [[ -e "$backup/launcher" || -L "$backup/launcher" ]] || { echo 'Invalid backup: missing launcher' >&2; return 1; }
  systemctl --user stop voxa || return
  cp -a -- "$backup/launcher" "$launcher.restore.$$" || return
  mv -Tf -- "$launcher.restore.$$" "$launcher" || return
  if [[ -f "$backup/90-native.conf" ]]; then
    install -m 600 -- "$backup/90-native.conf" "$dropin" || return
  else
    rm -f -- "$dropin" || return
  fi
  if [[ -f "$backup/voxa-c" ]]; then
    install -m 755 -- "$backup/voxa-c" "$app/native/voxa-c.restore.$$" || return
    mv -Tf -- "$app/native/voxa-c.restore.$$" "$app/native/voxa-c" || return
  fi
  systemctl --user daemon-reload || return
  systemctl --user restart voxa || return
  wait_idle
}
if [[ ${1-} == --rollback ]]; then
  [[ $# == 2 ]] || { echo 'Usage: use-native.sh --rollback BACKUP_DIRECTORY' >&2; exit 2; }
  state=$("$launcher" status 2>/dev/null || true)
  [[ $state != recording && $state != committing ]] || { echo 'Finish dictation before rollback' >&2; exit 1; }
  restore "$2"
  echo 'Restored previous daemon and launcher.'
  exit 0
fi
[[ $# == 0 ]] || { echo 'Usage: use-native.sh [--rollback BACKUP_DIRECTORY]' >&2; exit 2; }
[[ $(uname -s) == Linux ]] || { echo 'Linux only' >&2; exit 1; }
[[ -x "$launcher" && -f "$app/dist/index.js" ]] || { echo 'Install Node Voxa first; this is an opt-in migration, not a fresh installer.' >&2; exit 1; }
for command in make cc pkg-config node pw-record wtype systemctl; do command -v "$command" >/dev/null; done
# Always build release binaries, even if the last local build used sanitizers.
make -C "$root/native" clean all test
backup=$(mktemp -d "$app/backup-before-native-$(date +%Y%m%d-%H%M%S).XXXXXX")
cp -a -- "$launcher" "$backup/launcher"
if [[ -f "$dropin" ]]; then cp -a -- "$dropin" "$backup/90-native.conf"; fi
if [[ -f "$app/native/voxa-c" ]]; then cp -a -- "$app/native/voxa-c" "$backup/voxa-c"; fi
if [[ -f "$HOME/.config/systemd/user/voxa.service" ]]; then cp -a -- "$HOME/.config/systemd/user/voxa.service" "$backup/voxa.service"; fi
# Do not interrupt a dictation (check again after the build/tests).
[[ $("$launcher" status 2>/dev/null || true) == idle ]] || { echo 'Daemon is not idle; migration cancelled.' >&2; exit 1; }
echo "Backup: $backup"
on_error() {
  local code=$?
  trap - ERR
  echo 'Migration failed; restoring the previous installation.' >&2
  restore "$backup" || echo "Automatic rollback failed; backup is $backup" >&2
  exit "$code"
}
trap on_error ERR
systemctl --user stop voxa
mkdir -p -- "$app/native" "$(dirname -- "$dropin")"
install -m 755 -- "$root/native/voxa-c" "$app/native/voxa-c.next.$$"
mv -Tf -- "$app/native/voxa-c.next.$$" "$app/native/voxa-c"
# Atomic replacement, not writing through the old launcher symlink.
install -m 755 -- "$root/native/voxa-launcher" "$launcher.next.$$"
mv -Tf -- "$launcher.next.$$" "$launcher"
install -m 600 -- "$root/native/90-native.conf" "$dropin"
systemctl --user daemon-reload
systemctl --user restart voxa
wait_idle
systemctl --user is-active --quiet voxa
pid=$(systemctl --user show voxa --property=MainPID --value)
[[ $(readlink -- "/proc/$pid/exe") == "$app/native/voxa-c" ]]
trap - ERR
echo 'C daemon active; existing shortcuts use the native client. Node files are preserved.'
printf 'Rollback: bash %q --rollback %q\n' "$root/scripts/use-native.sh" "$backup"
