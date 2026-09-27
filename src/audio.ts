import { spawn, type ChildProcessByStdio } from 'node:child_process';
import type { Readable } from 'node:stream';

export function capture(device: string, onData: (data: Buffer) => void, onFailure: (error: Error) => void): ChildProcessByStdio<null, Readable, Readable> {
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
  return child;
}
