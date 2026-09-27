import { spawn } from 'node:child_process';

// Best-effort Omarchy shell IPC. Never await from audio or paste paths.
export class Osd {
  private queue: Promise<void> = Promise.resolve();
  show(state: 'recording' | 'committing' | 'done' | 'error') {
    if (process.platform === 'darwin') {
      if (state === 'done') return;
      const message = state === 'recording' ? 'Recording' : state === 'committing' ? 'Transcribing' : 'Error — check ~/Library/Logs/voxa-error.log';
      spawn('osascript', ['-e', `display notification ${JSON.stringify(message)} with title "Voxa"`], { stdio: 'ignore' }).on('error', () => {});
      return;
    }
    this.send(['-q', 'shell', 'summon', 'voxa.osd', JSON.stringify({ state })]);
  }
  hide() { if (process.platform !== 'darwin') this.send(['-q', 'shell', 'hide', 'voxa.osd']); }
  private send(args: string[]) {
    this.queue = this.queue.then(() => new Promise<void>(resolve => {
      const child = spawn('omarchy-shell', args, { stdio: 'ignore', timeout: 1500 });
      child.on('error', () => resolve()); // optional on non-Omarchy desktops
      child.on('close', () => resolve());
    })).catch(() => {});
  }
}
