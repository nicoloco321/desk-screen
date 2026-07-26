// DeskSwitch - a macOS menu bar dropdown for the ESP32 desk display.
//
// Same endpoints the /switch Claude Code command uses:
//   POST /mode/usage | /mode/spotify | /mode/split | /mode/toggle
//   GET  /mode                                  (current mode, no change)
//
// Build with menubar/build.sh, which produces DeskSwitch.app. It runs as an
// agent (LSUIElement), so there is no Dock icon or main window - just the
// menu bar item, whose glyph tracks whatever the display is currently showing.

import Cocoa

private let kHost = "claude-display.local"
private let kPort = 8080
private let kTimeout: TimeInterval = 5   // headroom for a cold mDNS resolve
private let kRefreshSeconds: TimeInterval = 60

// ---------------------------------------------------------------- modes

enum Mode: String, CaseIterable {
    case usage, spotify, split

    var title: String {
        switch self {
        case .usage:   return "Claude Code usage"
        case .spotify: return "Spotify now playing"
        case .split:   return "Split screen"
        }
    }

    /// Menu bar glyph, so the icon alone says what the panel is showing.
    var symbol: String {
        switch self {
        case .usage:   return "chart.bar.fill"
        case .spotify: return "music.note"
        case .split:   return "rectangle.split.2x1.fill"
        }
    }
}

// ---------------------------------------------------------------- network

/// Talks to the display, addressed by its mDNS hostname.
///
/// The shell commands in this repo have to pass `curl -4`, because a raw
/// dual-stack getaddrinfo on a .local name stalls ~5s on the AAAA query that
/// nothing answers - and on a cold mDNS cache it fails outright with
/// EAI_NONAME. URLSession does not share that problem: CFNetwork resolves
/// .local on its own path, measured at 369ms cold and ~90ms warm with no
/// stalls. So hand the hostname straight to URLSession.
///
/// Do not "optimise" this by pre-resolving to an IPv4 literal - that was the
/// first cut here and it was both slower and flaky on a cold cache, on top of
/// going stale whenever DHCP moves the device.
final class Device {
    func send(path: String, method: String, done: @escaping (String?) -> Void) {
        guard let url = URL(string: "http://\(kHost):\(kPort)\(path)") else {
            DispatchQueue.main.async { done(nil) }
            return
        }
        var req = URLRequest(url: url, timeoutInterval: kTimeout)
        req.httpMethod = method
        req.cachePolicy = .reloadIgnoringLocalCacheData   // /mode must never be cached

        URLSession.shared.dataTask(with: req) { data, response, _ in
            let code = (response as? HTTPURLResponse)?.statusCode ?? 0
            var body: String?
            if code == 200, let data = data {
                body = String(decoding: data, as: UTF8.self)
                    .trimmingCharacters(in: .whitespacesAndNewlines)
            }
            DispatchQueue.main.async { done(body) }
        }.resume()
    }
}

// ---------------------------------------------------------------- menu

final class Controller: NSObject, NSMenuDelegate {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let device = Device()
    private let menu = NSMenu()
    private let statusLine = NSMenuItem(title: "Checking…", action: nil, keyEquivalent: "")
    private var modeItems: [Mode: NSMenuItem] = [:]
    private var current: Mode?
    private var timer: Timer?

    override init() {
        super.init()

        menu.delegate = self
        statusLine.isEnabled = false
        menu.addItem(statusLine)
        menu.addItem(.separator())

        for mode in Mode.allCases {
            let item = NSMenuItem(title: mode.title,
                                  action: #selector(pick(_:)),
                                  keyEquivalent: "")
            item.target = self
            item.representedObject = mode.rawValue
            modeItems[mode] = item
            menu.addItem(item)
        }

        menu.addItem(.separator())
        let toggle = NSMenuItem(title: "Cycle to next",
                                action: #selector(cycle),
                                keyEquivalent: "t")
        toggle.target = self
        menu.addItem(toggle)

        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Quit DeskSwitch",
                              action: #selector(NSApplication.terminate(_:)),
                              keyEquivalent: "q")
        menu.addItem(quit)

        statusItem.menu = menu
        setIcon(nil)
        refresh()

        // Keeps the glyph honest when the mode is changed elsewhere - the
        // /switch command, a curl, or another machine.
        timer = Timer.scheduledTimer(withTimeInterval: kRefreshSeconds, repeats: true) { [weak self] _ in
            self?.refresh()
        }
    }

    /// `nil` means unreachable: fall back to a neutral glyph rather than
    /// leaving a stale mode showing.
    private func setIcon(_ mode: Mode?) {
        let name = mode?.symbol ?? "display.trianglebadge.exclamationmark"
        let label = mode.map { "Desk display: \($0.title)" } ?? "Desk display unreachable"
        let image = NSImage(systemSymbolName: name, accessibilityDescription: label)
        image?.isTemplate = true          // let the menu bar tint it for light/dark
        statusItem.button?.image = image
        statusItem.button?.toolTip = label
    }

    private func apply(_ raw: String?) {
        current = raw.flatMap { Mode(rawValue: $0) }
        statusLine.title = current.map { "Showing: \($0.title)" } ?? "Display unreachable"
        for (mode, item) in modeItems {
            item.state = (mode == current) ? .on : .off
        }
        setIcon(current)
    }

    private func refresh() {
        device.send(path: "/mode", method: "GET") { [weak self] body in
            self?.apply(body)
        }
    }

    // Reopening the menu is the cheapest moment to resync, so do it then too.
    func menuWillOpen(_ menu: NSMenu) { refresh() }

    @objc private func pick(_ sender: NSMenuItem) {
        guard let raw = sender.representedObject as? String else { return }
        statusLine.title = "Switching…"
        device.send(path: "/mode/\(raw)", method: "POST") { [weak self] body in
            // The device answers with the mode that is now active, so the reply
            // is authoritative - including a 409 refusal, which leaves it
            // unchanged and simply reports the old mode back.
            self?.apply(body)
        }
    }

    @objc private func cycle() {
        statusLine.title = "Switching…"
        device.send(path: "/mode/toggle", method: "POST") { [weak self] body in
            self?.apply(body)
        }
    }
}

// ---------------------------------------------------------------- main

final class AppDelegate: NSObject, NSApplicationDelegate {
    // Held here so the status item outlives launch; nothing else retains it.
    private var controller: Controller?

    func applicationDidFinishLaunching(_ notification: Notification) {
        controller = Controller()
    }
}

@main
struct DeskSwitchApp {
    static func main() {
        let app = NSApplication.shared
        // NSApplication.delegate is unowned, so this local has to outlive the
        // call - it does, because run() only returns when the app quits.
        let delegate = AppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.accessory)   // menu bar only, no Dock icon
        app.run()
    }
}
