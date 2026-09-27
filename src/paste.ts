import { spawn } from 'node:child_process';

function run(cmd: string, args: string[], input?: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, args, { stdio: [input === undefined ? 'ignore' : 'pipe', 'ignore', 'pipe'] });
    let error = '';
    p.stderr?.on('data', b => { error = (error + String(b)).slice(-1000); });
    p.on('error', reject);
    // wl-copy forks a clipboard owner which inherits stderr; don't wait for its pipe to close.
    p.on('exit', code => { p.stderr?.destroy(); code === 0 ? resolve() : reject(Error(`${cmd} exited ${code}: ${error}`)); });
    if (input !== undefined) p.stdin?.end(input, 'utf8');
  });
}
export async function paste(text: string): Promise<void> {
  if (!text.trim()) return;
  if (process.platform === 'darwin') {
    await run('pbcopy', [], text);
    await run('osascript', ['-e', 'tell application "System Events" to keystroke "v" using command down']);
  } else {
    await run('wl-copy', ['--type', 'text/plain;charset=utf-8'], text);
    await run('wtype', ['-M', 'ctrl', 'v', '-m', 'ctrl']);
  }
}
