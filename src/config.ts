import { readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

export type Config = { language: string | null; secondaryLanguages: string[]; audioDevice: string; keyterms: string[]; debug: boolean; pasteCommand: string; stopPunctuation: boolean };
export const defaults: Config = { language: 'th', secondaryLanguages: ['en'], audioDevice: 'default', keyterms: [], debug: false, pasteCommand: 'wtype', stopPunctuation: false };
export function validate(value: unknown): Config {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw Error('config must be an object');
  const obj = value as Record<string, unknown>;
  for (const k of Object.keys(obj)) if (!(k in defaults)) throw Error(`unknown config field: ${k}`);
  const c = { ...defaults, ...obj } as Config;
  if (!(c.language === null || (typeof c.language === 'string' && /^[a-z]{2,3}$/.test(c.language)))) throw Error('language must be ISO language code or null');
  for (const [name, a] of [['secondaryLanguages', c.secondaryLanguages], ['keyterms', c.keyterms]] as const) {
    if (!Array.isArray(a) || !a.every(x => typeof x === 'string')) throw Error(`${name} must be string array`);
  }
  if (typeof c.audioDevice !== 'string' || !c.audioDevice || typeof c.debug !== 'boolean' || c.pasteCommand !== 'wtype' || typeof c.stopPunctuation !== 'boolean') throw Error('invalid audioDevice, debug, pasteCommand or stopPunctuation');
  return c;
}
export function loadConfig(): Config {
  try { return validate(JSON.parse(readFileSync(join(process.env.XDG_CONFIG_HOME || join(homedir(), '.config'), 'voxa/config.json'), 'utf8'))); }
  catch (e) { if ((e as NodeJS.ErrnoException).code === 'ENOENT') return { ...defaults }; throw e; }
}
