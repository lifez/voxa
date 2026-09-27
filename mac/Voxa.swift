import AppKit
import AVFoundation
import ApplicationServices
import ServiceManagement
import Darwin

// Microphone capture lives in this app process, not in ffmpeg or the Node child: TCC grants the app itself.
final class Voxa: NSObject, NSApplicationDelegate {
    private var server: Int32 = -1
    private var client: Int32 = -1
    private var ownsSocket = false
    private var engine: AVAudioEngine?
    private var node: Process?
    private var nodeExecutable = ""
    private var eventTap: CFMachPort?
    private var holding = false
    private let commands = DispatchQueue(label: "voxa.shortcuts")
    private let writer = DispatchQueue(label: "voxa.audio.writer")
    private var item: NSStatusItem!
    private let socketURL = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Caches/voxa/mic.sock")

    func applicationDidFinishLaunching(_ notification: Notification) {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.button?.title = "Voxa"
        let menu = NSMenu()
        menu.addItem(NSMenuItem(title: "Enable / Retry Shortcuts", action: #selector(enableShortcuts), keyEquivalent: ""))
        menu.addItem(NSMenuItem(title: "Test microphone (speak for 2 seconds)", action: #selector(testMic), keyEquivalent: ""))
        menu.addItem(NSMenuItem(title: "Quit Voxa", action: #selector(quit), keyEquivalent: "q"))
        item.menu = menu
        do {
            if SMAppService.mainApp.status == .notRegistered { try SMAppService.mainApp.register() }
        } catch {
            NSLog("Voxa: enable Open at Login in System Settings: %@", String(describing: error))
        }
        AVCaptureDevice.requestAccess(for: .audio) { allowed in
            DispatchQueue.main.async {
                guard allowed else {
                    self.item.button?.title = "Voxa: mic denied"
                    NSLog("Voxa: grant Microphone access to Voxa in System Settings → Privacy & Security")
                    return
                }
                do { try self.start() }
                catch { self.item.button?.title = "Voxa: error"; NSLog("Voxa: %@", String(describing: error)) }
            }
        }
    }

    private func start() throws {
        let directory = socketURL.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        // A second instance must not replace the live instance's socket.
        if FileManager.default.fileExists(atPath: socketURL.path) {
            let probe = socket(AF_UNIX, SOCK_STREAM, 0)
            defer { close(probe) }
            var address = sockaddr_un()
            address.sun_family = sa_family_t(AF_UNIX)
            let capacity = MemoryLayout.size(ofValue: address.sun_path)
            socketURL.path.withCString { path in
                withUnsafeMutablePointer(to: &address.sun_path) { pointer in
                    _ = strncpy(UnsafeMutableRawPointer(pointer).assumingMemoryBound(to: CChar.self), path, capacity - 1)
                }
            }
            let connected = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(probe, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) } }
            if connected == 0 { throw NSError(domain: "Voxa already running", code: 1) }
            try FileManager.default.removeItem(at: socketURL)
        }
        server = socket(AF_UNIX, SOCK_STREAM, 0)
        guard server >= 0 else { throw NSError(domain: "socket", code: Int(errno)) }
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        guard socketURL.path.utf8.count < MemoryLayout.size(ofValue: address.sun_path) else { throw NSError(domain: "socket path too long", code: 1) }
        let capacity = MemoryLayout.size(ofValue: address.sun_path)
        socketURL.path.withCString { path in
            withUnsafeMutablePointer(to: &address.sun_path) { pointer in
                _ = strncpy(UnsafeMutableRawPointer(pointer).assumingMemoryBound(to: CChar.self), path, capacity - 1)
            }
        }
        let bound = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.bind(server, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) } }
        guard bound == 0, chmod(socketURL.path, 0o600) == 0, listen(server, 1) == 0 else { throw NSError(domain: "mic socket", code: Int(errno)) }
        ownsSocket = true
        signal(SIGPIPE, SIG_IGN)
        let resources = Bundle.main.resourceURL!
        let executable = try String(contentsOf: resources.appendingPathComponent("node-path"), encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines)
        nodeExecutable = executable
        let task = Process()
        task.executableURL = URL(fileURLWithPath: executable)
        task.arguments = [resources.appendingPathComponent("dist/index.js").path, "daemon"]
        var env = ProcessInfo.processInfo.environment
        env["VOXA_MAC_APP"] = "1"
        env["PATH"] = URL(fileURLWithPath: executable).deletingLastPathComponent().path + ":/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
        task.environment = env
        let log = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/voxa.log")
        if !FileManager.default.fileExists(atPath: log.path) { FileManager.default.createFile(atPath: log.path, contents: nil) }
        let output = try FileHandle(forWritingTo: log)
        output.seekToEndOfFile()
        task.standardOutput = output
        task.standardError = output
        task.terminationHandler = { process in
            DispatchQueue.main.async {
                if NSApp.isRunning { self.item.button?.title = "Voxa: daemon stopped (see log)" }
            }
        }
        try task.run()
        node = task
        enableShortcuts()
        DispatchQueue.global(qos: .userInitiated).async {
            while self.server >= 0 {
                let fd = accept(self.server, nil, nil)
                if fd < 0 { break }
                DispatchQueue.main.async { self.connected(fd) }
                // One recording at a time. Wait for the client to disconnect before accepting another.
                var byte: UInt8 = 0
                while read(fd, &byte, 1) > 0 {}
                DispatchQueue.main.sync { self.disconnected(fd) }
                close(fd)
            }
        }
    }

    private func connected(_ fd: Int32) {
        guard client < 0 else { close(fd); return }
        client = fd
        let audio = AVAudioEngine()
        var tapInstalled = false
        do {
            let input = audio.inputNode
            let format = input.outputFormat(forBus: 0)
            guard let pcm = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: 16000, channels: 1, interleaved: true),
                  let converter = AVAudioConverter(from: format, to: pcm) else { throw NSError(domain: "audio format", code: 1) }
            input.installTap(onBus: 0, bufferSize: 1024, format: format) { buffer, _ in
                let capacity = AVAudioFrameCount(ceil(Double(buffer.frameLength) * 16000 / format.sampleRate)) + 32
                guard let output = AVAudioPCMBuffer(pcmFormat: pcm, frameCapacity: capacity) else { return }
                var supplied = false
                var error: NSError?
                converter.convert(to: output, error: &error) { _, status in
                    if supplied { status.pointee = .noDataNow; return nil }
                    supplied = true
                    status.pointee = .haveData
                    return buffer
                }
                if let error { NSLog("Voxa: conversion: %@", String(describing: error)); return }
                let bytes = Int(output.frameLength) * Int(pcm.streamDescription.pointee.mBytesPerFrame)
                guard bytes > 0, let samples = output.int16ChannelData else { return }
                let data = Data(bytes: samples[0], count: bytes)
                self.writer.async { data.withUnsafeBytes { raw in
                    guard let base = raw.baseAddress else { return }
                    var offset = 0
                    while offset < raw.count {
                        let n = Darwin.write(fd, base.advanced(by: offset), raw.count - offset)
                        if n <= 0 { break }
                        offset += n
                    }
                } }
            }
            tapInstalled = true
            try audio.start()
            engine = audio
        } catch {
            if tapInstalled { audio.inputNode.removeTap(onBus: 0) }
            audio.stop()
            audioFailure(error)
            shutdown(fd, SHUT_RDWR)
        }
    }

    private func audioFailure(_ error: Error) {
        NSLog("Voxa: microphone start failed: %@", String(describing: error))
    }

    private func disconnected(_ fd: Int32) {
        engine?.stop()
        engine?.inputNode.removeTap(onBus: 0)
        engine = nil
        writer.sync {} // Drain queued PCM before the socket is closed or its fd reused.
        if client == fd { client = -1 }
    }

    @objc private func enableShortcuts() {
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        guard AXIsProcessTrustedWithOptions(options) else {
            item.button?.title = "Voxa: allow Accessibility"
            NSLog("Voxa: grant Accessibility to Voxa.app (not voxa-keys), then use Enable / Retry Shortcuts")
            return
        }
        if let eventTap { CGEvent.tapEnable(tap: eventTap, enable: true); item.button?.title = "Voxa"; return }
        let mask = (CGEventMask(1) << CGEventType.keyDown.rawValue) | (CGEventMask(1) << CGEventType.keyUp.rawValue)
        guard let tap = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .defaultTap,
                                          eventsOfInterest: mask, callback: shortcutCallback,
                                          userInfo: Unmanaged.passUnretained(self).toOpaque()) else {
            item.button?.title = "Voxa: shortcuts unavailable"
            NSLog("Voxa: event tap failed; check Accessibility and Input Monitoring for Voxa.app")
            return
        }
        eventTap = tap
        let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)
        item.button?.title = "Voxa"
        NSLog("Voxa: shortcuts active")
    }

    fileprivate func shortcut(_ type: CGEventType, _ event: CGEvent) -> Unmanaged<CGEvent>? {
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            NSLog("Voxa: shortcut tap disabled; re-enabling")
            if let eventTap { CGEvent.tapEnable(tap: eventTap, enable: true) }
            return Unmanaged.passUnretained(event)
        }
        let code = event.getIntegerValueField(.keyboardEventKeycode)
        let flags = event.flags.intersection([.maskCommand, .maskShift, .maskControl, .maskAlternate])
        let matched = flags == [.maskCommand, .maskShift]
        let repeated = event.getIntegerValueField(.keyboardEventAutorepeat) != 0
        var command: String?
        if code == 15 { // Command+Shift+R: hold to record
            if type == .keyUp && holding { holding = false; command = "stop" }
            else if type == .keyDown && matched && !holding && !repeated { holding = true; command = "start" }
        } else if code == 17 && matched && type == .keyDown && !repeated { command = "toggle" }
        if let command {
            let executable = nodeExecutable
            let cli = Bundle.main.resourceURL!.appendingPathComponent("dist/index.js").path
            commands.async {
                let task = Process()
                task.executableURL = URL(fileURLWithPath: executable)
                task.arguments = [cli, command]
                task.standardOutput = FileHandle.nullDevice
                task.standardError = FileHandle.nullDevice
                do { try task.run(); task.waitUntilExit() }
                catch { NSLog("Voxa: shortcut %@ failed: %@", command, String(describing: error)) }
            }
            return nil
        }
        if (code == 15 && holding && type == .keyDown) || (code == 17 && matched && (type == .keyDown || type == .keyUp)) { return nil }
        return Unmanaged.passUnretained(event)
    }

    @objc private func testMic() {
        guard !nodeExecutable.isEmpty else { return }
        let task = Process()
        task.executableURL = URL(fileURLWithPath: nodeExecutable)
        task.arguments = [Bundle.main.resourceURL!.appendingPathComponent("dist/index.js").path, "test-mic"]
        var env = ProcessInfo.processInfo.environment
        env["VOXA_MAC_APP"] = "1"
        task.environment = env
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = pipe
        item.button?.title = "Voxa: testing…"
        task.terminationHandler = { process in
            let output = String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
            NSLog("Voxa microphone test: %@", output)
            DispatchQueue.main.async {
                if let peak = output.range(of: "peak ").flatMap({ Int(output[$0.upperBound...].trimmingCharacters(in: .whitespacesAndNewlines)) }), peak > 0, process.terminationStatus == 0 {
                    self.item.button?.title = "Voxa: mic OK (peak \(peak))"
                } else { self.item.button?.title = "Voxa: mic silent/error (see log)" }
            }
        }
        do { try task.run() } catch { item.button?.title = "Voxa: test failed"; NSLog("Voxa: %@", String(describing: error)) }
    }

    @objc private func quit() { NSApp.terminate(nil) }
    func applicationWillTerminate(_ notification: Notification) {
        if let eventTap { CGEvent.tapEnable(tap: eventTap, enable: false) }
        node?.terminate()
        if client >= 0 { shutdown(client, SHUT_RDWR) }
        if server >= 0 { close(server); server = -1 }
        if ownsSocket { try? FileManager.default.removeItem(at: socketURL) }
    }
}

private let shortcutCallback: CGEventTapCallBack = { _, type, event, userInfo in
    guard let userInfo else { return Unmanaged.passUnretained(event) }
    return Unmanaged<Voxa>.fromOpaque(userInfo).takeUnretainedValue().shortcut(type, event)
}

let app = NSApplication.shared
let delegate = Voxa()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
