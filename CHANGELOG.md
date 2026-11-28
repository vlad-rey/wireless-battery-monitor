# Changelog

All notable changes to this fork. Versions follow [Semantic Versioning](https://semver.org/).

## [1.0.2] - 2026-11-28

### Fixed

- Auto-hidden taskbar: Windows exposes the tray to UI Automation only while the taskbar is shown, so the widget fell
  back to `TrayNotifyWnd` and covered the clock (seen with Windhawk's Windows 11 Taskbar Styler on a 4K monitor). The
  last tray position measured while the taskbar was shown is now kept while it is hidden, saved for the next start
  (per taskbar width and DPI), and applied as soon as it is measured instead of on the next timer tick.
- Auto-hidden taskbar: the widget sat in the few pixels a hidden taskbar leaves on screen and took the mouse, so
  pointing there did not bring the taskbar up. It now lets the mouse through while the taskbar is hidden.
- Secondary taskbars that are auto-hidden no longer put the widget over the clock before their tray was measured.

## [1.0.1] - 2026-11-23

### Fixed

- *Start with Windows* did nothing on Windows 11 builds that only start Run entries Explorer has marked as enabled.
  The app and the installer now also write that flag (`Explorer\StartupApproved\Run`), and the menu shows autostart
  as off when it was disabled in Task Manager or Settings.

## [1.0.0] - 2026-11-23

First release of this fork, based on
[fuyue521/Wireless-Device-Battery-Monitor-CN](https://github.com/fuyue521/Wireless-Device-Battery-Monitor-CN) 1.0.0,
itself based on krisgfx's Wireless Battery.

### Added

- Ukrainian translation (`lang/uk.txt`).
- `build_msvc.bat`: build with Visual Studio 2022 or its Build Tools.
- Issue templates, security policy, code of conduct and this changelog.

### Fixed

- Windows 11: the taskbar widget covered the clock on builds where `TrayNotifyWnd` starts right of the XAML tray. The
  widget now sits left of the first tray button found through UI Automation.
- Rongyuan receivers: a sleeping device disappeared from the taskbar. It now stays, shown as asleep with the level the
  receiver still reports (AJAZZ AJ159 Apex on `3151:5007`).
- Varmilo keyboards over Bluetooth LE were shown as a second mouse.
- The code compiles with MSVC as well as MinGW.

### Security

- `headsetcontrol.exe` is only taken from the app's folder or an absolute `PATH` entry, never from the current
  directory, and is started by full path.

### Changed

- English interface by default (the Windows display language when a translation exists).
- All devices are shown side by side by default; one-device mode stays a menu option.
- Documentation, comments, the version resource and the installer are in English.

### Removed

- The "Request support for a device" window and `--report`: they sent device data to the original author's website.
  Use the issue templates instead.
- The "Support the project" menu item (linked to the original author's sponsor page).
- GitHub Actions workflows and the Discord release script.
