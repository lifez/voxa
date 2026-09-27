import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

function run(cmd: string, args: string[], input?: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, args, { stdio: [input === undefined ? 'ignore' : 'pipe', 'ignore', 'pipe'] });
    let error = '';
    p.stderr?.on('data', b => { error = (error + String(b)).slice(-1000); });
    p.on('error', reject);
    p.on('close', code => { code === 0 ? resolve() : reject(Error(`${cmd} exited ${code}: ${error}`)); });
    p.stdin?.on('error', reject);
    if (input !== undefined) p.stdin?.end(input, 'utf8');
  });
}

// Separate command selection from process execution for tests without touching the clipboard.
export function createPaster(platform: string, execute = run) {
  return async (text: string): Promise<void> => {
    if (!text.trim()) return;
    if (platform === 'darwin') {
      await execute(fileURLToPath(new URL('./voxa-paste', import.meta.url)), [], text);
    } else {
      // stdin avoids argument-size limits and keeps transcripts out of process arguments.
      await execute('wtype', ['-'], text);
    }
  };
}

export const paste = createPaster(process.platform);
