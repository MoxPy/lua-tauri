export interface TauriEvent<T = unknown> {
  event: string;
  payload: T;
}

export type UnlistenFn = () => void;

export interface TauriDirEntry {
  name: string;
  isDirectory: boolean;
}

export interface TauriWindowOptions {
  label?: string;
  title?: string;
  url?: string;
  width?: number;
  height?: number;
  resizable?: boolean;
  decorations?: boolean;
  transparent?: boolean;
}

export interface TauriWebviewWindowHandle {
  readonly label?: string;
  setTitle(title: string): Promise<boolean>;
  setSize(width: number, height: number): Promise<boolean>;
  close(): Promise<boolean>;
  destroy(): Promise<boolean>;
  onCloseRequested(handler: (event: TauriEvent<{ label?: string }>) => void): Promise<UnlistenFn>;
  hide(): Promise<boolean>;
  show(): Promise<boolean>;
  minimize(): Promise<boolean>;
  maximize(): Promise<boolean>;
  setFocus?(): Promise<boolean>;
  theme?(): Promise<"light" | "dark">;
  onDragDropEvent?(handler: (payload: { label: string; type: string; paths?: string[]; position?: { x: number; y: number } }) => void): Promise<UnlistenFn>;
  startDragging?(): Promise<boolean>;
}

export interface TauriDialogOptions {
  title?: string;
  kind?: "info" | "warning" | "error";
  type?: "info" | "warning" | "error";
}

export interface TauriNotificationOptions {
  title: string;
  body?: string;
}

export interface TauriShellOutput {
  code: number;
  stdout?: string;
  error?: string;
}

export interface TauriChannelInstance<T = unknown> {
  readonly id: number;
  onmessage: ((data: T) => void) | null;
  send(data: T): void;
  close(): void;
  toJSON(): { __tauri_channel__: number };
}

export interface TauriChannelConstructor {
  new <T = unknown>(onMessage?: (data: T) => void): TauriChannelInstance<T>;
}

export interface TauriGlobalAPI {
  core: {
    invoke<T = unknown>(cmd: string, payload?: Record<string, unknown>): Promise<T>;
  };
  invoke<T = unknown>(cmd: string, payload?: Record<string, unknown>): Promise<T>;
  Channel: TauriChannelConstructor;
  channel: {
    Channel: TauriChannelConstructor;
  };
  state: {
    get<T = unknown>(key: string): Promise<T>;
    set(key: string, value: unknown): Promise<boolean>;
  };
  event: {
    listen<T = unknown>(event: string, cb: (event: TauriEvent<T>) => void): Promise<UnlistenFn>;
    emit(event: string, payload?: unknown): Promise<boolean>;
  };
  webviewWindow: {
    WebviewWindow: new (label: string, options?: TauriWindowOptions) => TauriWebviewWindowHandle;
    getCurrentWebviewWindow(): TauriWebviewWindowHandle;
  };
  menu: {
    onMenuClicked(handler: (payload: { id: string }) => void): Promise<UnlistenFn>;
    setMenu(menu: Record<string, unknown>): Promise<boolean>;
  };
  tray: {
    TrayIcon: {
      new(options?: { tooltip?: string; icon?: string }): Promise<boolean>;
    };
  };
  globalShortcut: {
    register(shortcut: string, handler: (event: TauriEvent<Record<string, unknown>>) => void): Promise<boolean>;
    unregister(shortcut: string): Promise<boolean>;
  };
  deepLink: {
    onOpenUrl(handler: (event: TauriEvent<{ url: string }>) => void): Promise<UnlistenFn>;
  };
  updater: {
    check(): Promise<never>;
    downloadAndInstall(): Promise<never>;
  };
  fs: {
    readTextFile(path: string): Promise<string>;
    writeTextFile(path: string, contents: string): Promise<boolean>;
    readDir(path: string): Promise<TauriDirEntry[]>;
    remove(path: string): Promise<boolean>;
  };
  dialog: {
    open(options?: TauriDialogOptions): Promise<string | null>;
    save(options?: TauriDialogOptions): Promise<string | null>;
    message(message: string, options?: TauriDialogOptions): Promise<boolean>;
    ask(message: string, options?: TauriDialogOptions): Promise<boolean>;
  };
  clipboard: {
    readText(): Promise<string>;
    writeText(text: string): Promise<boolean>;
    readHtml(): Promise<string>;
    writeHtml(html: string): Promise<boolean>;
    readImage(): Promise<string>;
    writeImage(base64Png: string): Promise<boolean>;
    clear(): Promise<boolean>;
  };
  window: {
    theme(): Promise<"light" | "dark">;
  };
  notification: {
    isPermissionGranted(): Promise<boolean>;
    requestPermission(): Promise<"granted">;
    sendNotification(options: string | TauriNotificationOptions): Promise<boolean>;
  };
  singleInstance: {
    onSecondInstance(cb: (payload: { args?: string[]; cwd?: string }) => void): Promise<UnlistenFn>;
  };
  store: {
    get<T = unknown>(key: string): Promise<T>;
    set(key: string, value: unknown): Promise<boolean>;
    has(key: string): Promise<boolean>;
    delete(key: string): Promise<boolean>;
    clear(): Promise<boolean>;
  };
  shell: {
    execute(cmd: string, args?: string[] | ((chunk: { line: string; type: string }) => void), onOutput?: (chunk: { line: string; type: string }) => void): Promise<TauriShellOutput>;
  };
  path: {
    appDataDir(): Promise<string>;
    homeDir(): Promise<string>;
    desktopDir(): Promise<string>;
  };
}

declare global {
  interface Window {
    __TAURI__: TauriGlobalAPI;
  }
}
