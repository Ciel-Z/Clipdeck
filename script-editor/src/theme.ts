import { EditorView } from "@codemirror/view";
import { HighlightStyle, syntaxHighlighting } from "@codemirror/language";
import { tags } from "@lezer/highlight";

const monospace = "ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, 'Noto Sans Mono', 'DejaVu Sans Mono', monospace";

// The math-italic glyphs CodeMirror uses for these icons are missing from
// many Linux fonts; an italic serif letter looks the same everywhere.
const letterIcon = (letter: string) => ({
  "&:after": { content: `'${letter}'`, fontFamily: "Georgia, 'Times New Roman', 'Noto Serif', 'DejaVu Serif', serif", fontStyle: "italic" }
});

export const clipDeckTheme = EditorView.theme({
  "&": { height: "100%", color: "var(--editor-foreground)", backgroundColor: "var(--editor-background)" },
  ".cm-scroller": { fontFamily: monospace, lineHeight: "1.55", overflow: "auto" },
  // Chromium 65 (Qt 5.11 WebEngine on UOS) rejects the two-value
  // `overflow: hidden auto` in CodeMirror's base theme, leaving the list
  // unclipped; spell out each axis and the font instead of "monospace".
  ".cm-tooltip.cm-tooltip-autocomplete > ul": { fontFamily: monospace, overflowX: "hidden", overflowY: "auto", maxWidth: "700px" },
  ".cm-completionIcon-variable": letterIcon("x"),
  ".cm-completionIcon-constant": letterIcon("C"),
  ".cm-completionIcon-type": letterIcon("t"),
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
