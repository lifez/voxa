import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.Commons
import qs.Ui

// A separate layer-shell surface: unlike a notification or the shared volume
// OSD, this remains visible throughout recording without stealing focus.
Item {
  id: root
  property bool opened: false
  property string state: "idle"
  property string label: ""
  property bool pulse: false

  function open(payloadJson) {
    var next = "recording"
    try { next = String(JSON.parse(payloadJson || "{}").state || "recording") } catch (e) {}
    setState(next)
  }
  function setState(next) {
    if (["recording", "committing", "done", "error"].indexOf(next) < 0) return
    state = next
    label = next === "recording" ? "Recording…" : next === "committing" ? "Transcribing…" : next === "done" ? "Pasted" : "Dictation failed"
    opened = true
    autoHide.stop()
    if (next === "done" || next === "error") autoHide.start()
    else if (next === "recording") safety.start()
  }
  function close() { opened = false; state = "idle"; autoHide.stop(); safety.stop() }

  Timer { id: autoHide; interval: 1300; onTriggered: root.close() }
  // Avoid a stale recording badge if the daemon crashes mid-keypress.
  Timer { id: safety; interval: 120000; onTriggered: root.close() }
  Timer { interval: 500; running: root.opened && root.state === "recording"; repeat: true; onTriggered: root.pulse = !root.pulse }

  PanelWindow {
    visible: root.opened
    anchors { top: true; bottom: true; left: true; right: true }
    color: "transparent"
    WlrLayershell.namespace: "voxa-osd"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
    exclusionMode: ExclusionMode.Ignore
    mask: Region {}

    BorderSurface {
      id: card
      width: Style.space(205)
      height: Style.space(50)
      anchors.horizontalCenter: parent.horizontalCenter
      anchors.bottom: parent.bottom
      anchors.bottomMargin: Style.space(67)
      color: Util.alpha(Color.background, 0.97)
      borderSpec: Border.surfaceSpec("popups", "border", Color.popups.border, Math.max(1, Style.space(2)))
      radius: Style.cornerRadius

      Row {
        anchors.centerIn: parent
        spacing: Style.space(12)
        Text {
          text: root.state === "recording" ? "●" : root.state === "committing" ? "󰔟" : root.state === "done" ? "✓" : "!"
          font.family: Style.font.family
          font.pixelSize: Style.font.title
          color: root.state === "recording" ? (root.pulse ? "#ff6868" : "#b94040") : Color.accent
        }
        Text {
          text: root.label
          font.family: Style.font.family
          font.bold: true
          font.pixelSize: Style.font.title
          color: Color.popups.text
        }
      }
    }
  }
}
