// Card presentation rules (ClipboardCardView computed properties,
// ClipboardColorParser) and the Deck model.
#include "deck_image_provider.h"
#include "deck_item_model.h"
#include "deck_presentation.h"
#include "app_preferences.h"
#include "deck_backdrop.h"
#include "javascript_formatter.h"
#include "settings_controller.h"

#include <QGuiApplication>
#include <QSettings>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

ClipboardItem textItem(const QString &text, const QString &title = QStringLiteral("文本")) {
    ClipboardItem item;
    item.type = ClipboardPayloadType::Text;
    item.text = text;
    item.title = title;
    item.copiedAt = QDateTime::currentDateTime();
    return item;
}

void relativeTimeTests() {
    const QDateTime now(QDate(2026, 10, 30), QTime(12, 0));
    using DeckPresentation::relativeTime;
    check(relativeTime(now.addSecs(-59), now) == QStringLiteral("现在"), "time-under-a-minute-is-now");
    check(relativeTime(now.addSecs(-60 * 7), now) == QStringLiteral("7 分钟"), "time-minutes");
    check(relativeTime(now.addSecs(-3600 * 5), now) == QStringLiteral("5 小时"), "time-hours");
    check(relativeTime(now.addDays(-6), now) == QStringLiteral("6 天"), "time-days");
    check(relativeTime(now.addDays(-8), now) == QStringLiteral("10月22日"), "time-over-a-week-is-a-date");
    check(relativeTime(now.addSecs(30), now) == QStringLiteral("现在"), "time-in-the-future-is-now");
}

void linkTests() {
    using namespace DeckPresentation;
    check(webUrl(textItem(QStringLiteral(" https://www.example.com/a "))).isValid(), "link-trims-whitespace");
    check(!webUrl(textItem(QStringLiteral("see https://example.com"))).isValid(), "link-rejects-sentences");
    check(!webUrl(textItem(QStringLiteral("ftp://example.com"))).isValid(), "link-only-http-and-https");
    check(!webUrl(textItem(QStringLiteral("https://"))).isValid(), "link-needs-a-host");
    check(linkHost(textItem(QStringLiteral("https://www.example.com/a"))) == QStringLiteral("example.com"),
          "link-host-drops-www");
    check(linkDisplayTitle(textItem(QStringLiteral("https://www.example.com/a"))) == QStringLiteral("example.com"),
          "link-title-falls-back-to-host");
    check(linkDisplayTitle(textItem(QStringLiteral("https://example.com"), QStringLiteral("Example Domain")))
              == QStringLiteral("Example Domain"),
          "link-title-uses-fetched-title");
    check(cardHeaderTitle(textItem(QStringLiteral("https://example.com"))) == QStringLiteral("链接"), "link-card-header");
    check(cardKind(textItem(QStringLiteral("https://example.com"))) == QStringLiteral("link"), "link-card-kind");
}

void titleTests() {
    using namespace DeckPresentation;
    ClipboardItem files;
    files.type = ClipboardPayloadType::Files;
    files.files = {{QStringLiteral("C:/a.txt"), QStringLiteral("a.txt"), QString(), false},
                   {QStringLiteral("C:/b"), QStringLiteral("b"), QString(), true}};
    check(cardHeaderTitle(files) == QStringLiteral("文件 · 2 项"), "files-header-title");
    check(cardKind(files) == QStringLiteral("files"), "files-card-kind");

    ClipboardItem code = textItem(QStringLiteral("def main():\n    print('hi')\n    return 0\n"));
    code.category = QStringLiteral("代码");
    code.previewLanguage = QStringLiteral("code");
    check(!categoryLabel(code).isEmpty() && categoryLabel(code) != QStringLiteral("代码"),
          "code-category-shows-detected-language");
    ClipboardItem json = textItem(QStringLiteral("{}"));
    json.category = QStringLiteral("JSON");
    json.previewLanguage = QStringLiteral("json");
    check(categoryLabel(json) == QStringLiteral("JSON"), "category-label-is-name");
    check(categoryLabel(textItem(QStringLiteral("x"))).isEmpty(), "uncategorised-has-no-label");
    check(displayCodeLanguage(QStringLiteral("cpp")) == QStringLiteral("C++"), "language-display-name-map");
    check(displayCodeLanguage(QStringLiteral("python")) == QStringLiteral("Python"), "language-display-name-capitalised");
}

