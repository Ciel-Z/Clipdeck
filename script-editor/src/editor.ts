import { basicSetup } from "codemirror";
import { autocompletion } from "@codemirror/autocomplete";
import { indentWithTab, insertNewlineAndIndent } from "@codemirror/commands";
import { javascript } from "@codemirror/lang-javascript";
import { indentUnit } from "@codemirror/language";
import { EditorState, Prec, Transaction } from "@codemirror/state";
import { EditorView, keymap } from "@codemirror/view";
import { clipDeckCompletion } from "./completion";
import { attachHost, HostMessage, postToHost, ThemeName } from "./bridge";
import { clipDeckHighlighting, clipDeckTheme } from "./theme";

let revision = 0;
let applyingHostDocument = false;
let composing = false;

function applyTheme(theme: ThemeName): void {
  document.documentElement.dataset.theme = theme;
}

const view = new EditorView({
  parent: document.getElementById("editor")!,
  state: EditorState.create({
    doc: "",
    extensions: [
      basicSetup,
      javascript(),
      indentUnit.of("    "),
      autocompletion({ override: [clipDeckCompletion], activateOnTyping: true }),
      clipDeckTheme,
      clipDeckHighlighting,
      Prec.high(keymap.of([
        indentWithTab,
        { key: "Enter", run: insertNewlineAndIndent },
        { key: "Mod-s", preventDefault: true, run: () => sendShortcut("save") },
        { key: "Mod-Enter", preventDefault: true, run: () => sendShortcut("run") },
        { key: "Shift-Mod-f", preventDefault: true, run: () => sendShortcut("format") }
      ])),
      EditorView.updateListener.of(update => {
        if (!update.docChanged || applyingHostDocument) return;
        revision += 1;
        postToHost({ type: "change", revision, text: update.state.doc.toString() });
      }),
      EditorView.domEventHandlers({
        compositionstart: () => { composing = true; },
        compositionend: () => { composing = false; },
        focus: () => { postToHost({ type: "focusChanged", focused: true }); },
        blur: () => { postToHost({ type: "focusChanged", focused: false }); }
      })
    ]
  })
});

// Overlay-style scrollbars: both thumbs show while the code scrolls (wheel,
// thumb drag, keyboard) and hide shortly after; see .is-scrolling in editor.css.
let scrollbarTimer: number | undefined;
view.scrollDOM.addEventListener("scroll", () => {
  view.scrollDOM.classList.add("is-scrolling");
  window.clearTimeout(scrollbarTimer);
  scrollbarTimer = window.setTimeout(() => view.scrollDOM.classList.remove("is-scrolling"), 900);
}, { passive: true });

function sendShortcut(command: "save" | "run" | "format"): true {
  postToHost({ type: "shortcut", command });
  return true;
}

function setDocument(text: string, nextRevision: number): void {
  if (composing || text === view.state.doc.toString()) return;
  applyingHostDocument = true;
  revision = Math.max(revision, nextRevision);
  view.dispatch({
    changes: { from: 0, to: view.state.doc.length, insert: text },
    annotations: Transaction.addToHistory.of(false)
  });
  applyingHostDocument = false;
}

function receive(message: HostMessage): void {
  switch (message.type) {
    case "initialize":
      applyTheme(message.theme);
      setDocument(message.text, message.revision);
      if (message.autofocus) requestAnimationFrame(() => view.focus());
      break;
    case "setDocument":
      setDocument(message.text, message.revision);
      break;
    case "setTheme":
      applyTheme(message.theme);
      break;
    case "setSelection": {
      const anchor = Math.max(0, Math.min(message.anchor, view.state.doc.length));
      const head = Math.max(0, Math.min(message.head ?? anchor, view.state.doc.length));
      view.dispatch({ selection: { anchor, head }, scrollIntoView: true });
      break;
    }
    case "focus":
      view.focus();
      break;
    case "format":
      postToHost({ type: "shortcut", command: "format" });
      break;
  }
}

Object.assign(window, {
  clipDeckEditor: { receive, getDocument: () => view.state.doc.toString() },
  clipDeckAttachHost: attachHost
});
postToHost({ type: "ready", protocolVersion: 1 });
