import Foundation
import ApplicationServices

// Command+Shift+R to hold, Command+Shift+T to toggle. Consume these shortcuts so focused apps do not receive them.
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

var holding = false
let modifiers: CGEventFlags = [.maskCommand, .maskShift]
let modifierKeys: CGEventFlags = [.maskControl, .maskCommand, .maskShift, .maskAlternate]

func keyCommand(_ code: Int64, _ type: CGEventType, _ flags: CGEventFlags, _ repeated: Bool) -> String? {
    if code == 15 { // R
        if type == .keyUp && holding { holding = false; return "stop" }
        if type == .keyDown && !repeated && !holding && flags.intersection(modifierKeys) == modifiers {
            holding = true
            return "start"
        }
    } else if code == 17 && type == .keyDown && !repeated && flags.intersection(modifierKeys) == modifiers { // T
        return "toggle"
    }
    return nil
}

#if SHORTCUT_TEST
assert(keyCommand(15, .keyDown, modifiers, false) == "start")
assert(keyCommand(15, .keyDown, modifiers, true) == nil)
assert(keyCommand(15, .keyUp, [], false) == "stop") // Releasing modifiers first still stops.
assert(keyCommand(15, .keyDown, [.maskCommand], false) == nil)
assert(keyCommand(15, .keyDown, [.maskCommand, .maskShift, .maskControl], false) == nil)
assert(keyCommand(17, .keyDown, modifiers, false) == "toggle")
assert(keyCommand(17, .keyDown, modifiers, true) == nil)
exit(0)
#endif

let callback: CGEventTapCallBack = { _, type, event, _ in
    if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput { return Unmanaged.passUnretained(event) }
    let code = event.getIntegerValueField(.keyboardEventKeycode)
    let matched = event.flags.intersection(modifierKeys) == modifiers
    if let command = keyCommand(code, type, event.flags, event.getIntegerValueField(.keyboardEventAutorepeat) != 0) {
        send(command)
        return nil
    }
    if (code == 15 && holding && type == .keyDown) || (code == 17 && matched && (type == .keyDown || type == .keyUp)) { return nil }
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