void colorTests() {
    using DeckPresentation::parseColor;
    check(parseColor(QStringLiteral("#F54927")) == QColor(0xF5, 0x49, 0x27), "color-hex6");
    check(parseColor(QStringLiteral("#abc")) == QColor(0xAA, 0xBB, 0xCC), "color-hex3-expands");
    check(parseColor(QStringLiteral("#11223380"))->alpha() == 0x80, "color-hex8-alpha");
    check(parseColor(QStringLiteral("a1b2c3")).has_value(), "color-bare-hex-with-letters");
    check(!parseColor(QStringLiteral("123456")).has_value(), "color-bare-digits-are-not-a-color");
    check(parseColor(QStringLiteral("rgb(255, 0, 0)")) == QColor(255, 0, 0), "color-rgb");
    check(qAbs(parseColor(QStringLiteral("rgba(0,0,255,0.5)"))->alphaF() - 0.5) < 0.01, "color-rgba");
    check(parseColor(QStringLiteral("rgb(100%, 0%, 0%)")) == QColor(255, 0, 0), "color-rgb-percent");
    check(!parseColor(QStringLiteral("rgb(300,0,0)")).has_value(), "color-rgb-out-of-range");
    check(!parseColor(QStringLiteral("#12345")).has_value(), "color-hex5-invalid");

    ClipboardItem color = textItem(QStringLiteral("#F54927"));
    color.previewLanguage = QStringLiteral("color");
    check(DeckPresentation::cardKind(color) == QStringLiteral("color"), "color-category-renders-color-card");
    check(DeckPresentation::usesImmersiveHeader(color), "color-card-header-is-immersive");
    ClipboardItem notColor = textItem(QStringLiteral("not a colour"));
    notColor.previewLanguage = QStringLiteral("color");
    check(DeckPresentation::cardKind(notColor) == QStringLiteral("text"), "unparsable-color-falls-back-to-text");
}

// mac testImagePreviewSizeUsesAspectRatioInsteadOfPixelCount / …AccountsForRenderedRotation
void previewLayoutTests() {
    using DeckPresentation::previewImageSize;
    const QSizeF small = previewImageSize(400, 200, 0);
    const QSizeF large = previewImageSize(8000, 4000, 0);
    const QSizeF portrait = previewImageSize(2000, 4000, 0);
    check(small == large, "preview-size-uses-aspect-ratio");
    check(small.width() > small.height(), "preview-landscape-is-wide");
    check(portrait.width() < portrait.height(), "preview-portrait-is-tall");
    check(large.width() <= 560 && large.height() <= 405, "preview-size-is-bounded");
    const QSizeF landscape = previewImageSize(4000, 2000, 0);
    const QSizeF rotated = previewImageSize(4000, 2000, 90);
    check(landscape.width() > landscape.height() && rotated.width() < rotated.height(), "preview-size-follows-rotation");
    check(previewImageSize(0, 0, 0) == DeckPresentation::previewTextSize(), "preview-size-without-pixels-is-text-size");
}

// mac testJSONTextFormatterPrettyPrintsValidJSONAndRejectsInvalidInput
void jsonTests() {
    using DeckPresentation::formatJson;
    check(formatJson(QStringLiteral("{\"b\":2,\"a\":1}")) == QStringLiteral("{\n  \"a\" : 1,\n  \"b\" : 2\n}"),
          "json-sorted-two-space-indent");
    check(formatJson(QStringLiteral("{broken}")).isEmpty(), "json-rejects-invalid");
    check(formatJson(QStringLiteral("42")).isEmpty(), "json-rejects-scalar-top-level");
    check(formatJson(QStringLiteral("{\"u\":\"https://a/b\",\"n\":[1,2.5,true,null]}"))
              == QStringLiteral("{\n  \"n\" : [\n    1,\n    2.5,\n    true,\n    null\n  ],\n  \"u\" : \"https://a/b\"\n}"),
          "json-arrays-numbers-and-unescaped-slashes");
}

