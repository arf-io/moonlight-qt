# Moonlight desktop additions

This fork retains Moonlight's existing Qt Quick desktop settings and visual identity. Runtime controls and the active Qt style remain the canonical sources for colors, fonts, spacing, focus indicators, and disabled states. No new visual tokens are introduced.

The desktop workflow extends `app/gui/SettingsView.qml` in the Input Settings group. Reuse the existing full-width Qt Quick CheckBox, 12-point text, five-unit column spacing, and native keyboard interaction. Strings use `qsTr` and the existing translation provider. A focused control exposes the same help as a hovered control.

The clipboard option is explicit and off by default. Its description names supported content and says that host enablement is required. Transfer runs only during a streaming session, has no initial clipboard overwrite, preserves later local changes over stale replies, and never logs clipboard contents. File URLs are not file transfer. Network and clipboard policy belongs to the paired-host protocol and the `DesktopClipboard` class; UI state does not grant server access.

Canonical owners: settings persistence is `StreamingPreferences`; checkbox behavior is Qt Quick Controls; existing settings scroll ownership stays unchanged; TLS identity is the existing paired host certificate. The change adds no route, modal, dropdown, table, toast, palette, or animation.

The optional audio/video control follows the same CheckBox and help pattern in Audio Settings. It requests a negotiated capture-time protocol extension and may add bounded video delay; stock hosts retain their existing format. Media buffering and the audio clock are owned by MediaClock and Pacer, not the QML control.

The local cursor option reuses the existing Input Settings CheckBox. It defaults on, applies in remote desktop mouse mode and is disabled in the form when that mode is off. Its help explains host cursor capture and the lack of remote shape updates. Existing Ctrl+Alt+Shift+C remains a per-session override.
