# Changelog

All notable changes to the PN5180 ESP Reader firmware are documented here.
Versions follow the `VERSION` define in the sketches; versions before 2.4.0
were not tagged in git.

## [2.4.0] - 2026-10-23

### Added
- Serial commands `w0` and `w1` to turn WiFi and MQTT off and back on at
  runtime. With networking off the reader works serial-only, like the
  original sketch. The setting is not persisted: WiFi and MQTT are always
  enabled again after a reboot.
- When `w0` is sent while MQTT is connected, `offline` is published to the
  availability topic before disconnecting, so Home Assistant shows the
  device as unavailable instead of stale.
- The `w` status command now also reports whether networking is enabled.
- Dim blue heartbeat LED while networking is disabled, so it isn't
  mistaken for a WiFi fault (red).
- GitHub Actions CI that compiles both sketches for the Wemos D1 mini on
  every push and pull request, with all tool and library versions pinned,
  and fails on any compiler warning.

### Changed
- No amber "publish failed" flash after a tag scan while networking is
  disabled, since nothing is meant to be sent.

## [2.3.0] - 2026-10-09

### Changed
- Tag removal timeout lowered from 5 s to 2 s by default.

### Added
- "Tag removal timeout" number entity in Home Assistant (0.5–30 s), so the
  timeout can be adjusted without reflashing. The value is retained on the
  broker and re-applied after a reboot.

### Fixed
- MQTT buffer enlarged to 768 bytes so the larger discovery payloads publish
  reliably.

## [2.2.0] - 2026-10-09

### Added
- When a tag (e.g. a Tonie) is lifted off the reader and stays away for the
  removal timeout, an empty payload is published to the `tag_scanned` topic,
  so automations can stop playback. A brief misread never triggers it.

## [2.1.0] - 2026-09-18

### Added
- MQTT heartbeat: device uptime published to `<device>/heartbeat` every
  30 s (retained), so a hung or rebooted device is visible, not just a
  disconnected one.
- Firmware version published to `<device>/version` (retained).
- Home Assistant diagnostic sensors for heartbeat and firmware version,
  grouped under the same device as the tag scanner.
- "Heartbeat LED" switch in Home Assistant to turn the heartbeat blip off.
  It only affects the LED; the watchdog always stays active.

### Changed
- Switched to semantic versioning.

### Fixed
- Replaced per-poll `String` handling of tag UIDs with fixed buffers to
  avoid heap fragmentation on long uptimes.

## [2.0-wifi] - 2026-09-18

### Added
- New `Pn5180Esp_WiFi_MQTT` sketch: connects to WiFi, publishes each scanned
  tag UID over MQTT, and registers itself in Home Assistant via MQTT Tag
  discovery, so the reader runs standalone without a host PC. The original
  `v`/`u`/`l`/`i` serial commands are unchanged; a new `w` command shows
  WiFi/MQTT status.
- WiFi/MQTT credentials live in a gitignored `secrets.h`, with
  `secrets.h.example` as the template.
- Status LED heartbeat every 2 s: green = WiFi and MQTT up, amber = MQTT
  down, red = WiFi down. Tag scans flash cyan for 500 ms.
- Self-recovery watchdog on its own hardware timer that restarts the board
  if the main loop hangs for 8 s (the PN5180 library's SPI waits have no
  timeout).
- Wiring diagram and step-by-step setup instructions in the README.

### Fixed
- Both sketches failed to compile against the upstream PN5180 library
  (`disablePrivacyMode()` replaced with `unlockICODESLIX2()`).
- Device flapped to `offline` in Home Assistant shortly after connecting:
  the MQTT client ID is now stable, and the discovery payload no longer
  exceeds PubSubClient's default buffer.
- Corrupted or partial tag reads (UIDs like `0000000000000078`) are
  filtered out before reaching MQTT.

## [1.0] - 2023-02-15

### Added
- Original serial-only `Pn5180Esp` sketch for the Wemos D1 mini with PN5180
  reader, KiCad PCB and 3D-printable housing.

[2.4.0]: https://github.com/StefanRiese/PN5180_ESP_Reader/releases/tag/v2.4.0
