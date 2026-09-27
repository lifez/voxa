#!/bin/bash
set -euo pipefail
[[ "$(uname -s)" == Darwin ]] || { echo 'Run on macOS' >&2; exit 1; }
for cmd in node npm swiftc; do command -v "$cmd" >/dev/null || { echo "Missing $cmd" >&2; exit 1; }; done
cd "$(dirname "$0")/.."
npm ci
npm run build
app="$HOME/Applications/Voxa.app"
if pgrep -f "^$app/Contents/MacOS/Voxa$" >/dev/null; then echo 'Quit Voxa from the menu bar before reinstalling.' >&2; exit 1; fi
resources="$app/Contents/Resources"
mkdir -p "$resources/dist" "$HOME/.local/bin" "$HOME/Library/Logs" "${XDG_CONFIG_HOME:-$HOME/.config}/voxa" "$app/Contents/MacOS"
# Stop the obsolete launchd daemon; it cannot capture microphone audio.
plist="$HOME/Library/LaunchAgents/com.voxa.daemon.plist"
if [[ -f "$plist" ]]; then
  launchctl bootout "gui/$(id -u)" "$plist" 2>/dev/null || true
  rm "$plist"
fi
cp dist/*.js "$resources/dist/"
rm -rf "$resources/node_modules"
cp -R node_modules "$resources/"
rm -f "$resources/bin/voxa-keys" # The app itself now owns the shortcuts and Accessibility grant.
swiftc -O mac/Voxa.swift -framework AppKit -framework AVFoundation -framework ApplicationServices -framework ServiceManagement -o "$app/Contents/MacOS/Voxa"
printf '%s\n' "$(command -v node)" > "$resources/node-path"
/usr/libexec/PlistBuddy -c Clear "$app/Contents/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c 'Add :CFBundleIdentifier string com.voxa.app' "$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Add :CFBundleName string Voxa' "$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Add :CFBundleExecutable string Voxa' "$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Add :CFBundlePackageType string APPL' "$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Add :CFBundleVersion string 1' "$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Add :NSMicrophoneUsageDescription string Voxa records audio only while a recording shortcut is active, to transcribe speech.' "$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Add :LSUIElement bool true' "$app/Contents/Info.plist"
# Sign the app bundle so TCC associates microphone and Accessibility with Voxa.
codesign --force --deep --sign - "$app"
ln -sf "$resources/dist/index.js" "$HOME/.local/bin/voxa"
chmod +x "$resources/dist/index.js"
config="${XDG_CONFIG_HOME:-$HOME/.config}/voxa"
if [[ ! -f "$config/config.json" ]]; then cp config.example.json "$config/config.json"; chmod 600 "$config/config.json"; fi
if [[ ! -f "$config/env" ]]; then printf 'ELEVENLABS_API_KEY=\n' > "$config/env"; chmod 600 "$config/env"; fi
open "$app"
echo 'Voxa.app opened and registers itself for login. Grant Microphone and Accessibility to Voxa.app, then use Enable / Retry Shortcuts and Test microphone (peak > 0) from the menu bar.'
