// SPDX-License-Identifier: GPL-2.0-only
// Torch toggle in the quick settings for the rear flash LEDs.

import Gio from 'gi://Gio';
import GObject from 'gi://GObject';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {QuickToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

// Torch mode brightness; the LED driver caps it at the torch current.
const LEDS = ['white:flash-0', 'white:flash-1'];
const ICON = 'camera-flash-symbolic';

function ledFiles() {
    return LEDS.map(l => Gio.File.new_for_path(`/sys/class/leds/${l}/brightness`))
        .filter(f => f.query_exists(null));
}

function setLeds(files, on) {
    for (const f of files) {
        try {
            // sysfs attributes take a plain write; no temporary file.
            const s = f.append_to(Gio.FileCreateFlags.NONE, null);
            s.write_all(new TextEncoder().encode(on ? '255' : '0'), null);
            s.close(null);
        } catch (e) {
            console.warn(`torch: ${f.get_path()}: ${e.message}`);
        }
    }
}

const TorchToggle = GObject.registerClass(
class TorchToggle extends QuickToggle {
    _init(files) {
        super._init({title: 'Torch', iconName: ICON, toggleMode: true});
        this._files = files;
        this.connect('notify::checked', () => setLeds(this._files, this.checked));
    }
});

const TorchIndicator = GObject.registerClass(
class TorchIndicator extends SystemIndicator {
    _init(files) {
        super._init();
        this._indicator = this._addIndicator();
        this._indicator.iconName = ICON;
        this.toggle = new TorchToggle(files);
        this.toggle.bind_property('checked', this._indicator, 'visible',
            GObject.BindingFlags.SYNC_CREATE);
        this.quickSettingsItems.push(this.toggle);
    }
});

export default class TorchExtension extends Extension {
    enable() {
        this._files = ledFiles();
        if (!this._files.length)
            return;
        this._indicator = new TorchIndicator(this._files);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        if (!this._indicator)
            return;
        // The torch goes off with the session lock, as the toggle does.
        setLeds(this._files, false);
        this._indicator.quickSettingsItems.forEach(i => i.destroy());
        this._indicator.destroy();
        this._indicator = null;
    }
}
