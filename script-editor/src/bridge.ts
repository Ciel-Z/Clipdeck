export type HostMessage =
  | { type: "initialize"; revision: number; text: string; theme: ThemeName; autofocus?: boolean }
  | { type: "setDocument"; revision: number; text: string }
  | { type: "setTheme"; theme: ThemeName }
  | { type: "setSelection"; anchor: number; head?: number }
  | { type: "focus" }
  | { type: "format" };

export type ThemeName = "light" | "dark";

export type EditorMessage =
  | { type: "ready"; protocolVersion: 1 }
  | { type: "change"; revision: number; text: string }
  | { type: "focusChanged"; focused: boolean }
  | { type: "shortcut"; command: "save" | "run" | "format" };

declare global {
  interface Window {
    webkit?: { messageHandlers?: { clipDeckEditor?: { postMessage(message: EditorMessage): void } } };
    qt?: { webChannelTransport?: unknown };
    clipDeckHost?: { postMessage(message: EditorMessage): void };
    clipDeckAttachHost?: (host: { postMessage(message: EditorMessage): void }) => void;
    clipDeckEditor?: {
      receive(message: HostMessage): void;
      getDocument(): string;
    };
  }
}

const pendingMessages: EditorMessage[] = [];

export function postToHost(message: EditorMessage): void {
  const appleHandler = window.webkit?.messageHandlers?.clipDeckEditor;
  if (appleHandler) {
    appleHandler.postMessage(message);
    return;
  }
  if (window.clipDeckHost) {
    window.clipDeckHost.postMessage(message);
  } else {
    pendingMessages.push(message);
  }
}

export function attachHost(host: { postMessage(message: EditorMessage): void }): void {
  window.clipDeckHost = host;
  for (const message of pendingMessages.splice(0)) host.postMessage(message);
}
