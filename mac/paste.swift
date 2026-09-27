import AppKit
import Foundation

// Run as a short-lived helper: keep all advertised item types, not just plain text.
func paste() throws {
    let input = FileHandle.standardInput.readDataToEndOfFile()
    guard let text = String(data: input, encoding: .utf8) else {
        throw NSError(domain: "VoxaPaste", code: 1, userInfo: [NSLocalizedDescriptionKey: "Invalid UTF-8 input"])
    }
    if text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return }
    let board = NSPasteboard.general
    let initialCount = board.changeCount
    var saved: [NSPasteboardItem] = []
    for item in board.pasteboardItems ?? [] {
        let copy = NSPasteboardItem()
        for type in item.types {
            guard let data = item.data(forType: type) else {
                throw NSError(domain: "VoxaPaste", code: 2, userInfo: [NSLocalizedDescriptionKey: "Cannot snapshot clipboard; paste cancelled"])
            }
            copy.setData(data, forType: type)
        }
        saved.append(copy)
    }
    guard board.changeCount == initialCount else {
        throw NSError(domain: "VoxaPaste", code: 3, userInfo: [NSLocalizedDescriptionKey: "Clipboard changed during snapshot; paste cancelled"])
    }
    board.clearContents()
    guard board.setString(text, forType: .string) else {
        // We still own the cleared clipboard; try to recover the snapshot.
        if !saved.isEmpty { board.writeObjects(saved) }
        throw NSError(domain: "VoxaPaste", code: 4, userInfo: [NSLocalizedDescriptionKey: "Cannot stage transcript"])
    }
    let stagedCount = board.changeCount
    defer {
        // Key injection completion is not an acknowledgement from the target app.
        // Allow its asynchronous clipboard read before restoring, including on errors.
        Thread.sleep(forTimeInterval: 0.25)
        if board.changeCount == stagedCount {
            board.clearContents()
            if !saved.isEmpty && !board.writeObjects(saved) {
                FileHandle.standardError.write(Data("Could not restore clipboard\n".utf8))
            }
        }
    }
    let process = Process()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
    process.arguments = ["-e", "tell application \"System Events\" to key code 9 using command down"]
    try process.run()
    process.waitUntilExit()
    if process.terminationStatus != 0 {
        throw NSError(domain: "VoxaPaste", code: 5, userInfo: [NSLocalizedDescriptionKey: "Paste keystroke failed; check Automation permission"])
    }
}

do {
    try paste()
} catch {
    FileHandle.standardError.write(Data("\(error.localizedDescription)\n".utf8))
    exit(1)
}
