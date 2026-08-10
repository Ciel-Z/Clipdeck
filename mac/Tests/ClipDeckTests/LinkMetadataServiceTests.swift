import Foundation
import XCTest
@testable import ClipDeck

final class LinkMetadataServiceTests: XCTestCase {
    func testExtractsCleanTitleAndDecodesEntities() {
        let html = """
        <html><head><title> Example &amp; Docs\n — Getting Started </title></head></html>
        """

        XCTAssertEqual(
            LinkMetadataService.title(fromHTML: html),
            "Example & Docs — Getting Started"
        )
    }

    func testResolvesRelativeFaviconURL() throws {
        let html = #"<html><head><link rel="shortcut icon" href="/assets/favicon.png"></head></html>"#
        let baseURL = try XCTUnwrap(URL(string: "https://example.com/docs/page"))

        XCTAssertEqual(
            LinkMetadataService.iconURL(fromHTML: html, baseURL: baseURL)?.absoluteString,
            "https://example.com/assets/favicon.png"
        )
    }

    func testCollectsMultipleIconCandidatesAndSupportsAppleTouchIcon() throws {
        let html = """
        <link rel="icon" href="/broken.svg">
        <link rel="apple-touch-icon" href="/touch.png">
        """
        let baseURL = try XCTUnwrap(URL(string: "https://example.com/page"))

        XCTAssertEqual(
            LinkMetadataService.iconURLs(fromHTML: html, baseURL: baseURL).map(\.absoluteString),
            ["https://example.com/broken.svg", "https://example.com/touch.png"]
        )
    }

    func testFallsBackToOpenGraphTitle() {
        let html = #"<html><head><meta property="og:title" content="Fallback &amp; Title"></head></html>"#
        XCTAssertEqual(LinkMetadataService.title(fromHTML: html), "Fallback & Title")
    }

    func testResolvesOpenGraphPreviewImage() throws {
        let html = #"<meta property="og:image" content="/images/preview.jpg">"#
        let baseURL = try XCTUnwrap(URL(string: "https://example.com/articles/one"))

        XCTAssertEqual(
            LinkMetadataService.previewImageURL(fromHTML: html, baseURL: baseURL)?.absoluteString,
            "https://example.com/images/preview.jpg"
        )
    }

    func testReadsManifestAndPrefersItsLargestListedIcon() throws {
        let html = #"<link rel="manifest" href="/site.webmanifest">"#
        let baseURL = try XCTUnwrap(URL(string: "https://example.com/page"))
        let manifestURL = try XCTUnwrap(LinkMetadataService.manifestURL(fromHTML: html, baseURL: baseURL))
        let manifest = Data(#"{"icons":[{"src":"small.png","sizes":"32x32"},{"src":"/large.png","sizes":"512x512"}]}"#.utf8)

        XCTAssertEqual(manifestURL.absoluteString, "https://example.com/site.webmanifest")
        XCTAssertEqual(
            LinkMetadataService.iconURLs(fromManifest: manifest, baseURL: manifestURL).map(\.absoluteString),
            ["https://example.com/large.png", "https://example.com/small.png"]
        )
    }

    func testClipboardItemRecognizesOnlyStandaloneWebLinks() {
        let source = SourceApplication(bundleIdentifier: nil, name: "Test", iconData: nil)
        let link = ClipboardItem(
            payload: .text(plainText: " https://example.com/path ", rtfData: nil),
            sourceApplication: source
        )
        let sentence = ClipboardItem(
            payload: .text(plainText: "visit https://example.com", rtfData: nil),
            sourceApplication: source
        )

        XCTAssertEqual(link.linkHost, "example.com")
        XCTAssertNil(sentence.webURL)
    }
}
