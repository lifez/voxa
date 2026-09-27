import { test } from 'node:test';
import { strict as assert } from 'node:assert';
import { shortcut, updateBindings, saveKey, hasKey } from '../dist/setup.js';
import { mkdtempSync, readFileSync, statSync, rmSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

test('validates shortcuts and rejects duplicate or unsafe values', () => {
  assert.equal(shortcut('super + shift + r'), 'SUPER + SHIFT + R');
  assert.equal(shortcut('XF86AudioMute'), 'XF86AudioMute');
  for (const value of ['F10"', 'ALT + ALT + K', 'CTRL', 'F10\nfoo', '']) assert.throws(() => shortcut(value));
  assert.throws(() => updateBindings('', 'F10', 'F10'), /differ/);
});
test('writes managed block, updates it idempotently, preserves other bindings', () => {
  const initial = '-- user\no.bind("SUPER + X", "other", "true")\n';
  const first = updateBindings(initial, 'F10', 'F11');
  assert.match(first, /BEGIN VOXA/);
  assert.equal(updateBindings(first, 'F10', 'F11'), first);
  const next = updateBindings(first, 'SUPER + R', 'F12');
  assert.match(next, /SUPER \+ R/);
  assert.doesNotMatch(next, /Start Voxa \(hold\)".*F10/);
  assert.match(next, /SUPER \+ X/);
});
test('stores keys privately without exposing them in config', () => {
  const dir = mkdtempSync(join(tmpdir(), 'voxa-test-'));
  const previous = process.env.XDG_CONFIG_HOME;
  try {
    process.env.XDG_CONFIG_HOME = dir;
    assert.throws(() => saveKey('bad\nINJECT=1'), /invalid API key/);
    saveKey('test_key-123');
    assert.equal(hasKey(), true);
    assert.equal(readFileSync(join(dir, 'voxa/env'), 'utf8'), 'ELEVENLABS_API_KEY=test_key-123\n');
    assert.equal(statSync(join(dir, 'voxa/env')).mode & 0o077, 0);
  } finally {
    if (previous === undefined) delete process.env.XDG_CONFIG_HOME;
    else process.env.XDG_CONFIG_HOME = previous;
    rmSync(dir, { recursive: true, force: true });
  }
});
test('GTK shortcut recorder formats keys and ignores modifier-only presses', (t) => {
  const python = spawnSync('python3', ['-c', `import importlib.util
spec = importlib.util.spec_from_file_location('recorder', 'ui/shortcut.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
Gdk = m.Gdk
assert m.format_key(Gdk.KEY_F10, Gdk.ModifierType(0)) == 'F10'
assert m.format_key(Gdk.KEY_r, Gdk.ModifierType.SUPER_MASK | Gdk.ModifierType.SHIFT_MASK) == 'SUPER + SHIFT + R'
assert m.format_key(Gdk.KEY_Control_L, Gdk.ModifierType.CONTROL_MASK) is None
`], { encoding: 'utf8' });
  if (python.error?.code === 'ENOENT' || /No module named 'gi'/.test(python.stderr)) t.skip('GTK/PyGObject not installed');
  else assert.equal(python.status, 0, python.stderr);
});
test('settings menu edits language without asking for or saving an API key', () => {
  const dir = mkdtempSync(join(tmpdir(), 'voxa-menu-'));
  try {
    mkdirSync(join(dir, 'hypr'));
    writeFileSync(join(dir, 'hypr/bindings.lua'), updateBindings('-- o.bind("F10", "Start Voxa (hold)", "~/.local/bin/voxa start")\n', 'F8', 'SUPER + R'));
    const menu = join(dir, 'zenity');
    writeFileSync(menu, `#!/bin/sh
state="$XDG_CONFIG_HOME/menu-state"
printf '%s\\n' "$@" >> "$XDG_CONFIG_HOME/zenity-args"
count=$(wc -l < "$state" 2>/dev/null || echo 0)
echo x >> "$state"
case "$count" in
  0) printf 'Language\\n' ;;
  1) printf 'en\\n' ;;
  2) printf 'th\\n' ;;
  *) exit 1 ;;
esac
`, { mode: 0o755 });
    const script = `import { settings } from './dist/setup.js'; await settings(true);`;
    const result = spawnSync(process.execPath, ['--input-type=module', '-e', script], {
      env: { ...process.env, PATH: `${dir}:${process.env.PATH}`, XDG_CONFIG_HOME: dir }, encoding: 'utf8', timeout: 10000
    });
    assert.equal(result.status, 0, result.stderr);
    assert.match(result.stdout, /Config saved/);
    assert.equal(JSON.parse(readFileSync(join(dir, 'voxa/config.json'), 'utf8')).language, 'en');
    assert.throws(() => readFileSync(join(dir, 'voxa/env')), /ENOENT/);
    const args = readFileSync(join(dir, 'zenity-args'), 'utf8');
    assert.match(args, /--column=Current/);
    assert.match(args, /Hold shortcut\nF8\nToggle shortcut\nSUPER \+ R/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});
test('settings can enable stop punctuation without changing other config', () => {
  const dir = mkdtempSync(join(tmpdir(), 'voxa-punctuation-'));
  try {
    writeFileSync(join(dir, 'zenity'), `#!/bin/sh
case "$*" in
  *--list*) if [ -f "$XDG_CONFIG_HOME/selected" ]; then exit 1; fi; touch "$XDG_CONFIG_HOME/selected"; printf 'Stop punctuation\\n' ;;
  *--entry*) printf 'yes\\n' ;;
esac
`, { mode: 0o755 });
    const result = spawnSync(process.execPath, ['--input-type=module', '-e', `import { settings } from './dist/setup.js'; await settings(true);`], {
      env: { ...process.env, PATH: `${dir}:${process.env.PATH}`, XDG_CONFIG_HOME: dir }, encoding: 'utf8', timeout: 10000
    });
    assert.equal(result.status, 0, result.stderr);
    const config = JSON.parse(readFileSync(join(dir, 'voxa/config.json'), 'utf8'));
    assert.equal(config.stopPunctuation, true);
    assert.equal(config.language, 'th');
  } finally { rmSync(dir, { recursive: true, force: true }); }
});
test('settings edits each shortcut independently', () => {
  const dir = mkdtempSync(join(tmpdir(), 'voxa-shortcuts-'));
  try {
    mkdirSync(join(dir, 'hypr'));
    const bindings = join(dir, 'hypr/bindings.lua');
    writeFileSync(bindings, updateBindings('', 'F10', 'F11'));
    writeFileSync(join(dir, 'zenity'), `#!/bin/sh
case "$*" in
  *--list*) if [ -f "$XDG_CONFIG_HOME/selected" ]; then exit 1; fi; touch "$XDG_CONFIG_HOME/selected"; printf '%s\\n' "$VOXA_SECTION" ;;
  *) exit 1 ;;
esac
`, { mode: 0o755 });
    writeFileSync(join(dir, 'python3'), `#!/bin/sh
printf '%s\\n' "$VOXA_SHORTCUT"
`, { mode: 0o755 });
    const runSettings = (section, value) => {
      rmSync(join(dir, 'selected'), { force: true });
      return spawnSync(process.execPath, ['--input-type=module', '-e', `import { settings } from './dist/setup.js'; await settings(true);`], {
        env: { ...process.env, PATH: `${dir}:${process.env.PATH}`, XDG_CONFIG_HOME: dir, VOXA_SECTION: section, VOXA_SHORTCUT: value },
        encoding: 'utf8', timeout: 10000
      });
    };
    let result = runSettings('Toggle shortcut', 'SUPER + T');
    assert.equal(result.status, 0, result.stderr);
    assert.match(result.stdout, /Toggle shortcut saved/);
    assert.match(readFileSync(bindings, 'utf8'), /o.bind\("F10", "Start Voxa/);
    assert.match(readFileSync(bindings, 'utf8'), /o.bind\("SUPER \+ T", "Toggle Voxa/);
    result = runSettings('Hold shortcut', 'F8');
    assert.equal(result.status, 0, result.stderr);
    assert.match(result.stdout, /Hold shortcut saved/);
    assert.match(readFileSync(bindings, 'utf8'), /o.bind\("F8", "Start Voxa/);
    assert.match(readFileSync(bindings, 'utf8'), /o.bind\("F8", "Stop Voxa/);
    assert.match(readFileSync(bindings, 'utf8'), /o.bind\("SUPER \+ T", "Toggle Voxa/);
    const before = readFileSync(bindings, 'utf8');
    result = runSettings('Hold shortcut', 'SUPER + T');
    assert.match(result.stderr, /must differ/);
    assert.equal(readFileSync(bindings, 'utf8'), before);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});
test('migrates legacy bindings, refuses conflicting or malformed blocks', () => {
  const old = 'o.bind("F10", "Start Voxa (hold)", "~/.local/bin/voxa start")\no.bind("F10", "Stop Voxa (release)", "~/.local/bin/voxa stop", { release = true })\no.bind("F11", "Toggle Voxa", "~/.local/bin/voxa toggle")\n';
  const result = updateBindings(old, 'F8', 'F9');
  assert.equal((result.match(/voxa start/g) || []).length, 1);
  assert.throws(() => updateBindings('o.bind("F8", "mine", "true")', 'F8', 'F9'), /conflicting/);
  assert.throws(() => updateBindings('-- BEGIN VOXA (managed by voxa settings)', 'F8', 'F9'), /invalid Voxa/);
});
