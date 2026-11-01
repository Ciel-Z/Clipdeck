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

    @MainActor
    func testQueueFetchesEachLinkOnceAFewAtATimeInOrder() async {
        let source = SourceApplication(bundleIdentifier: nil, name: "Test", iconData: nil)
        let links = (0 ..< 5).map {
            ClipboardItem(payload: .text(plainText: "https://example.com/\($0)", rtfData: nil), sourceApplication: source)
        }
        let fetched = Box<[ClipboardItem.ID]>([])
        let started = Box<[URL]>([])
        let concurrent = Box(0)
        let peak = Box(0)
        let done = expectation(description: "all fetched")
        done.expectedFulfillmentCount = links.count
        let queue = LinkMetadataQueue(fetch: { url in
            await MainActor.run {
                started.value.append(url)
                concurrent.value += 1
                peak.value = max(peak.value, concurrent.value)
            }
            try? await Task.sleep(nanoseconds: 20_000_000)
            await MainActor.run { concurrent.value -= 1 }
            return LinkMetadata(title: nil, iconData: nil, previewData: nil)
        }, onFetched: { id, _ in
            fetched.value.append(id)
            done.fulfill()
        })
        let sentence = ClipboardItem(payload: .text(plainText: "not a link", rtfData: nil), sourceApplication: source)
        queue.enqueue(links + [sentence])
        queue.enqueue(links) // a synced batch repeating the same cards
        await fulfillment(of: [done], timeout: 5)

        XCTAssertEqual(Set(fetched.value), Set(links.map(\.id)))
        XCTAssertEqual(started.value, links.compactMap(\.webURL))
        XCTAssertLessThanOrEqual(peak.value, LinkMetadataQueue.maximumConcurrentFetches)
    }
}

@MainActor
private final class Box<Value> {
    var value: Value
    init(_ value: Value) { self.value = value }
}
