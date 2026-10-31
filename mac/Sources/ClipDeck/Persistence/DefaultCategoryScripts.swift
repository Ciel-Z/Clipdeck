enum DefaultCategoryScripts {
    struct Preset: Sendable {
        let name: String
        let icon: String
        let script: String
        let previewLanguage: String
    }

    static let legacyJSON = "function classify(item) { try { JSON.parse(item.text); return true; } catch (_) { return false; } }"
    static let formattedLegacyJSON = """
    function classify(item) {
        try {
            JSON.parse(item.text);
            return true;
        }
        catch (_) {
            return false;
        }
    }
    """
    static let json = #"""
    function classify(item) {
        const text = item.text.trim();
        function parse(source) {
            try {
                return JSON.parse(source);
            } catch (_) {
                return undefined;
            }
        }
        function isContainer(value) {
            return value !== null && typeof value === "object";
        }
        // Empty containers and one-number lists ("{}", "[]", "[1]") read as
        // placeholders or citations far more often than as data.
        function isData(value) {
            if (!isContainer(value)) return false;
            if (Array.isArray(value)) {
                return value.length > 1 || (value.length === 1 && isContainer(value[0]));
            }
            return Object.keys(value).length > 0;
        }

        const first = text.charAt(0);
        const last = text.charAt(text.length - 1);
        if ((first === "{" && last === "}") || (first === "[" && last === "]")) {
            if (isData(parse(text))) return true;
        }

        // JSON Lines (log lines, exports): every non-empty line is an object or array.
        const lines = text.split(/\r?\n/).filter(function (line) {
            return line.trim() !== "";
        });
        return lines.length >= 2 && lines.every(function (line) {
            return isContainer(parse(line.trim()));
        });
    }
    """#

    static let previousJSON = """
    function classify(item) {
        const text = item.text.trim();
        const isContainer =
            (text.startsWith("{") && text.endsWith("}")) ||
            (text.startsWith("[") && text.endsWith("]"));
        if (!isContainer) return false;
        try {
            const value = JSON.parse(text);
            return value !== null && typeof value === "object";
        } catch (_) {
            return false;
        }
    }
    """

    static let url = #"""
    function classify(item) {
        // One http(s) link whose host is a real name, IPv4/IPv6 address or
        // localhost: what the Deck shows as a link card.
        return /^https?:\/\/(?:[^\s\/?#@]+@)?(?:\[[0-9a-f:.]+\]|[^\s\/?#:.@-][^\s\/?#:@]*)(?::\d{1,5})?(?:[\/?#]\S*)?$/i
            .test(item.text.trim());
    }
    """#

    static let previousURL = #"""
    function classify(item) {
        return /^https?:\/\/\S+$/i.test(item.text.trim());
    }
    """#

    static let sql = #"""
    function classify(item) {
        // Leading comments and an opening parenthesis do not change the statement.
        const text = item.text.trim()
            .replace(/^(?:\s+|--[^\n]*(?:\n|$)|\/\*[\s\S]*?\*\/)+/, "")
            .replace(/^\(\s*/, "");
        if (text.length < 6) return false;

        // A table or column name, never an English article or pronoun, so that
        // "Delete from the list" or "Insert into your document" stay text.
        const name = "(?!(?:the|a|an|this|that|these|those|my|your|our|their|it|here|there)\\b)[\\w$.`\"\\[\\]]+";
        const statements = [
            new RegExp("^select\\s+(?:distinct\\s+|top\\s+\\d+\\s+)?(?!from\\b)[\\s\\S]+?\\bfrom\\s+" + name, "i"),
            /^select\s+(?:\d+(?:\.\d+)?|'[^']*'|@@?\w+|\w+\s*\([^()]*\)|\*)\s*(?:as\s+\w+\s*)?(?:,[^\n]*)?;?$/i,
            new RegExp("^(?:insert(?:\\s+ignore)?|replace|upsert)\\s+into\\s+" + name, "i"),
            new RegExp("^update\\s+" + name + "\\s+set\\s+[\\w$.`\"\\[\\]]+\\s*=", "i"),
            new RegExp("^delete\\s+from\\s+" + name, "i"),
            new RegExp("^(?:create|alter|drop)\\s+(?:or\\s+replace\\s+|temp(?:orary)?\\s+|unique\\s+|materialized\\s+)*"
                + "(?:table|view|index|database|schema|function|procedure|trigger|sequence|type|extension|user|role)\\s+"
                + "(?:if\\s+(?:not\\s+)?exists\\s+)?" + name
                + "\\s*(?:\\(|;|$|\\n|\\s(?:as|like|on|add|drop|alter|rename|modify|owner|set|cascade|restrict)\\b)", "i"),
            /^with\s+(?:recursive\s+)?[\w"`]+\s*(?:\([^)]*\))?\s+as\s*\(/i,
            new RegExp("^truncate\\s+(?:table\\s+)?" + name, "i"),
            /^merge\s+into\s+\S+/i,
            /^explain\s+(?:analyze\s+|query\s+plan\s+)?(?:select|insert|update|delete|with)\b/i,
            /^(?:grant|revoke)\s+[\w\s,]+?\s+on\s+\S+\s+(?:to|from)\s+\S+/i,
            /^show\s+(?:full\s+)?(?:tables|databases|schemas|columns|indexes|index|create\s+table|processlist|variables|grants)\b/i,
            // "Describe yourself" / "Use Chrome" are sentences: these short MySQL
            // commands count only when written like commands.
            /^(?:desc|describe|use)\s+[\w$.`"]+\s*;$/i,
            /^(?:DESC|DESCRIBE|USE)\s+[\w$.`"]+$/
        ];
        if (!statements.some(function (pattern) { return pattern.test(text); })) return false;

        // Prose that happens to start like SQL: everyday words outside quotes.
        const bare = text.replace(/'(?:[^']|'')*'|"(?:[^"]|"")*"/g, "''");
        const prose = bare.match(/\b(?:the|you|your|please|we|our|my|me|this|these|those|will|should|would|could|can|then|just)\b/gi);
        if (prose && prose.length >= 2) return false;
        // A sentence ending needs some SQL punctuation or clause to back it up.
        if (/[.!?。！？]$/.test(bare)) return /[*=(),;]|\b(?:where|join|group\s+by|order\s+by|limit|values)\b/i.test(bare);
        return true;
    }
    """#

    static let previousSQL = #"""
    function classify(item) {
        return /^(select|insert|update|delete|create|alter|drop)\b/i.test(
            item.text.trim()
        );
    }
    """#

    static let markdown = #"""
    function classify(item) {
        const text = item.text.replace(/\r\n?/g, "\n");
        function count(pattern) {
            const matches = text.match(pattern);
            return matches ? matches.length : 0;
        }

        // Structures only Markdown uses: one is enough.
        const strong = [
            /(?:^|\n)(`{3,}|~{3,})[^\n]*\n[\s\S]*?\n\1[ \t]*(?:\n|$)/,                  // fenced code block
            /^(`{3,}|~{3,})[\w+#.-]*\n/,                                                  // fence opening a copy
            /(?:^|\n)[^\n]*\|[^\n]*\n[ \t]*\|?[ \t]*:?-{3,}:?[ \t]*(?:\|[ \t]*:?-{3,}:?[ \t]*)+\|?[ \t]*(?:\n|$)/, // table
            /!?\[[^\]\n]+\]\((?:https?:\/\/|mailto:|\/|\.{1,2}\/|#)[^)\s]*(?:\s+"[^"\n]*")?\)/, // [link](url), ![image](src)
            /(?:^|\n)#{2,6}[ \t]+\S/,                                                     // ## heading
            /(?:^|\n)[ \t]{0,3}[-*+][ \t]+\[[ xX]\][ \t]+\S/                              // - [ ] task
        ];
        if (strong.some(function (pattern) { return pattern.test(text); })) return true;

        // Shapes plain notes, shell comments or code also use: two different
        // kinds are needed. A lone "# comment" line or one "- item" stays text.
        const weak = [
            /(?:^|\n)#[ \t]+\S/.test(text),                                               // # heading
            count(/(?:^|\n)[ \t]{0,3}[-*+][ \t]+\S/g) >= 2,                               // bullet list
            count(/(?:^|\n)[ \t]{0,3}\d{1,3}[.)][ \t]+\S/g) >= 2,                         // numbered list
            /(?:^|\n)>[ \t]?\S/.test(text),                                               // > quote
            /(?:^|[\s(])(\*\*|__)(?=\S)[^\n]*?\S\1(?=[\s).,:;!?，。；：！？]|$)/.test(text), // **bold**
            /(?:^|[^`])`[^`\n]+`(?!`)/.test(text),                                        // `code`
            /(?:^|\n)[ \t]*(?:-{3,}|\*{3,}|_{3,})[ \t]*(?:\n|$)/.test(text)               // --- rule
        ];
        return weak.filter(Boolean).length >= 2;
    }
    """#

    static let previousMarkdown = #"""
    function classify(item) {
        return /(^|\n)(#{1,6} |[-*] |```)/.test(item.text);
    }
    """#

    static let java = #"""
    function classify(item) {
        const text = item.text.trim();
        const hasJavaDeclaration =
            /\b(public|private|protected|class|interface|enum|record|package|import)\b/.test(text);
        return hasJavaDeclaration && /[;{}]/.test(text);
    }
    """#

    static let javaScript = #"""
    function classify(item) {
        const text = item.text.trim();
        return /\b(function|const|let|var|async|await|export|import)\b|=>/.test(text);
    }
    """#

    static let relevanceBasedCode = #"""
    function classify(item) {
        const text = item.text.trim();
        const isStandaloneColor = /^#(?:[0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})$/i.test(text)
            || /^rgba?\(/i.test(text);
        return !isStandaloneColor
            && Boolean(item.detectedLanguage)
            && item.codeRelevance >= 2;
    }
    """#

    static let code = #"""
    function classify(item) {
        return item.codeEngineMatched === true;
    }
    """#

    static let hashPrefixedColor = #"""
    function classify(item) {
        const text = item.text.trim();
        const hex = /^#(?:[0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})$/i;
        if (hex.test(text)) {
            return true;
        }

        const match = /^rgba?\((.*)\)$/i.exec(text);
        if (!match) {
            return false;
        }

        const channels = match[1].split(",").map(value => value.trim());
        if (channels.length !== 3 && channels.length !== 4) {
            return false;
        }

        const numericChannel = /^\d+(?:\.\d+)?%?$/;
        const validColorChannel = value => numericChannel.test(value) && (value.endsWith("%")
            ? Number(value.slice(0, -1)) >= 0 && Number(value.slice(0, -1)) <= 100
            : Number(value) >= 0 && Number(value) <= 255);
        const validAlphaChannel = value => numericChannel.test(value) && (value.endsWith("%")
            ? Number(value.slice(0, -1)) >= 0 && Number(value.slice(0, -1)) <= 100
            : Number(value) >= 0 && Number(value) <= 1);

        return channels.slice(0, 3).every(validColorChannel)
            && (channels.length === 3 || validAlphaChannel(channels[3]));
    }
    """#

    static let color = #"""
    function classify(item) {
        const text = item.text.trim();
        const hashHex = /^#(?:[0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})$/i;
        const bareHex = /^(?=[0-9a-f]*[a-f])(?:[0-9a-f]{6}|[0-9a-f]{8})$/i;
        if (hashHex.test(text) || bareHex.test(text)) {
            return true;
        }

        const match = /^rgba?\((.*)\)$/i.exec(text);
        if (!match) {
            return false;
        }

        const channels = match[1].split(",").map(value => value.trim());
        if (channels.length !== 3 && channels.length !== 4) {
            return false;
        }

        const numericChannel = /^\d+(?:\.\d+)?%?$/;
        const validColorChannel = value => numericChannel.test(value) && (value.endsWith("%")
            ? Number(value.slice(0, -1)) >= 0 && Number(value.slice(0, -1)) <= 100
            : Number(value) >= 0 && Number(value) <= 255);
        const validAlphaChannel = value => numericChannel.test(value) && (value.endsWith("%")
            ? Number(value.slice(0, -1)) >= 0 && Number(value.slice(0, -1)) <= 100
            : Number(value) >= 0 && Number(value) <= 1);

        return channels.slice(0, 3).every(validColorChannel)
            && (channels.length === 3 || validAlphaChannel(channels[3]));
    }
    """#

    static let numericBareHexColor = #"""
    function classify(item) {
        const text = item.text.trim();
        const hex = /^(?:#(?:[0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})|[0-9a-f]{6}|[0-9a-f]{8})$/i;
        if (hex.test(text)) {
            return true;
        }

        const match = /^rgba?\((.*)\)$/i.exec(text);
        if (!match) {
            return false;
        }

        const channels = match[1].split(",").map(value => value.trim());
        if (channels.length !== 3 && channels.length !== 4) {
            return false;
        }

        const numericChannel = /^\d+(?:\.\d+)?%?$/;
        const validColorChannel = value => numericChannel.test(value) && (value.endsWith("%")
            ? Number(value.slice(0, -1)) >= 0 && Number(value.slice(0, -1)) <= 100
            : Number(value) >= 0 && Number(value) <= 255);
        const validAlphaChannel = value => numericChannel.test(value) && (value.endsWith("%")
            ? Number(value.slice(0, -1)) >= 0 && Number(value.slice(0, -1)) <= 100
            : Number(value) >= 0 && Number(value) <= 1);

        return channels.slice(0, 3).every(validColorChannel)
            && (channels.length === 3 || validAlphaChannel(channels[3]));
    }
    """#

    static let presets: [Preset] = [
        Preset(name: "JSON", icon: "curlybraces", script: json, previewLanguage: "json"),
        Preset(name: "URL", icon: "link", script: url, previewLanguage: "url"),
        Preset(name: "SQL", icon: "cylinder", script: sql, previewLanguage: "sql"),
        Preset(name: "Markdown", icon: "text.badge.checkmark", script: markdown, previewLanguage: "markdown"),
        Preset(name: "代码", icon: "chevron.left.forwardslash.chevron.right", script: code, previewLanguage: "code"),
        Preset(name: "色彩", icon: "eyedropper.halffull", script: color, previewLanguage: "color"),
    ]

    static let legacyURL = "function classify(item) { return /^https?:\\/\\/\\S+$/i.test(item.text.trim()); }"
    static let legacySQL = "function classify(item) { return /^(select|insert|update|delete|create|alter|drop)\\b/i.test(item.text.trim()); }"
    static let legacyShell = "function classify(item) { return /^(sudo |git |cd |ls |npm |yarn |pnpm |brew |curl |ssh )/.test(item.text.trim()); }"
    static let formattedLegacyShell = #"""
    function classify(item) {
        return /^(sudo |git |cd |ls |npm |yarn |pnpm |brew |curl |ssh )/.test(item.text.trim());
    }
    """#
    static let legacyMarkdown = "function classify(item) { return /(^|\\n)(#{1,6} |[-*] |```)/.test(item.text); }"
    static let permissiveColor = #"""
    function classify(item) {
        const text = item.text.trim();
        const hex = /^#(?:[0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})$/i;
        const rgb = /^rgba?\(\s*\d{1,3}%?\s*,\s*\d{1,3}%?\s*,\s*\d{1,3}%?(?:\s*,\s*(?:0|1|0?\.\d+|\d{1,3}%))?\s*\)$/i;
        return hex.test(text) || rgb.test(text);
    }
    """#
}
