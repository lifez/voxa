#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
for cmd in node npm pw-record wl-copy wtype; do command -v "$cmd" >/dev/null || { echo "Missing $cmd (Arch: nodejs npm pipewire wl-clipboard wtype)" >&2; exit 1; }; done
: "${XDG_CONFIG_HOME:=$HOME/.config}"
install_dir="$HOME/.local/share/voxa"
mkdir -p "$install_dir" "$HOME/.local/bin" "$XDG_CONFIG_HOME/voxa" "$XDG_CONFIG_HOME/systemd/user"
npm ci --no-audit --no-fund
npm run build
# No secrets or local config are copied into the installed application.
cp -r dist ui package.json package-lock.json "$install_dir/"
(cd "$install_dir" && npm ci --omit=dev --no-audit --no-fund)
ln -sfn "$install_dir/dist/index.js" "$HOME/.local/bin/voxa"
chmod +x "$install_dir/dist/index.js"
if [[ ! -e "$XDG_CONFIG_HOME/voxa/config.json" ]]; then
  if [[ -f "$XDG_CONFIG_HOME/scribe-dictation/config.json" ]]; then
    cp "$XDG_CONFIG_HOME/scribe-dictation/config.json" "$XDG_CONFIG_HOME/voxa/config.json"
  else
    cp config.example.json "$XDG_CONFIG_HOME/voxa/config.json"
  fi
fi
if [[ ! -e "$XDG_CONFIG_HOME/voxa/env" ]]; then
  if [[ -f "$XDG_CONFIG_HOME/scribe-dictation/env" ]]; then
    cp "$XDG_CONFIG_HOME/scribe-dictation/env" "$XDG_CONFIG_HOME/voxa/env"
  else
    printf '# Set your key on the next line (no quotes needed):\nELEVENLABS_API_KEY=\n' > "$XDG_CONFIG_HOME/voxa/env"
  fi
  chmod 600 "$XDG_CONFIG_HOME/voxa/env"
fi
cp systemd/voxa.service "$XDG_CONFIG_HOME/systemd/user/"
# Omarchy native, focus-free on-screen status. Only touch this project's plugin.
if command -v omarchy-shell >/dev/null && omarchy-shell shell ping >/dev/null 2>&1; then
  plugin="$XDG_CONFIG_HOME/omarchy/plugins/voxa.osd"
  mkdir -p "$plugin"
  cp omarchy/voxa-osd/manifest.json omarchy/voxa-osd/Osd.qml "$plugin/"
  omarchy-shell shell rescanPlugins >/dev/null
  omarchy plugin enable voxa.osd
fi
systemctl --user daemon-reload
printf '\nInstalled. Run: voxa setup\nThis guides API key, shortcuts, language, microphone, and enables the service.\nLater: voxa settings (GUI with zenity, terminal fallback); voxa doctor for diagnostics.\n'
