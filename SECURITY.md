# Security policy

Wireless Battery Monitor runs without administrator rights, makes no network connections and only sends read-only
battery queries to devices it recognises by vendor and product ID.

## Reporting a vulnerability

Please report security problems privately through
[GitHub private vulnerability reporting](https://github.com/vlad-rey/wireless-battery-monitor/security/advisories/new)
rather than in a public issue. Include the version or commit, what an attacker can do, and how to reproduce it.

Examples of what counts: a driver that writes to a device it does not recognise or changes its settings, a crafted
language file or device response that crashes the app or runs code, or a build that downloads or runs something
unexpected.

## Supported versions

Only the latest release and the `main` branch get fixes.

## Downloads

Only download builds from this repository's [Releases](https://github.com/vlad-rey/wireless-battery-monitor/releases)
page, or build from source. Releases are not code-signed.
