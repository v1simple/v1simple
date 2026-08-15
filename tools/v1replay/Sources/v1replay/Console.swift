import Foundation
import Darwin

// =============================================================================
// Terminal: raw-mode key capture, a scrolling log, and a sticky status line.
// =============================================================================

final class Console {
    private let lock = NSLock()
    private var status: String = ""
    private var rawModeActive = false
    private var originalTermios = termios()
    private let interactive: Bool

    var onKey: ((Character) -> Void)?

    init() {
        interactive = isatty(STDIN_FILENO) == 1
    }

    // MARK: - Raw mode

    func enableRawMode() {
        guard interactive, !rawModeActive else { return }
        guard tcgetattr(STDIN_FILENO, &originalTermios) == 0 else { return }
        var raw = originalTermios
        raw.c_lflag &= ~(tcflag_t(ECHO) | tcflag_t(ICANON))
        guard tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0 else { return }
        rawModeActive = true
    }

    func restore() {
        guard rawModeActive else { return }
        rawModeActive = false
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &originalTermios)
        fputs("\u{1B}[?25h", stdout)   // show cursor
        fflush(stdout)
    }

    /// Blocking key reader — run it on its own thread.
    func startKeyLoop() {
        guard interactive else { return }
        let thread = Thread { [weak self] in
            while true {
                var byte: UInt8 = 0
                let count = read(STDIN_FILENO, &byte, 1)
                if count <= 0 { break }
                guard let self = self else { break }
                self.onKey?(Character(UnicodeScalar(byte)))
            }
        }
        thread.name = "v1replay.keys"
        thread.start()
    }

    // MARK: - Output

    func log(_ message: String) {
        lock.lock()
        fputs("\u{1B}[2K\r" + message + "\n", stdout)
        if !status.isEmpty { fputs("\u{1B}[2K\r" + status, stdout) }
        fflush(stdout)
        lock.unlock()
    }

    func setStatus(_ line: String) {
        lock.lock()
        status = line
        fputs("\u{1B}[2K\r" + line, stdout)
        fflush(stdout)
        lock.unlock()
    }

    func clearStatus() {
        lock.lock()
        status = ""
        fputs("\u{1B}[2K\r", stdout)
        fflush(stdout)
        lock.unlock()
    }

    /// Plain (non-status) output, used before the replay starts.
    func print(_ message: String = "") {
        lock.lock()
        fputs(message + "\n", stdout)
        fflush(stdout)
        lock.unlock()
    }

    // MARK: - Formatting helpers

    static func barMeter(_ bars: Int, width: Int = 8) -> String {
        let lit = max(0, min(width, bars))
        let colour: String
        switch lit {
        case 0: colour = Ansi.dim
        case 1...2: colour = Ansi.green
        case 3...5: colour = Ansi.yellow
        default: colour = Ansi.red
        }
        let filled = String(repeating: "▮", count: lit)
        let empty = String(repeating: "▯", count: width - lit)
        return colour + filled + Ansi.dim + empty + Ansi.reset
    }

    static func clock(_ seconds: Double) -> String {
        let total = max(0, Int(seconds.rounded()))
        return String(format: "%d:%02d", total / 60, total % 60)
    }
}

enum Ansi {
    static let reset = "\u{1B}[0m"
    static let bold = "\u{1B}[1m"
    static let dim = "\u{1B}[2m"
    static let red = "\u{1B}[31m"
    static let green = "\u{1B}[32m"
    static let yellow = "\u{1B}[33m"
    static let blue = "\u{1B}[34m"
    static let cyan = "\u{1B}[36m"
}
