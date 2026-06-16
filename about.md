# GD Live

GD Live is a native streaming companion for Geometry Dash. It adds in-game overlays, session stats, click feedback, Twitch chat display, safe chat-triggered effects, and level request tools without requiring a pile of OBS-only widgets.

## Features

- Native progress HUD with top/bottom positioning and offset controls.
- Click tracker with total clicks, click timing, and pulse feedback.
- Session analytics with level name, attempt count, best percent, elapsed time, and completion rate.
- Twitch account linking from the Geode settings page using Twitch's device-code authorization flow.
- Twitch chat sidebar via the native Windows chat bridge.
- Custom GD-Live control menu from the main menu and pause menu.
- Chat level request queue with `!request`, `!level`, and `!lr` commands plus a `Play Next` button.
- Optional chat commands, disabled by default, cooldown-gated, and blocked on Demon / 10-star runs.

## Privacy

GD Live does not collect analytics, upload gameplay data, or send data to a custom server.

When Twitch linking is used, Twitch account authorization is handled by Twitch directly. The returned token is saved only on the user's device in this mod's Geode save folder with private-file hardening where supported. Twitch chat uses the read-only `chat:read` scope.
