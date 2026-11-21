// SPDX-License-Identifier: GPL-2.0-only
// Bypass charging toggle in the quick settings: battery charging is held
// off and a connected charger powers the tablet directly.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {QuickToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const PATH = '/sys/class/power_supply/qcom-battmgr-bat/charge_behaviour';
const ICON = 'battery-full-charging-symbolic';

function bypassActive(file) {
    try {
        const [, data] = file.load_contents(null);
        // The selected behaviour is shown in brackets.
        return new TextDecoder().decode(data).includes('[inhibit-charge]');
    } catch (e) {
        return false;
    }
}

function setBypass(file, on) {
    try {
        // sysfs attributes take a plain write; no temporary file.
        const s = file.append_to(Gio.FileCreateFlags.NONE, null);
        s.write_all(new TextEncoder().encode(on ? 'inhibit-charge' : 'auto'), null);
        s.close(null);
    } catch (e) {
        console.warn(`bypass-charging: ${e.message}`);
    }
}

const BypassToggle = GObject.registerClass(
class BypassToggle extends QuickToggle {
    _init(file) {
        super._init({
            title: 'Bypass Charging',
            subtitle: 'Battery not charged',
            iconName: ICON,
            toggleMode: true,
        });
        this._file = file;
        this.checked = bypassActive(file);
        this.connect('notify::checked', () => {
            if (this.checked !== bypassActive(this._file))
                setBypass(this._file, this.checked);
        });
    }

    sync() {
        this.checked = bypassActive(this._file);
    }
});

const BypassIndicator = GObject.registerClass(
class BypassIndicator extends SystemIndicator {
    _init(file) {
        super._init();
        this._indicator = this._addIndicator();
        this._indicator.iconName = ICON;
        this.toggle = new BypassToggle(file);
        this.toggle.bind_property('checked', this._indicator, 'visible',
            GObject.BindingFlags.SYNC_CREATE);
        this.quickSettingsItems.push(this.toggle);
    }
});

export default class BypassChargingExtension extends Extension {
    enable() {
        this._file = Gio.File.new_for_path(PATH);
        if (!this._file.query_exists(null))
            return;
        this._indicator = new BypassIndicator(this._file);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
        // Follow a change made elsewhere, such as from a terminal.
        this._poll = GLib.timeout_add_seconds(GLib.PRIORITY_LOW, 5, () => {
            this._indicator.toggle.sync();
            return GLib.SOURCE_CONTINUE;
        });
    }

    disable() {
        if (this._poll) {
            GLib.source_remove(this._poll);
            this._poll = 0;
        }
        if (!this._indicator)
            return;
        // Bypass stays as set across the session lock, like charge limits.
        this._indicator.quickSettingsItems.forEach(i => i.destroy());
        this._indicator.destroy();
        this._indicator = null;
    }
}
