import { EditorView } from "@codemirror/view";
import { HighlightStyle, syntaxHighlighting } from "@codemirror/language";
import { tags } from "@lezer/highlight";

export const clipDeckTheme = EditorView.theme({
  "&": { height: "100%", color: "var(--editor-foreground)", backgroundColor: "var(--editor-background)" },
  ".cm-scroller": { fontFamily: "ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace", lineHeight: "1.55", overflow: "auto" },
  ".cm-content": { padding: "10px 0", caretColor: "var(--editor-caret)" },
  ".cm-cursor, .cm-dropCursor": { borderLeftColor: "var(--editor-caret) !important", borderLeftWidth: "2px" },
  ".cm-line": { padding: "0 12px 0 6px" },
  ".cm-gutters": { backgroundColor: "var(--editor-background)", color: "var(--editor-line-number)", border: "none" },
  ".cm-activeLine, .cm-activeLineGutter": { backgroundColor: "var(--editor-active-line)" },
  ".cm-selectionBackground, &.cm-focused .cm-selectionBackground, ::selection": { backgroundColor: "var(--editor-selection) !important" },
  ".cm-tooltip": { backgroundColor: "var(--editor-tooltip)", border: "1px solid var(--editor-border)" },
  ".cm-tooltip-autocomplete > ul > li[aria-selected]": { backgroundColor: "var(--editor-selection)", color: "var(--editor-foreground)" },
  ".cm-completionDetail": { color: "var(--editor-secondary)", fontStyle: "normal" }
});

export const clipDeckHighlighting = syntaxHighlighting(HighlightStyle.define([
  { tag: [tags.keyword, tags.modifier], color: "#c678dd" },
  { tag: [tags.string, tags.special(tags.string)], color: "#98c379" },
  { tag: [tags.number, tags.bool, tags.null], color: "#d19a66" },
  { tag: [tags.comment, tags.lineComment, tags.blockComment], color: "#7f848e", fontStyle: "italic" },
  { tag: [tags.function(tags.variableName), tags.definition(tags.variableName)], color: "#61afef" },
  { tag: [tags.propertyName], color: "#e5c07b" },
  { tag: [tags.operator, tags.punctuation], color: "#abb2bf" }
]));
