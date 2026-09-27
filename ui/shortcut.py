#!/usr/bin/env python3
"""Small GTK4 key recorder. Prints only the confirmed shortcut to stdout."""
import re
import sys

import gi
gi.require_version('Gtk', '4.0')
gi.require_version('Gdk', '4.0')
from gi.repository import Gdk, Gtk

ALIASES = {
    'Return': 'RETURN', 'KP_Enter': 'KP_Enter', 'space': 'SPACE',
    'BackSpace': 'BACKSPACE', 'Tab': 'TAB', 'Delete': 'DELETE',
    'Escape': 'ESCAPE', 'Prior': 'PAGE_UP', 'Next': 'PAGE_DOWN',
    'Up': 'UP', 'Down': 'DOWN', 'Left': 'LEFT', 'Right': 'RIGHT',
    'Home': 'HOME', 'End': 'END', 'Insert': 'INSERT',
}
MODIFIER_KEYS = {
    'Shift_L', 'Shift_R', 'Control_L', 'Control_R', 'Alt_L', 'Alt_R',
    'Super_L', 'Super_R', 'Meta_L', 'Meta_R', 'ISO_Level3_Shift',
}


def format_key(keyval, state):
    name = Gdk.keyval_name(keyval)
    if not name or name in MODIFIER_KEYS:
        return None
    # Do not emit punctuation or text altered by Shift (e.g. ! instead of 1).
    # These names match Hyprland key symbols and the shortcut() validator.
    if name in ALIASES:
        key = ALIASES[name]
    elif re.fullmatch(r'F(?:[1-9]|[12][0-9]|3[0-5])', name):
        key = name
    elif re.fullmatch(r'[a-zA-Z0-9]', name):
        key = name.upper()
    elif re.fullmatch(r'XF86[A-Za-z0-9_]+', name):
        key = name
    else:
        return None
    mods = []
    if state & Gdk.ModifierType.SUPER_MASK:
        mods.append('SUPER')
    if state & Gdk.ModifierType.CONTROL_MASK:
        mods.append('CTRL')
    if state & Gdk.ModifierType.ALT_MASK:
        mods.append('ALT')
    if state & Gdk.ModifierType.SHIFT_MASK:
        mods.append('SHIFT')
    return ' + '.join([*mods, key])


class Recorder(Gtk.Application):
    def __init__(self):
        super().__init__(application_id='io.github.voxa.shortcut_recorder')
        self.selected = None
        self.waiting = False

    def do_activate(self):
        window = Gtk.ApplicationWindow(application=self, title='Voxa — Record Shortcut')
        window.set_default_size(420, 190)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14)
        box.set_margin_top(20)
        box.set_margin_bottom(20)
        box.set_margin_start(20)
        box.set_margin_end(20)
        title = sys.argv[1] if len(sys.argv) > 1 else 'Shortcut'
        current = sys.argv[2] if len(sys.argv) > 2 else '(not set)'
        box.append(Gtk.Label(label=f'{title}\nCurrent: {current}'))
        self.message = Gtk.Label(label='Click Record, then press your key combination.')
        self.message.set_wrap(True)
        box.append(self.message)
        row = Gtk.Box(spacing=10)
        record = Gtk.Button(label='Record shortcut')
        record.connect('clicked', self.start_recording)
        row.append(record)
        save = Gtk.Button(label='Use shortcut')
        save.set_sensitive(False)
        save.connect('clicked', self.confirm)
        self.save_button = save
        row.append(save)
        cancel = Gtk.Button(label='Type manually instead')
        cancel.connect('clicked', lambda _: window.close())
        row.append(cancel)
        box.append(row)
        controller = Gtk.EventControllerKey()
        controller.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        controller.connect('key-pressed', self.on_key)
        window.add_controller(controller)
        window.set_child(box)
        window.present()

    def start_recording(self, _):
        self.waiting = True
        self.selected = None
        self.save_button.set_sensitive(False)
        self.message.set_text('Press a non-modifier key with optional Ctrl/Alt/Shift/Super. Escape cancels recording.\nIf Hyprland intercepts the shortcut, enter it manually instead.')

    def on_key(self, _controller, keyval, _keycode, state):
        if not self.waiting:
            return False
        if keyval == Gdk.KEY_Escape:
            self.waiting = False
            self.message.set_text('Recording cancelled. Click Record to try again.')
            return True
        selected = format_key(keyval, state)
        if not selected:
            if Gdk.keyval_name(keyval) not in MODIFIER_KEYS:
                self.message.set_text('That key is not supported; try a letter, number, F-key or navigation key.')
            return True
        self.waiting = False
        self.selected = selected
        self.message.set_text(f'Recorded: {selected}\nClick Use shortcut to confirm, or Record to try again.')
        self.save_button.set_sensitive(True)
        return True

    def confirm(self, _):
        if self.selected:
            print(self.selected, flush=True)
            self.quit()


if __name__ == '__main__':
    Recorder().run([])
