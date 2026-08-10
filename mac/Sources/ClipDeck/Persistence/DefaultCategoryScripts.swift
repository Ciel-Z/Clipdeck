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
    static let json = """
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
        return /^https?:\/\/\S+$/i.test(item.text.trim());
    }
    """#

    static let sql = #"""
    function classify(item) {
        return /^(select|insert|update|delete|create|alter|drop)\b/i.test(
            item.text.trim()
        );
    }
    """#

    static let markdown = #"""
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
