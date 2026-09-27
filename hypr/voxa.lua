-- Add all three lines to ~/.config/hypr/bindings.lua (Omarchy Lua).
-- F10: hold to record; release to transcribe. Use a non-modifier key for release.
o.bind("F10", "Start Voxa (hold)", "~/.local/bin/voxa start")
o.bind("F10", "Stop Voxa (release)", "~/.local/bin/voxa stop", { release = true })
-- F11: press once to record; press again to transcribe (no release binding).
o.bind("F11", "Toggle Voxa", "~/.local/bin/voxa toggle")
