# ClipDeck Script Editor

Shared CodeMirror 6 editor used by the macOS and Linux clients. The editor owns
text editing, completion, highlighting and editor shortcuts. Script execution
stays in each native client.

```sh
npm ci
npm run check
npm run build
```

`dist/` is committed because application packaging is offline. Platform hosts
must load only the bundled `index.html` and expose the narrow message contract
defined in `src/bridge.ts`.

## Hosts

- macOS: `WKWebView` and the `clipDeckEditor` script message handler.
- Linux: `QWebEngineView` and `QWebChannel`; after the channel is ready, call
  `window.clipDeckAttachHost(channel.objects.clipDeckEditor)`. Messages emitted
  before the asynchronous channel handshake are queued and delivered afterward.
