import { createServer, connect } from 'node:net';
import { existsSync, lstatSync, mkdirSync, unlinkSync, chmodSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { spawn, type ChildProcess } from 'node:child_process';
import { capture, audioPeak, type Capture } from './audio.js';
import { Scribe } from './scribe.js';
import { paste } from './paste.js';
import { Osd } from './osd.js';
import { loadConfig, loadKeyFile, type Config } from './config.js';

import { runtimeDir, socketPath } from './runtime.js';
export { runtimeDir, socketPath } from './runtime.js';
type State = 'idle' | 'recording' | 'committing';
export class Daemon {
  state: State = 'idle';
  private config: Config;
  private mic?: Capture;
  private scribe?: Scribe;
  private serial = 0;
  private recordingLimit?: NodeJS.Timeout;
  private recordingStarted = 0;
  private audioBytes = 0;
  private peak = 0;
  private osd = new Osd();
  private keys?: ChildProcess;
  constructor() { this.config = loadConfig(); }
  private log(message: string) { console.log(`[voxa] ${message}`); }
  private error(e: Error) {
    let message = e.message;
    for (const key of (process.env.ELEVENLABS_API_KEY ?? '').split(',').map(x => x.trim()).filter(Boolean)) message = message.replaceAll(key, '[redacted]');
    console.error(`[voxa] ${message}`);
  }
  start() {
    if (this.state !== 'idle') return;
    try {
      if (process.env.VOXA_MAC_APP === '1') loadKeyFile(true);
      this.config = loadConfig();
      const id = ++this.serial;
      this.scribe = new Scribe(this.config, e => { if (id === this.serial) this.fail(e); });
      this.state = 'recording';
      this.recordingStarted = performance.now();
      this.audioBytes = 0;
      this.peak = 0;
      this.mic = capture(this.config.audioDevice, b => { if (id === this.serial && (this.state === 'recording' || this.state === 'committing')) { this.audioBytes += b.length; this.peak = Math.max(this.peak, audioPeak(b)); this.scribe?.add(b); } }, e => { if (id === this.serial && this.state === 'recording') this.fail(e); });
      this.recordingLimit = setTimeout(() => {
        if (id === this.serial && this.state === 'recording') this.fail(Error('recording exceeded 60 seconds; stopped for safety'));
      }, 60_000);
      this.osd.show('recording');
      this.log('recording started');
    } catch (e) { this.fail(e as Error); }
  }
  toggle() {
    if (this.state === 'idle') this.start();
    else if (this.state === 'recording') this.stop();
    // Ignore presses while committing; don't start another recording mid-paste.
  }
  stop() {
    if (this.state !== 'recording') return;
    const id = this.serial;
    this.state = 'committing';
    clearTimeout(this.recordingLimit);
    this.recordingLimit = undefined;
    const mic = this.mic;
    mic?.stop(); this.mic = undefined;
    this.osd.show('committing');
    const duration = Math.round(performance.now() - this.recordingStarted);
    const start = performance.now();
    // Drain PipeWire's last stdout frames after stopping capture before committing.
    void (async () => {
      if (mic) await Promise.race([mic.waitClose(), new Promise<void>(resolve => setTimeout(resolve, 250))]);
      if (id !== this.serial) return;
      this.log(`recording stopped after ${duration}ms; captured ${this.audioBytes} audio bytes; peak ${this.peak}; committing...`);
      const text = await this.scribe!.stop();
      if (id !== this.serial) return;
      this.log(`transcript received in ${Math.round(performance.now() - start)}ms`);
      if (this.config.debug) this.log(`debug transcript: ${text}`);
      if (text.trim()) { await paste(text); this.osd.show('done'); this.log(`pasted ${text.length} chars`); }
      else { this.osd.hide(); this.log('empty transcript; nothing pasted'); }
      if (id === this.serial) this.state = 'idle';
    })().catch(e => { if (id === this.serial) this.fail(e as Error); });
  }
  private fail(e: Error) {
    this.error(e); ++this.serial;
    clearTimeout(this.recordingLimit);
    this.recordingLimit = undefined;
    this.osd.show('error');
    this.mic?.stop(); this.mic = undefined;
    this.scribe?.abort(); this.scribe = undefined;
    this.state = 'idle';
  }
  serve() {
    const path = socketPath();
    mkdirSync(runtimeDir(), { recursive: true, mode: 0o700 });
    if (existsSync(path)) {
      if (!lstatSync(path).isSocket()) throw Error(`${path} exists and is not a socket`);
      // Refuse to unlink a socket still owned by a live daemon.
      const probe = connect(path);
      probe.on('connect', () => { console.error('[voxa] daemon already running'); process.exit(1); });
      probe.on('error', () => this.listen(path));
    } else this.listen(path);
  }
  private listen(path: string) {
    if (existsSync(path)) unlinkSync(path);
    const server = createServer(sock => {
      sock.setTimeout(1000, () => sock.destroy());
      sock.once('data', data => {
        const command = data.toString().trim();
        if (command === 'start') this.start();
        else if (command === 'stop') this.stop();
        else if (command === 'toggle') this.toggle();
        else if (command !== 'status') { sock.end('unknown command\n'); return; }
        sock.end(this.state + '\n');
      });
    });
    server.listen(path, () => {
      chmodSync(path, 0o600); this.log('ready');
      if (process.platform === 'darwin' && process.env.VOXA_MAC_APP !== '1') {
        const helper = fileURLToPath(new URL('../bin/voxa-keys', import.meta.url));
        const cli = fileURLToPath(new URL('./index.js', import.meta.url));
        this.keys = spawn(helper, [process.execPath, cli], { stdio: ['ignore', 'ignore', 'inherit'] });
        this.keys.on('error', e => this.error(Error(`keyboard helper: ${e.message}`)));
        this.keys.on('exit', (code, signal) => { if (code !== 0 && signal !== 'SIGTERM') this.error(Error(`keyboard helper exited ${code ?? signal}`)); });
      }
    });
    process.on('SIGTERM', () => { clearTimeout(this.recordingLimit); this.keys?.kill(); this.osd.hide(); this.mic?.stop(); this.scribe?.abort(); server.close(); try { unlinkSync(path); } catch {} process.exit(0); });
  }
}
