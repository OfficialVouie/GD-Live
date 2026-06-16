# v1.0.0

- Added native GD-Live overlay engine with progress HUD, click tracker, and session analytics.
- Added Geode settings for overlay visibility, progress/sidebar offsets, chat commands, and request queue size.
- Added in-game Twitch account linking with local token storage.
- Added native Windows Twitch bridge support for reading linked-account chat into the in-game sidebar.
- Added custom GD-Live menus on the main menu and pause menu.
- Added chat level request queue with `Play Next`, `Skip`, and `Clear Queue` controls.
- Added safe chat commands `!randomshake` and `!colorshift` with cooldown and difficulty restrictions.
- Removed the unused clips/highlights surface so the release focuses on Twitch chat and requests.
- Removed the public Twitch Client ID setting and hardened local Twitch auth storage.
