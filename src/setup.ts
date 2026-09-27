import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync, renameSync, chmodSync, copyFileSync, unlinkSync, statSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { createInterface } from 'node:readline/promises';
import { stdin, stdout } from 'node:process';
import { fileURLToPath } from 'node:url';
import { loadConfig, validate } from './config.js';

export const configDir = () => join(process.env.XDG_CONFIG_HOME || join(homedir(), '.config'), 'voxa');
const bindingsPath = () => join(process.env.XDG_CONFIG_HOME || join(homedir(), '.config'), 'hypr', 'bindings.lua');
const begin = '-- BEGIN VOXA (managed by voxa settings)';
const end = '-- END VOXA';
const keyPattern = /^(?:(?:SUPER|CTRL|ALT|SHIFT) \+ )*(?:[A-Za-z0-9_]+)$/;

export function shortcut(value: string): string {
  const parts = value.trim().split(/\s*\+\s*/);
  const mods = parts.slice(0, -1).map(m => m.toUpperCase());
  const key = parts.at(-1)!;
  const normalized = [...mods, key.startsWith('XF86') ? key : key.toUpperCase()];
  if (!parts.every(Boolean) || new Set(mods).size !== mods.length || !mods.every(m => ['SUPER', 'CTRL', 'ALT', 'SHIFT'].includes(m)) || !keyPattern.test(normalized.join(' + ')) || ['SUPER', 'CTRL', 'ALT', 'SHIFT'].includes(key.toUpperCase())) throw Error(`invalid shortcut: ${value}`);
  return normalized.join(' + ');
}
export function updateBindings(source: string, hold: string, toggle: string): string {
  hold = shortcut(hold); toggle = shortcut(toggle);
  if (hold === toggle) throw Error('hold and toggle shortcuts must differ');
  const start = source.indexOf(begin), finish = source.indexOf(end);
  if ((start < 0) !== (finish < 0) || (start >= 0 && finish < start) || source.indexOf(begin, start + begin.length) !== -1) throw Error('invalid Voxa managed block; fix bindings.lua manually');
  let outside = start < 0 ? source : source.slice(0, start) + source.slice(finish + end.length);
  // Migrate only the exact three lines shipped by the old installer; leave other customizations alone.
  const legacy = ['o.bind("F10", "Start Voxa (hold)", "~/.local/bin/voxa start")', 'o.bind("F10", "Stop Voxa (release)", "~/.local/bin/voxa stop", { release = true })', 'o.bind("F11", "Toggle Voxa", "~/.local/bin/voxa toggle")'];
  if (legacy.every(line => outside.split('\n').includes(line))) outside = outside.split('\n').filter(line => !legacy.includes(line)).join('\n');
  // Never silently change somebody else's bindings.
  for (const line of outside.split('\n')) {
    if (/^\s*--/.test(line)) continue;
    if (/o\.bind\s*\(/.test(line) && (/voxa\s+(?:start|stop|toggle)/.test(line) || [hold, toggle].some(k => line.includes(`"${k}"`) || line.includes(`'${k}'`)))) throw Error('conflicting binding in bindings.lua; remove old Voxa/duplicate lines manually first');
  }
  const block = `${begin}\no.bind("${hold}", "Start Voxa (hold)", "~/.local/bin/voxa start")\no.bind("${hold}", "Stop Voxa (release)", "~/.local/bin/voxa stop", { release = true })\no.bind("${toggle}", "Toggle Voxa", "~/.local/bin/voxa toggle")\n${end}`;
  return outside.trimEnd() + '\n\n' + block + '\n';
}
function run(command: string, args: string[], timeout = 10000) { return spawnSync(command, args, { encoding: 'utf8', timeout }); }
function available(command: string) { return run('sh', ['-c', `command -v ${command} >/dev/null`]).status === 0; }
function save(path: string, content: string, mode: number) {
  mkdirSync(join(path, '..'), { recursive: true });
  const temp = `${path}.${process.pid}.tmp`;
  try { writeFileSync(temp, content, { mode, flag: 'wx' }); chmodSync(temp, mode); renameSync(temp, path); }
  catch (e) { try { unlinkSync(temp); } catch {} throw e; }
}
export function saveKey(key: string) {
  const keys = key.split(',').map(x => x.trim());
  if (!keys.length || keys.length > 5 || !keys.every(x => /^[A-Za-z0-9._~-]+$/.test(x))) throw Error('invalid API key (enter 1–5 keys, separated by commas)');
  save(join(configDir(), 'env'), `ELEVENLABS_API_KEY=${keys.join(',')}\n`, 0o600);
}
export function hasKey() {
  try { return /^ELEVENLABS_API_KEY=.+$/m.test(readFileSync(join(configDir(), 'env'), 'utf8')); } catch { return false; }
}
export function saveBindings(hold: string, toggle: string) {
  const path = bindingsPath();
  if (!existsSync(path)) throw Error(`${path} not found; install/configure Omarchy first`);
  const old = readFileSync(path, 'utf8');
  const next = updateBindings(old, hold, toggle);
  if (next === old) return;
  copyFileSync(path, `${path}.voxa.bak`);
  save(path, next, 0o644);
  if (available('hyprctl') && process.env.HYPRLAND_INSTANCE_SIGNATURE) {
    const reload = run('hyprctl', ['reload']);
    const errors = run('hyprctl', ['configerrors']);
    if (reload.status !== 0 || errors.status !== 0 || errors.stdout.trim()) {
      save(path, old, 0o644);
      run('hyprctl', ['reload']);
      throw Error(`Hyprland rejected bindings; restored backup: ${errors.stdout || errors.stderr || reload.stderr}`);
    }
  }
}
export function existingShortcuts(): [string, string] {
  const text = existsSync(bindingsPath()) ? readFileSync(bindingsPath(), 'utf8') : '';
  const start = text.indexOf(begin), finish = text.indexOf(end, start + begin.length);
  // Prefer our managed block; ignore commented-out bindings from older installs.
  const active = (start >= 0 && finish > start ? text.slice(start, finish) : text)
    .split('\n').filter(line => !/^\s*--/.test(line)).join('\n');
  return [active.match(/o\.bind\("([^"]+)", "Start Voxa \(hold\)"/)?.[1] ?? 'F10', active.match(/o\.bind\("([^"]+)", "Toggle Voxa"/)?.[1] ?? 'F11'];
}
function recordShortcut(title: string, value: string): string | null {
  // Installed layout: share/voxa/dist/setup.js + share/voxa/ui/shortcut.py.
  const script = fileURLToPath(new URL('../ui/shortcut.py', import.meta.url));
  if (available('python3') && existsSync(script)) {
    const result = run('python3', [script, title, value], 300000);
    if (result.status === 0 && result.stdout.trim()) return result.stdout.trim();
    // Cancel does not modify the shortcut; unsupported/missed compositor keys can be typed instead.
  }
  return guiEntry(`${title}: type a shortcut (e.g. SUPER + SHIFT + R), or keep the current value`, value);
}
function guiEntry(title: string, value = '', password = false): string | null {
  const result = run('zenity', ['--entry', `--title=Voxa Settings`, `--text=${title}`, ...(password ? ['--hide-text'] : [`--entry-text=${value}`])], 300000);
  if (result.status !== 0) return null;
  return result.stdout.replace(/\r?\n$/, '');
}
async function terminalEntry(title: string, value = '', secret = false): Promise<string | null> {
  if (secret && stdin.isTTY) {
    stdout.write(`${title} (leave blank to keep current): `);
    const raw = await new Promise<string>(resolve => {
      let answer = '';
      const onData = (chunk: Buffer) => { for (const c of chunk.toString()) { if (c === '\r' || c === '\n') { stdin.off('data', onData); stdin.setRawMode(false); stdout.write('\n'); resolve(answer); return; } if (c === '\u0003') { stdin.setRawMode(false); process.exit(130); } if (c === '\u007f') answer = answer.slice(0, -1); else answer += c; } };
      stdin.setRawMode(true); stdin.on('data', onData); stdin.resume();
    });
    return raw;
  }
  if (secret) throw Error('API key input requires an interactive terminal or GUI');
  const rl = createInterface({ input: stdin, output: stdout });
  try { return await rl.question(`${title}${value ? ` [${value}]` : ''}: `); } finally { rl.close(); }
}
type Ask = (title: string, value?: string, secret?: boolean) => string | null | Promise<string | null>;
type Section = 'API key' | 'Hold shortcut' | 'Toggle shortcut' | 'Language' | 'Microphone' | 'Stop punctuation';
const sections: Section[] = ['API key', 'Hold shortcut', 'Toggle shortcut', 'Language', 'Microphone', 'Stop punctuation'];

async function editKey(ask: Ask) {
  const key = await ask('ElevenLabs API keys — 1–5 keys separated by commas; enable Speech to Text for each (blank = keep existing)', '', true);
  if (key === null) return false;
  if (!key) {
    if (!hasKey()) throw Error('API key is required; enter a key or cancel');
    return true;
  }
  saveKey(key);
  console.log('API key saved.');
  if (process.platform === 'darwin') {
    console.log('Voxa.app reads the API key on the next recording.');
  } else if (available('systemctl') && run('systemctl', ['--user', 'is-active', '--quiet', 'voxa']).status === 0) {
    const result = run('systemctl', ['--user', 'restart', 'voxa']);
    if (result.status !== 0) console.warn('Restart service to apply API key: systemctl --user restart voxa');
  }
  return true;
}
async function editShortcut(which: 'Hold shortcut' | 'Toggle shortcut', ask: Ask, gui: boolean) {
  const [oldHold, oldToggle] = existingShortcuts();
  const isHold = which === 'Hold shortcut';
  const current = isHold ? oldHold : oldToggle;
  const title = isHold ? 'Hold-to-record shortcut (e.g. F10 or SUPER + R)' : 'Toggle shortcut (e.g. F11 or SUPER + T)';
  const value = gui ? recordShortcut(title, current) : await ask(title, current);
  if (value === null) return false;
  saveBindings(isHold ? value || oldHold : oldHold, isHold ? oldToggle : value || oldToggle);
  console.log(`${which} saved.`);
  return true;
}
function saveConfig(config: ReturnType<typeof loadConfig>) {
  save(join(configDir(), 'config.json'), JSON.stringify(validate(config), null, 2) + '\n', 0o600);
  console.log('Config saved; changes apply on next recording.');
}
async function editLanguage(ask: Ask) {
  const c = loadConfig();
  const language = await ask('Primary language (e.g. th; auto for detection)', c.language ?? 'auto');
  if (language === null) return false;
  const secondary = await ask('Secondary languages (comma-separated; blank = none)', c.secondaryLanguages.join(', '));
  if (secondary === null) return false;
  saveConfig({ ...c, language: (language || c.language || 'auto') === 'auto' ? null : language, secondaryLanguages: secondary.split(',').map(x => x.trim()).filter(Boolean) });
  return true;
}
async function editMicrophone(ask: Ask) {
  const c = loadConfig();
  const mic = await ask(process.platform === 'darwin' ? 'Microphone (Voxa.app supports only default)' : 'PipeWire audio device (default = system default)', c.audioDevice);
  if (mic === null) return false;
  if (process.platform === 'darwin' && mic && mic !== 'default') throw Error('Voxa.app supports only the system default microphone');
  saveConfig({ ...c, audioDevice: mic || c.audioDevice });
  return true;
}
async function editStopPunctuation(ask: Ask) {
  const c = loadConfig();
  const answer = await ask('Keep a full stop at the end of transcripts? (yes/no)', c.stopPunctuation ? 'yes' : 'no');
  if (answer === null) return false;
  const value = answer.trim().toLowerCase();
  if (value !== 'yes' && value !== 'no' && value !== '') throw Error('enter yes or no');
  saveConfig({ ...c, stopPunctuation: value === '' ? c.stopPunctuation : value === 'yes' });
  return true;
}
async function editSection(section: Section, ask: Ask, gui: boolean) {
  if (section === 'API key') return editKey(ask);
  if (section === 'Hold shortcut' || section === 'Toggle shortcut') return editShortcut(section, ask, gui);
  if (section === 'Language') return editLanguage(ask);
  if (section === 'Stop punctuation') return editStopPunctuation(ask);
  return editMicrophone(ask);
}
async function chooseSection(gui: boolean): Promise<Section | null> {
  const [hold, toggle] = existingShortcuts();
  if (gui) {
    const result = run('zenity', ['--list', '--title=Voxa Settings', '--text=What would you like to configure?', '--width=560', '--column=Setting', '--column=Current', '--print-column=1',
      'API key', hasKey() ? 'Set' : 'Not set', 'Hold shortcut', hold, 'Toggle shortcut', toggle, 'Language', '', 'Microphone', '', 'Stop punctuation', loadConfig().stopPunctuation ? 'Yes' : 'No'], 300000);
    const choice = result.stdout.trim();
    return result.status === 0 && sections.includes(choice as Section) ? choice as Section : null;
  }
  console.log(`\nVoxa settings: 1) API key (${hasKey() ? 'set' : 'not set'})  2) Hold shortcut (${hold})  3) Toggle shortcut (${toggle})  4) Language  5) Microphone  6) Stop punctuation (${loadConfig().stopPunctuation ? 'yes' : 'no'})  0) Done`);
  const choice = await terminalEntry('Choose a setting', '0');
  if (choice === null || !choice.trim() || choice.trim() === '0') return null;
  const section = sections[Number(choice.trim()) - 1];
  if (!section) { console.log('Invalid choice.'); return chooseSection(gui); }
  return section;
}
export async function settings(gui = false) {
  if (process.platform === 'darwin') {
    console.log('macOS shortcuts are fixed: Command+Shift+R hold, Command+Shift+T toggle.');
    return macSettings();
  }
  if (gui && !available('zenity')) { console.log('zenity not installed; using terminal settings (sudo pacman -S zenity for GUI).'); gui = false; }
  if (!gui && !stdin.isTTY) throw Error('terminal settings require an interactive terminal');
  const ask: Ask = gui ? guiEntry : terminalEntry;
  for (;;) {
    const section = await chooseSection(gui);
    if (!section) break;
    try { await editSection(section, ask, gui); }
    catch (e) {
      const message = (e as Error).message;
      console.error(`voxa: ${message}`);
      if (gui) run('zenity', ['--error', '--title=Voxa Settings', `--text=${message}`], 300000);
    }
  }
}
async function macSettings() {
  if (!stdin.isTTY) throw Error('settings require an interactive terminal');
  const macSections: Section[] = ['API key', 'Language', 'Microphone', 'Stop punctuation'];
  for (;;) {
    console.log('\n1) API key  2) Language  3) Microphone  4) Stop punctuation  0) Done');
    const choice = await terminalEntry('Choose a setting', '0');
    if (!choice || choice.trim() === '0') break;
    const section = macSections[Number(choice.trim()) - 1];
    if (section) { try { await editSection(section, terminalEntry, false); } catch (e) { console.error(`voxa: ${(e as Error).message}`); } }
  }
}
export async function setup() {
  if (process.platform === 'darwin') {
    if (!stdin.isTTY) throw Error('setup requires an interactive terminal');
    for (const section of ['API key', 'Language', 'Microphone', 'Stop punctuation'] as Section[]) {
      if (!await editSection(section, terminalEntry, false)) return;
    }
    console.log('Command+Shift+R: hold to record; Command+Shift+T: toggle. Use Voxa.app menu bar Test microphone to verify peak > 0.');
    await doctor();
    return;
  }
  const gui = available('zenity') && !!(process.env.WAYLAND_DISPLAY || process.env.DISPLAY);
  if (!gui && !stdin.isTTY) throw Error('setup requires an interactive terminal or GUI session');
  const ask: Ask = gui ? guiEntry : terminalEntry;
  for (const section of sections) {
    if (!await editSection(section, ask, gui)) return;
  }
  if (available('systemctl')) {
    const result = run('systemctl', ['--user', 'enable', '--now', 'voxa']);
    if (result.status !== 0) console.warn(`Could not enable service: ${result.stderr.trim()}`);
  }
  await doctor();
}
export async function doctor() {
  let failures = 0;
  const check = (name: string, ok: boolean, fix: string) => { console.log(`${ok ? 'OK' : 'FAIL'} ${name}${ok ? '' : ` — ${fix}`}`); if (!ok) failures++; };
  for (const cmd of (process.platform === 'darwin' ? ['node', 'pbcopy', 'osascript'] : ['node', 'pw-record', 'wl-copy', 'wtype', 'hyprctl'])) check(cmd, available(cmd), 'install required package (see README)');
  check('API key', hasKey(), 'run voxa settings');
  if (hasKey()) check('API key permissions', (statSync(join(configDir(), 'env')).mode & 0o077) === 0, 'chmod 600 ~/.config/voxa/env');
  try { loadConfig(); check('config.json', true, ''); } catch (e) { check('config.json', false, (e as Error).message); }
  if (process.platform === 'darwin') {
    check('Voxa.app', existsSync(join(homedir(), 'Applications/Voxa.app/Contents/MacOS/Voxa')), 'bash scripts/install-macos.sh');
    check('daemon socket', existsSync(join(homedir(), 'Library/Caches/voxa/voxa.sock')), 'check ~/Library/Logs/voxa.log');
    console.log('Use Voxa.app menu bar Test microphone (peak > 0); check Microphone and Accessibility for Voxa.app in System Settings.');
  } else {
    const path = bindingsPath();
    const text = existsSync(path) ? readFileSync(path, 'utf8') : '';
    check('Hyprland bindings', text.includes('voxa start') && text.includes('voxa toggle'), 'run voxa settings; remove old bindings first');
    check('user service', run('systemctl', ['--user', 'is-active', '--quiet', 'voxa']).status === 0, 'systemctl --user enable --now voxa');
    if (available('omarchy-shell')) check('OSD plugin', existsSync(join(process.env.XDG_CONFIG_HOME || join(homedir(), '.config'), 'omarchy/plugins/voxa.osd/manifest.json')), 'rerun scripts/install.sh');
  }
  if (failures) process.exitCode = 1;
  else console.log('Ready. For live checks: voxa test-mic / voxa test-scribe / voxa test-paste TEXT');
}
