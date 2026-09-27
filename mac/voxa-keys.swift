import Foundation
import ApplicationServices

// Global F10 hold and F11 toggle. Events pass through to the focused application.
// Grant Accessibility permission to the installed voxa-keys binary in System Settings.
guard CommandLine.arguments.count == 3 else { exit(2) }
let node = CommandLine.arguments[1]
let cli = CommandLine.arguments[2]

func send(_ command: String) {
    let task = Process()
    task.executableURL = URL(fileURLWithPath: node)
    task.arguments = [cli, command]
    task.standardOutput = FileHandle.nullDevice
    task.standardError = FileHandle.nullDevice
    do { try task.run() } catch { fputs("voxa-keys: \(error)\n", stderr) }
}

let callback: CGEventTapCallBack = { _, type, event, _ in
    if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput { return Unmanaged.passUnretained(event) }
    let code = event.getIntegerValueField(.keyboardEventKeycode)
    let repeatKey = event.getIntegerValueField(.keyboardEventAutorepeat) != 0
    if code == 109 { // F10
        if type == .keyDown && !repeatKey { send("start") }
        if type == .keyUp { send("stop") }
    } else if code == 103 && type == .keyDown && !repeatKey { // F11
        send("toggle")
    }
    return Unmanaged.passUnretained(event)
}
let mask = (CGEventMask(1) << CGEventType.keyDown.rawValue) | (CGEventMask(1) << CGEventType.keyUp.rawValue)
guard let tap = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .defaultTap, eventsOfInterest: mask, callback: callback, userInfo: nil) else {
    fputs("voxa-keys: enable Accessibility for voxa-keys in System Settings → Privacy & Security → Accessibility, then restart Voxa\n", stderr)
    exit(1)
}
let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
CFRunLoopAddSource(CFRunLoopGetCurrent(), source, .commonModes)
CGEvent.tapEnable(tap: tap, enable: true)
CFRunLoopRun()
