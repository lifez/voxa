#!/usr/bin/env node
import { connect } from 'node:net';
import { readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { Daemon, socketPath } from './daemon.js';
import { capture } from './audio.js';
import { paste } from './paste.js';
import { Scribe } from './scribe.js';
import { loadConfig } from './config.js';
import { setup, settings, doctor } from './setup.js';

function loadKeyFile() {
  if (process.env.ELEVENLABS_API_KEY) return;
  try {
    const env = readFileSync(join(process.env.XDG_CONFIG_HOME || join(homedir(), '.config'), 'voxa/env'), 'utf8');
    const line = env.split(/\r?\n/).find(s => /^ELEVENLABS_API_KEY=/.test(s));
    if (line) process.env.ELEVENLABS_API_KEY = line.slice('ELEVENLABS_API_KEY='.length).replace(/^(['"])(.*)\1$/, '$2');
  } catch (e) { if ((e as NodeJS.ErrnoException).code !== 'ENOENT') throw e; }
}

const [command, ...args] = process.argv.slice(2);
if (command === 'setup') setup().catch(e => { console.error(`voxa: ${e.message}`); process.exitCode = 1; });
else if (command === 'settings') settings(args[0] !== '--terminal').catch(e => { console.error(`voxa: ${e.message}`); process.exitCode = 1; });
else if (command === 'doctor') doctor().catch(e => { console.error(`voxa: ${e.message}`); process.exitCode = 1; });
else if (command === 'daemon') { loadKeyFile(); new Daemon().serve(); }
else if (command === 'test-mic') {
  let bytes = 0;
  const mic = capture(loadConfig().audioDevice, b => { bytes += b.length; }, e => { console.error(e.message); process.exitCode = 1; });
  setTimeout(() => { mic.kill('SIGTERM'); console.log(`Captured ${bytes} bytes of 16k mono PCM in 2 seconds`); if (!bytes) process.exitCode = 1; }, 2000);
} else if (command === 'test-scribe') {
  try {
    loadKeyFile();
    const scribe = new Scribe(loadConfig(), e => console.error(e.message));
    const mic = capture(loadConfig().audioDevice, b => scribe.add(b), e => console.error(e.message));
    console.log('Speak now (3 seconds)...');
    setTimeout(async () => {
      mic.kill('SIGTERM');
      await new Promise(resolve => mic.once('close', resolve));
      try { console.log('Transcript:', await scribe.stop()); }
      catch (e) { console.error((e as Error).message); process.exitCode = 1; }
    }, 3000);
  } catch (e) { console.error((e as Error).message); process.exitCode = 1; }
} else if (command === 'test-paste') {
  paste(args.join(' ')).catch(e => { console.error(e.message); process.exitCode = 1; });
} else if (['start', 'stop', 'toggle', 'status'].includes(command ?? '')) {
  const sock = connect(socketPath());
  sock.setTimeout(1500, () => sock.destroy(Error('daemon timed out')));
  sock.on('connect', () => sock.end(command + '\n'));
  sock.on('data', b => process.stdout.write(b));
  sock.on('error', e => { console.error(`voxa: ${e.message} (is the user service running?)`); process.exitCode = 1; });
} else { console.error('Usage: voxa setup|settings [--terminal]|doctor|daemon|start|stop|toggle|status|test-mic|test-scribe|test-paste TEXT'); process.exitCode = 2; }
