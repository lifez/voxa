#!/bin/bash
set -euo pipefail
[[ "$(uname -s)" == Darwin ]] || { echo 'Run on macOS' >&2; exit 1; }
for cmd in node npm ffmpeg swiftc; do command -v "$cmd" >/dev/null || { echo "Missing $cmd (install Node.js, ffmpeg via Homebrew, and Xcode Command Line Tools)" >&2; exit 1; }; done
cd "$(dirname "$0")/.."
npm ci
npm run build
install_dir="$HOME/Library/Application Support/voxa"
mkdir -p "$install_dir/dist" "$install_dir/bin" "$HOME/.local/bin" "$HOME/Library/LaunchAgents" "$HOME/Library/Logs" "${XDG_CONFIG_HOME:-$HOME/.config}/voxa"
cp dist/*.js "$install_dir/dist/"
rm -rf "$install_dir/node_modules"
cp -R node_modules "$install_dir/"
swiftc -O mac/voxa-keys.swift -o "$install_dir/bin/voxa-keys"
ln -sf "$install_dir/dist/index.js" "$HOME/.local/bin/voxa"
chmod +x "$install_dir/dist/index.js"
config="${XDG_CONFIG_HOME:-$HOME/.config}/voxa"
if [[ ! -f "$config/config.json" ]]; then cp config.example.json "$config/config.json"; chmod 600 "$config/config.json"; fi
if [[ ! -f "$config/env" ]]; then printf 'ELEVENLABS_API_KEY=\n' > "$config/env"; chmod 600 "$config/env"; fi
node_path="$(command -v node)"
# Plist via PlistBuddy avoids XML escaping of paths (including spaces and ampersands).
plist="$HOME/Library/LaunchAgents/com.voxa.daemon.plist"
/usr/libexec/PlistBuddy -c Clear "$plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c 'Add :Label string com.voxa.daemon' "$plist"
/usr/libexec/PlistBuddy -c 'Add :ProgramArguments array' "$plist"
/usr/libexec/PlistBuddy -c "Add :ProgramArguments:0 string $node_path" "$plist"
/usr/libexec/PlistBuddy -c "Add :ProgramArguments:1 string $install_dir/dist/index.js" "$plist"
/usr/libexec/PlistBuddy -c 'Add :ProgramArguments:2 string daemon' "$plist"
/usr/libexec/PlistBuddy -c 'Add :RunAtLoad bool true' "$plist"
/usr/libexec/PlistBuddy -c 'Add :KeepAlive bool true' "$plist"
/usr/libexec/PlistBuddy -c "Add :StandardOutPath string $HOME/Library/Logs/voxa.log" "$plist"
/usr/libexec/PlistBuddy -c "Add :StandardErrorPath string $HOME/Library/Logs/voxa-error.log" "$plist"
launchctl bootout "gui/$(id -u)" "$plist" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$plist"
echo 'Installed. Run ~/.local/bin/voxa setup, then grant Accessibility to voxa-keys in System Settings and restart the agent.'
echo "To restart: launchctl kickstart -k gui/$(id -u)/com.voxa.daemon"
