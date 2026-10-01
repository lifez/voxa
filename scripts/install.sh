#!/usr/bin/env bash
# Install the C daemon and CLI. Never source a credentials file.
set -euo pipefail
umask 077
cd "$(dirname "${BASH_SOURCE[0]}")/.."
[[ $(uname -s) == Linux ]] || { echo 'Use scripts/install-macos.sh on macOS.' >&2; exit 1; }
for cmd in make cc pkg-config wayland-scanner pw-record systemctl; do
  command -v "$cmd" >/dev/null || { echo "Missing $cmd (Arch: base-devel curl json-c pipewire wayland libxkbcommon)" >&2; exit 1; }
done
pkg-config --exists libcurl json-c wayland-client xkbcommon
make -C native clean all
: "${XDG_CONFIG_HOME:=$HOME/.config}"
app="$HOME/.local/share/voxa"
launcher="$HOME/.local/bin/voxa"
units="$XDG_CONFIG_HOME/systemd/user"
config="$XDG_CONFIG_HOME/voxa"
socket="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/voxa.sock"
active=false
if systemctl --user is-active --quiet voxa; then active=true; fi
state=$(native/voxa --socket "$socket" status 2>/dev/null || true)
if [[ $state == recording || $state == committing ]]; then
  echo 'Finish dictation before installing.' >&2; exit 1
fi
if $active && [[ $state != idle ]]; then
  echo 'Running service is not responding; stop it manually before installing.' >&2; exit 1
fi
if ! $active && [[ -n $state ]]; then
  echo 'A standalone daemon is running; stop it before installing.' >&2; exit 1
fi
mkdir -p "$app" "$(dirname "$launcher")" "$units" "$config" "$HOME/.local/share/voxa-backups"
backup=$(mktemp -d "$HOME/.local/share/voxa-backups/install-XXXXXX")
# Snapshot only app-owned paths. Keys and user config are never replaced.
if [[ -e $launcher || -L $launcher ]]; then cp -a "$launcher" "$backup/launcher"; fi
if [[ -f $app/voxa ]]; then cp -a "$app/voxa" "$backup/voxa"; fi
if [[ -f $units/voxa.service ]]; then cp -a "$units/voxa.service" "$backup/voxa.service"; fi
for override in 80-config.conf 90-native.conf; do
  if [[ -f $units/voxa.service.d/$override ]]; then cp -a "$units/voxa.service.d/$override" "$backup/$override"; fi
done
rollback() {
  local code=$?
  trap - ERR
  echo "Installation failed; restoring application files (backup: $backup)." >&2
  systemctl --user stop voxa || true
  rm -f "$launcher" "$app/voxa" "$units/voxa.service" "$units/voxa.service.d/80-config.conf" "$units/voxa.service.d/90-native.conf"
  [[ ! -e $backup/launcher && ! -L $backup/launcher ]] || cp -a "$backup/launcher" "$launcher"
  [[ ! -f $backup/voxa ]] || cp -a "$backup/voxa" "$app/voxa"
  [[ ! -f $backup/voxa.service ]] || cp -a "$backup/voxa.service" "$units/voxa.service"
  for override in 80-config.conf 90-native.conf; do
    if [[ -f $backup/$override ]]; then mkdir -p "$units/voxa.service.d"; cp -a "$backup/$override" "$units/voxa.service.d/$override"; fi
  done
  for item in dist node_modules package.json package-lock.json native ui; do
    [[ ! -e $backup/$item ]] || mv "$backup/$item" "$app/$item"
  done
  systemctl --user daemon-reload || true
  if $active; then systemctl --user restart voxa || true; fi
  exit "$code"
}
trap rollback ERR
if $active; then systemctl --user stop voxa; fi
install -m 755 native/voxa "$app/voxa.next"
mv -f "$app/voxa.next" "$app/voxa"
# Replace, never write through a legacy symlink.
ln -s "$app/voxa" "$launcher.next.$$"
mv -Tf "$launcher.next.$$" "$launcher"
# Explicit config path supports XDG_CONFIG_HOME even when systemd doesn't inherit it.
escaped=${XDG_CONFIG_HOME//\\/\\\\}; escaped=${escaped//\"/\\\"}; escaped=${escaped//%/%%}
install -m 600 systemd/voxa.service "$units/voxa.service"
mkdir -p "$units/voxa.service.d"
printf '[Service]\nEnvironment="XDG_CONFIG_HOME=%s"\n' "$escaped" > "$units/voxa.service.d/80-config.conf"
rm -f "$units/voxa.service.d/90-native.conf"
if [[ ! -e $config/config.json ]]; then install -m 600 config.example.json "$config/config.json"; fi
if [[ ! -e $config/env ]]; then printf 'ELEVENLABS_API_KEY=\n' > "$config/env"; fi
systemctl --user daemon-reload
if $active; then
  systemctl --user restart voxa
  ready=false
  for ((i=0; i<30; i++)); do
    if [[ $("$launcher" status 2>/dev/null || true) == idle ]]; then ready=true; break; fi
    sleep .1
  done
  $ready
fi
# Retire legacy application code only after the native installation is healthy.
for item in dist node_modules package.json package-lock.json native ui; do
  if [[ -e $app/$item ]]; then mv "$app/$item" "$backup/$item"; fi
done
trap - ERR
# OSD is optional; display integration cannot fail a working daemon install.
if command -v omarchy-shell >/dev/null && omarchy-shell shell ping >/dev/null 2>&1; then
  plugin="$XDG_CONFIG_HOME/omarchy/plugins/voxa.osd"
  mkdir -p "$plugin"
  cp omarchy/voxa-osd/manifest.json omarchy/voxa-osd/Osd.qml "$plugin/"
  omarchy-shell shell rescanPlugins >/dev/null || true
  omarchy plugin enable voxa.osd || true
fi
printf '\nInstalled native Voxa. Backup: %s\nRun: voxa setup\nSettings use a terminal; voxa doctor checks local dependencies.\n' "$backup"
