import WebSocket from 'ws';
import type { Config } from './config.js';

// Remove only a sentence-final full stop; preserve internal punctuation and questions.
export function formatTranscript(text: string, stopPunctuation: boolean): string {
  return stopPunctuation ? text : text.replace(/[.。](\s*)$/, '$1');
}

export class Scribe {
  private ws: WebSocket;
  private ready = false;
  private stopPunctuation: boolean;
  private pending: Buffer[] = [];
  private bytes = 0;
  private queuedBytes = 0;
  private tail = Buffer.alloc(0);
  private committed = false;
  private finished = false;
  private timer: NodeJS.Timeout;
  private resolve!: (text: string) => void;
  private reject!: (e: Error) => void;
  readonly result: Promise<string>;
  constructor(config: Config, onError: (error: Error) => void) {
    const key = process.env.ELEVENLABS_API_KEY;
    if (!key) throw Error('ELEVENLABS_API_KEY is not set');
    this.stopPunctuation = config.stopPunctuation;
    const query = new URLSearchParams({ model_id: 'scribe_v2_realtime', audio_format: 'pcm_16000', commit_strategy: 'manual' });
    if (config.language) query.set('language_code', config.language);
    for (const lang of config.secondaryLanguages) query.append('secondary_languages', lang);
    for (const term of config.keyterms) query.append('keyterms', term);
    this.result = new Promise((resolve, reject) => { this.resolve = resolve; this.reject = reject; });
    // Attach a handler immediately even if recording aborts before stop() awaits result.
    void this.result.catch(() => {});
    this.ws = new WebSocket(`wss://api.elevenlabs.io/v1/speech-to-text/realtime?${query}`, { headers: { 'xi-api-key': key }, handshakeTimeout: 8000 });
    this.timer = setTimeout(() => this.fail(Error('Scribe connection timed out')), 10000);
    this.ws.on('message', raw => {
      let event: { message_type?: string; text?: string; error?: string; message?: string };
      try { event = JSON.parse(raw.toString()); } catch { return; }
      if (event.message_type === 'session_started') {
        this.ready = true; clearTimeout(this.timer); this.flush();
      } else if (event.message_type === 'committed_transcript') {
        this.finished = true; clearTimeout(this.timer); this.resolve(formatTranscript(event.text ?? '', this.stopPunctuation)); this.ws.close();
      } else if (event.message_type?.includes('error')) {
        this.fail(Error(`Scribe ${event.message_type}: ${event.message ?? event.error ?? 'request failed'}`));
      }
    });
    this.ws.on('error', e => this.fail(Error(`Scribe connection: ${e.message}`)));
    this.ws.on('close', (code) => { if (!this.finished) this.fail(Error(`Scribe disconnected (${code})`)); });
    this.result.catch(onError);
  }
  private send(data: Buffer, commit: boolean) {
    this.ws.send(JSON.stringify({ message_type: 'input_audio_chunk', audio_base_64: data.toString('base64'), sample_rate: 16000, commit }));
  }
  // Keep the last chunk unsent so release always commits with actual audio, not an empty frame.
  add(data: Buffer) {
    if (this.committed || this.finished) return;
    this.bytes += data.length;
    this.queuedBytes += data.length;
    if (this.queuedBytes > 16_000 * 2 * 12) { this.fail(Error('Scribe connection too slow (audio queue exceeded 12s)')); return; }
    this.pending.push(data);
    if (this.ready) this.flush();
  }
  private flush() {
    if (!this.ready || this.finished) return;
    for (const piece of this.pending) {
      const combined = Buffer.concat([this.tail, piece]);
      // Stream in 200ms frames, retaining one frame for final commit.
      while (combined.length - this.offset >= 12800) {
        this.send(combined.subarray(this.offset, this.offset + 6400), false);
        this.offset += 6400;
      }
      this.tail = combined.subarray(this.offset);
      this.offset = 0;
    }
    this.pending = [];
    this.queuedBytes = 0;
  }
  private offset = 0;
  stop(): Promise<string> {
    this.committed = true;
    if (this.finished) return this.result;
    const commit = () => {
      if (this.finished) return;
      this.flush();
      if (!this.bytes) { this.fail(Error('no microphone audio captured')); return; }
      this.send(this.tail, true);
      this.tail = Buffer.alloc(0);
      this.timer = setTimeout(() => this.fail(Error('Scribe commit timed out')), 8000);
    };
    if (this.ready) commit();
    else this.ws.once('message', () => { if (this.ready) commit(); });
    return this.result;
  }
  abort() { this.fail(Error('dictation aborted')); }
  private fail(error: Error) {
    if (this.finished) return;
    this.finished = true; clearTimeout(this.timer); this.ws.close(); this.reject(error);
  }
}
