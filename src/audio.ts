import { spawn } from 'node:child_process';
import { connect } from 'node:net';
import { join } from 'node:path';
import { homedir } from 'node:os';

export type Capture = { stop(): void; waitClose(): Promise<void> };

export function audioPeak(data: Buffer): number {
  let peak = 0;
  for (let i = 0; i + 1 < data.length; i += 2) peak = Math.max(peak, Math.abs(data.readInt16LE(i)));
  return peak;
}

export function capture(device: string, onData: (data: Buffer) => void, onFailure: (error: Error) => void): Capture {
  if (process.platform === 'darwin' && process.env.VOXA_MAC_APP === '1') {
    if (device !== 'default') throw Error('Voxa.app currently supports the system default microphone only');
    const mic = connect(join(homedir(), 'Library/Caches/voxa/mic.sock'));
    let stopping = false, closed = false;
    mic.on('close', () => { closed = true; });
    mic.on('data', onData);
    mic.on('error', e => { if (!stopping) onFailure(e); });
    mic.on('end', () => { if (!stopping) onFailure(Error('Voxa.app microphone disconnected')); });
    return { stop() { stopping = true; mic.end(); }, waitClose: () => closed ? Promise.resolve() : new Promise(resolve => mic.once('close', resolve)) };
  }
  const mac = process.platform === 'darwin';
  const args = mac
    ? ['-hide_banner', '-loglevel', 'error', '-f', 'avfoundation', '-i', `:${device === 'default' ? 'default' : device}`, '-ac', '1', '-ar', '16000', '-f', 's16le', 'pipe:1']
    : ['--rate', '16000', '--channels', '1', '--format', 's16', '--raw', '--latency', '100ms', ...(device === 'default' ? [] : ['--target', device]), '-'];
  const command = mac ? 'ffmpeg' : 'pw-record';
  const child = spawn(command, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  let stderr = '';
  child.stderr.on('data', b => { stderr = (stderr + String(b)).slice(-1000); });
  child.stdout.on('data', onData);
  child.on('error', onFailure);
  child.on('exit', (code, signal) => { if (!child.killed) onFailure(Error(`${command} exited ${code ?? signal}: ${stderr}`)); });
  return { stop() { child.kill('SIGTERM'); }, waitClose: () => child.exitCode !== null || child.signalCode !== null ? Promise.resolve() : new Promise(resolve => child.once('close', resolve)) };
}
