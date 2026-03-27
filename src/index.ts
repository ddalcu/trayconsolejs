import { spawn, ChildProcess } from 'node:child_process';
import { createInterface, Interface } from 'node:readline';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readFileSync } from 'node:fs';
import { EventEmitter } from 'node:events';

const require = createRequire(import.meta.url);
const __dirname = dirname(fileURLToPath(import.meta.url));

const PLATFORMS: Record<string, string> = {
  'linux-x64':     '@agent-orcha/trayconsole-linux-x64',
  'linux-arm64':   '@agent-orcha/trayconsole-linux-arm64',
  'darwin-x64':    '@agent-orcha/trayconsole-darwin-x64',
  'darwin-arm64':  '@agent-orcha/trayconsole-darwin-arm64',
  'win32-x64':     '@agent-orcha/trayconsole-win32-x64',
  'win32-arm64':   '@agent-orcha/trayconsole-win32-arm64',
};

const BIN_NAME = process.platform === 'win32' ? 'trayconsole.exe' : 'trayconsole';

export interface MenuItem {
  id: string;
  title?: string;
  tooltip?: string;
  enabled?: boolean;
  checked?: boolean;
  separator?: boolean;
  items?: MenuItem[];
}

export interface Icon {
  png: string;
  ico: string;
}

export interface TrayConsoleOptions {
  icon?: Icon;
  tooltip?: string;
  title?: string;
  onMenuRequested?: () => MenuItem[] | Promise<MenuItem[]>;
  onClicked?: (id: string) => void;
}

function resolveIcon(icon: Icon): Buffer {
  const path = process.platform === 'win32' ? icon.ico : icon.png;
  return readFileSync(path);
}

function getBinaryPath(): string {
  const key = `${process.platform}-${process.arch}`;
  if (process.env.DEV)
    return join(__dirname, '..', 'binaries', key, 'bin', BIN_NAME);

  const pkg = PLATFORMS[key];
  if (!pkg)
    throw new Error(`trayconsolejs: unsupported platform ${key}`);
  const pkgJson = require.resolve(`${pkg}/package.json`);
  return join(dirname(pkgJson), 'bin', BIN_NAME);
}

export class TrayConsole extends EventEmitter {
  #proc: ChildProcess;
  #rl: Interface;
  #menuRequestedCb?: () => MenuItem[] | Promise<MenuItem[]>;
  #clickedCb?: (id: string) => void;
  #pendingIcon?: Icon | null;
  #pendingTitle?: string | null;

  constructor({ icon, tooltip, title, onMenuRequested, onClicked }: TrayConsoleOptions = {}) {
    super();
    this.#menuRequestedCb = onMenuRequested;
    this.#clickedCb = onClicked;
    this.#pendingIcon = icon;
    this.#pendingTitle = title;

    const bin = getBinaryPath();
    const args: string[] = [];
    if (tooltip) args.push('--tooltip', tooltip);

    this.#proc = spawn(bin, args, {
      stdio: ['pipe', 'pipe', 'inherit'],
      detached: true,
    });
    // Allow the Node process to exit without waiting for the tray binary.
    // The binary will detect stdin EOF and show "Process exited" in the log.
    this.#proc.unref();

    this.#rl = createInterface({ input: this.#proc.stdout! });
    this.#rl.on('line', (line: string) => this.#handle(JSON.parse(line)));
    this.#proc.on('close', (code: number | null) => this.emit('close', code));
  }

  #send(msg: Record<string, unknown>): void {
    this.#proc.stdin!.write(JSON.stringify(msg) + '\n');
  }

  async #handle(msg: { method: string; params?: Record<string, unknown> }): Promise<void> {
    switch (msg.method) {
      case 'ready':
        if (this.#pendingIcon) {
          this.setIcon(this.#pendingIcon);
          this.#pendingIcon = null;
        }
        if (this.#pendingTitle) {
          this.setTitle(this.#pendingTitle);
          this.#pendingTitle = null;
        }
        this.emit('ready');
        break;
      case 'menuRequested':
        await this.#refreshMenu();
        break;
      case 'clicked':
        this.#clickedCb?.((msg.params as { id: string }).id);
        break;
    }
  }

  async #refreshMenu(): Promise<void> {
    if (!this.#menuRequestedCb) return;
    const items = await this.#menuRequestedCb();
    this.#send({ method: 'setMenu', params: { items } });
  }

  /** Update the tray icon. */
  setIcon(icon: Icon): void {
    const buf = resolveIcon(icon);
    this.#send({
      method: 'setIcon',
      params: { base64: buf.toString('base64') },
    });
  }

  /** Set tray context menu items directly. */
  setMenu(items: MenuItem[]): void {
    this.#send({ method: 'setMenu', params: { items } });
  }

  /** Update the tray tooltip text. */
  setTooltip(text: string): void {
    this.#send({ method: 'setTooltip', params: { text } });
  }

  /** Append text to the log console window. */
  appendLog(text: string): void {
    this.#send({ method: 'appendLog', params: { text } });
  }

  /** Show / restore the log console window. */
  showWindow(): void {
    this.#send({ method: 'showWindow' });
  }

  /** Hide the log console window (minimize to tray). */
  hideWindow(): void {
    this.#send({ method: 'hideWindow' });
  }

  /** Set the log console window title. */
  setTitle(text: string): void {
    this.#send({ method: 'setTitle', params: { text } });
  }

  /** Close the tray and log window, ending the process. */
  quit(): void {
    this.#proc.stdin!.end();
  }
}
