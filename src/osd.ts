import { spawn } from 'node:child_process';

// Best-effort Omarchy shell IPC. Never await from audio or paste paths.
export class Osd {
  private queue: Promise<void> = Promise.resolve();
  show(state: 'recording' | 'committing' | 'done' | 'error') {
    this.send(['-q', 'shell', 'summon', 'voxa.osd', JSON.stringify({ state })]);
  }
  hide() { this.send(['-q', 'shell', 'hide', 'voxa.osd']); }
  private send(args: string[]) {
    if (process.platform === 'darwin') return; // Omarchy OSD is Linux-only.
    this.queue = this.queue.then(() => new Promise<void>(resolve => {
      const child = spawn('omarchy-shell', args, { stdio: 'ignore', timeout: 1500 });
      child.on('error', () => resolve()); // optional on non-Omarchy desktops
      child.on('close', () => resolve());
    })).catch(() => {});
  }
}