void popoverTextTests() {
    using namespace DeckPresentation;
    check(previewDateTime(QDateTime(QDate(2026, 10, 30), QTime(9, 5))) == QStringLiteral("2026年10月30日 09:05"),
          "preview-date-time-format");
    ClipboardItem json = textItem(QStringLiteral("{}"));
    json.previewLanguage = QStringLiteral("JSON");
    json.category = QStringLiteral("JSON");
    check(editableLanguage(json) == QStringLiteral("json"), "editable-language-json");
    check(editableLanguage(textItem(QStringLiteral("x"))).isEmpty(), "plain-text-has-no-editor-language");
    check(previewCategoryName(json) == QStringLiteral("JSON"), "preview-category-name");
    check(previewCategoryName(textItem(QStringLiteral("x"))) == QStringLiteral("文本"), "preview-category-defaults-to-text");
}

void settingsLogicTests() {
    const QString formatted = JavaScriptFormatter::format(
        QStringLiteral("function classify(item){if(item.text.startsWith('#')){return true;}return /a;b/.test(item.text);}"));
    check(formatted == QStringLiteral("function classify(item) {\n    if(item.text.startsWith('#')) {\n        return true;\n    }\n"
                                      "    return /a;b/.test(item.text);\n}"),
          "js-formatter-indents-and-keeps-regex");
    check(JavaScriptFormatter::format(QStringLiteral("var s = \"{;}\"; // {x}")) == QStringLiteral("var s = \"{;}\";\n// {x}"),
          "js-formatter-keeps-strings-and-comments");

    check(SettingsController::formatByteCount(512) == QStringLiteral("512 字节"), "bytes-small");
    check(SettingsController::formatByteCount(345000) == QStringLiteral("345 KB"), "bytes-kilobytes");
    check(SettingsController::formatByteCount(12345678) == QStringLiteral("12.3 MB"), "bytes-megabytes");

    const HotkeySpec next = HotkeySpec::fromQtKey(Qt::Key_Right, Qt::ControlModifier);
    check(next == AppPreferences::defaultNextCategoryShortcut() && next.toString() == QStringLiteral("Ctrl+→"),
          "shortcut-from-qt-arrow");
    check(!HotkeySpec::fromQtKey(Qt::Key_V, Qt::NoModifier).isValid(), "shortcut-needs-modifier");
    check(!HotkeySpec::fromQtKey(Qt::Key_Control, Qt::ControlModifier).isValid(), "shortcut-modifier-alone-is-invalid");
    check(HotkeySpec::fromQtKey(Qt::Key_F5, Qt::AltModifier | Qt::ShiftModifier).toString() == QStringLiteral("Alt+Shift+F5"),
          "shortcut-function-key");
    check(next.matchesQtKey(Qt::Key_Right, Qt::ControlModifier) && !next.matchesQtKey(Qt::Key_Right, Qt::ShiftModifier),
          "shortcut-matches-exact-modifiers");

    QSettings settings;
    settings.remove(QStringLiteral("shortcuts"));
    settings.remove(QStringLiteral("history/retentionDays"));
    check(AppPreferences::retentionDays() == 30, "retention-default-30-days");
    check(AppPreferences::quickPasteModifier() == QStringLiteral("ctrl")
              && AppPreferences::plainTextModifier() == QStringLiteral("shift"),
          "modifier-defaults");
    AppPreferences::setQuickPasteModifier(QStringLiteral("bogus"));
    check(AppPreferences::quickPasteModifier() == QStringLiteral("ctrl"), "invalid-modifier-falls-back");
    HotkeySpec{HotkeySpec::Alt, 'N'}.save(QStringLiteral("nextCategory"));
    AppPreferences::setPlainTextModifier(QStringLiteral("alt"));
    AppPreferences::resetShortcutDefaults();
    check(AppPreferences::nextCategoryShortcut() == AppPreferences::defaultNextCategoryShortcut()
              && AppPreferences::plainTextModifier() == QStringLiteral("shift"),
          "reset-restores-shortcut-defaults");
    settings.remove(QStringLiteral("shortcuts"));
}

