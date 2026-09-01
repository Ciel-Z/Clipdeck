import { CompletionContext, CompletionResult } from "@codemirror/autocomplete";

const itemCompletions = [
  { label: "item.text", type: "property", detail: "复制的文本内容" },
  { label: "item.textLength", type: "property", detail: "文本字符数" },
  { label: "item.sourceApplication.bundleId", type: "property", detail: "来源应用标识" },
  { label: "item.sourceApplication.name", type: "property", detail: "来源应用名称" },
  { label: "item.copiedAt", type: "property", detail: "毫秒时间戳" },
  { label: "item.detectedLanguage", type: "property", detail: "识别的代码语言" },
  { label: "item.codeScore", type: "property", detail: "代码识别分数" },
  { label: "item.codeEngineMatched", type: "property", detail: "是否识别为代码" }
];

const consoleCompletions = ["log", "info", "warn", "error", "debug"].map(name => ({
  label: `console.${name}`,
  type: "function",
  apply: `console.${name}()`
}));

export function clipDeckCompletion(context: CompletionContext): CompletionResult | null {
  const word = context.matchBefore(/[\w.]*/);
  if (!word || (word.from === word.to && !context.explicit)) return null;
  return {
    from: word.from,
    options: [...itemCompletions, ...consoleCompletions],
    validFor: /^[\w.]*$/
  };
}
