# Upgrading to C-only Voxa

Run `bash scripts/install.sh` from the current checkout on Linux. It builds the C binary directly; no Node.js, npm or previous installation is required. Existing installations must be idle. The script does not uninstall a system-wide Node.js package used by other projects.

## What changes

- `~/.local/share/voxa/voxa`: native daemon and CLI.
- `~/.local/bin/voxa`: symlink to that binary (replaces the old router/symlink).
- `voxa.service`: starts C directly, without importing keys into its environment.
- `voxa.service.d/80-config.conf`: passes the selected `XDG_CONFIG_HOME`.
- The old `90-native.conf` opt-in override is removed.
- Old application `dist`, `node_modules`, package manifests, `native` and `ui` directories move into a private backup outside the application directory.
- Config, API keys, custom service overrides and existing shortcuts are preserved. Key files reload on each recording unless an explicit environment key overrides them.

The installer prints a backup under `~/.local/share/voxa-backups/install-*`. On installation failure it attempts to restore the old launcher, binary, service and app directories, and restart a previously active service. Keep backups until you have verified actual dictation. A service/status check alone does not establish transcription quality.

`voxa setup`, `settings`, `doctor`, `test-mic`, `test-scribe` and `test-paste` are now C commands. Settings are terminal-only; the former Zenity/GTK shortcut recorder is no longer provided. The setup wizard can update a managed bindings block, but refuses conflicting custom or legacy bindings instead of silently rewriting them.

`scripts/use-native.sh` is now a compatibility alias for installation, not a separate opt-in migration. Its old `--rollback` mode is retired.

## Recovering a legacy opt-in backup

For a backup created by the old `use-native.sh`, use the matching script from the commit that created it (for example `3b9eeb3`), not the new alias:

```sh
# Stop dictation first. Review the historical script before running it.
git show 3b9eeb3:scripts/use-native.sh > /tmp/voxa-legacy-rollback.sh
bash /tmp/voxa-legacy-rollback.sh --rollback /absolute/path/to/backup-before-native-TIMESTAMP.SUFFIX
```

That script depends on the old application files still being installed. If the C-only installer has already moved them, restore those files from its `install-*` backup first. Do not delete backups or keys as part of recovery.

## Manual recovery from a new installer backup

Stop `voxa.service`. Restore `launcher` to `~/.local/bin/voxa`, `voxa` to `~/.local/share/voxa/voxa`, and `voxa.service` to your user systemd directory when those files exist in the backup. Restore the saved `80-config.conf` and `90-native.conf` overrides, removing the corresponding installed override when it was absent from the backup. Move any saved application directories/manifests back into `~/.local/share/voxa`.

Then run `systemctl --user daemon-reload`, restart the service, and verify `voxa status`. Preserve symlinks when copying the launcher (`cp -a`). Do not overwrite unrelated service overrides, user config or key files.

## macOS

Quit Voxa before running `scripts/install-macos.sh`. It builds the C daemon and Swift app/helpers, replaces bundled legacy runtime files, and points the CLI at `Resources/bin/voxa`. Back up the existing app bundle before upgrading: this installer does not provide transactional rollback. The C/macOS implementation still needs a build and live validation on macOS.