void backdropTests() {
    QImage stripes(800, 160, QImage::Format_RGB32);
    for (int y = 0; y < stripes.height(); ++y) {
        for (int x = 0; x < stripes.width(); ++x) stripes.setPixel(x, y, (x / 8) % 2 ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
    }
    const QImage frosted = DeckBackdrop::frost(stripes, 8, 3, 3, 1.0);
    check(frosted.size() == QSize(100, 20), "backdrop-is-downscaled");
    int minimum = 255, maximum = 0;
    for (int x = 10; x < 90; ++x) {
        minimum = std::min(minimum, qRed(frosted.pixel(x, 10)));
        maximum = std::max(maximum, qRed(frosted.pixel(x, 10)));
    }
    check(maximum - minimum < 20 && minimum > 100 && maximum < 160, "backdrop-blurs-stripes-to-grey");

    QImage red(64, 64, QImage::Format_RGB32);
    red.fill(qRgb(180, 100, 100));
    const QImage vivid = DeckBackdrop::frost(red, 8, 1, 1, 1.5);
    check(qRed(vivid.pixel(4, 4)) > 180 && qGreen(vivid.pixel(4, 4)) < 100, "backdrop-raises-saturation");
    check(DeckBackdrop::frost(QImage()).isNull(), "backdrop-of-nothing-is-null");
}

void modelTests() {
    auto *images = new DeckImageProvider;
    DeckItemModel model(images);
    ClipboardItem picture;
    picture.type = ClipboardPayloadType::Image;
    picture.image = QImage(40, 20, QImage::Format_ARGB32);
    picture.image.fill(Qt::red);
    picture.imagePixelSize = QSize(4000, 2000);
    picture.imageAngle = 90;
    ClipboardItem text = textItem(QStringLiteral("hello"));
    text.textLength = 4321;
    model.setItems({picture, text});
    check(model.rowCount() == 2, "model-row-count");
    const QModelIndex first = model.index(0);
    check(model.data(first, DeckItemModel::ImageSizeTextRole).toString() == QStringLiteral("2000 × 4000"),
          "image-size-swaps-for-quarter-turns");
    check(model.data(first, DeckItemModel::ImageUrlRole).toString().startsWith(QStringLiteral("image://deck/thumb/")),
          "image-thumbnail-is-served");
    QSize size;
    const QString key = model.data(first, DeckItemModel::ImageUrlRole).toString().mid(QStringLiteral("image://deck/").size());
    check(!images->requestImage(key, &size, QSize()).isNull() && size == QSize(40, 20), "image-provider-returns-thumbnail");
    check(model.data(model.index(1), DeckItemModel::CharacterCountRole).toLongLong() == 4321,
          "character-count-uses-full-length");

    int resets = 0, changes = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&] { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, [&] { ++changes; });
    ClipboardItem renamed = text;
    renamed.title = QStringLiteral("renamed");
    model.setItems({picture, renamed});
    check(resets == 0 && changes == 1, "same-rows-update-in-place");
    model.setItems({renamed});
    check(resets == 1, "different-rows-reset");
    check(model.indexOf(renamed.id) == 0 && model.indexOf(picture.id) == -1, "index-of-by-identity");
    delete images;
}

} // namespace

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ClipDeckTests"));
    QCoreApplication::setApplicationName(QStringLiteral("deck-tests"));
    relativeTimeTests();
    linkTests();
    titleTests();
    colorTests();
    previewLayoutTests();
    jsonTests();
    popoverTextTests();
    settingsLogicTests();
    backdropTests();
    modelTests();
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
