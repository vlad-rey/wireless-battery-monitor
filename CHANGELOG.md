# Changelog

All notable changes to this fork. Versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

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
