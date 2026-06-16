# Twitch Integration

GD-Live now links Twitch accounts from inside Geometry Dash. The in-game flow uses Twitch's device-code authorization, which is designed for games and apps where typing a full login flow is awkward.

## 1. Developer setup before release

Create one Twitch application in the Twitch Developer Console and copy its public Client ID.

GD-Live is a Windows client mod, so do not ship or embed a Twitch Client Secret. For the device-code flow, the Twitch app must be a public client when that option is available. Public device-code clients can refresh access tokens without a Client Secret; confidential clients will link at first, then fail later with `missing client secret` when GD-Live tries to refresh.

For release builds, configure the mod with that Client ID so users do not see or paste any developer setting:

```powershell
cmake -S . -B .\build -DGDLIVE_TWITCH_CLIENT_ID=PASTE_THE_REAL_CLIENT_ID_HERE -DGDLIVE_REQUIRE_TWITCH_CLIENT_ID=ON
cmake --build .\build --config RelWithDebInfo
```

If `GDLIVE_TWITCH_CLIENT_ID` is empty, users cannot link Twitch. GD-Live never asks users for a Client ID or Client Secret.
`GDLIVE_REQUIRE_TWITCH_CLIENT_ID=ON` makes upload builds fail fast when this value is empty; leave it off only for local UI/testing builds.

If linking fails with `invalid client`, the build was packaged with the wrong value, usually the placeholder text, the Client Secret, or an app name. Re-run CMake with the public Client ID shown in the Twitch app details page.

If linking or startup says `Twitch app must be public`, create or update the Twitch app as a public device-code client, rebuild with that Client ID, then unlink/link once in-game.

## 2. User authorization in-game

Users only need to do this:

1. Open `Geode > GD Live > Settings`.
2. Paste their Twitch channel link or username, such as `https://twitch.tv/example`.
3. Press `Link Twitch` / `Connect`.
4. Click `Open Twitch`, enter the shown code, and authorize GD-Live.
5. Return to Geometry Dash. GD-Live saves the login, starts chat, and enables level requests automatically.

GD-Live saves the token locally at:

`%LOCALAPPDATA%\GeometryDash\geode\mods\vouie.gd-live\twitch-auth.json`

Do not commit or share this file.
GD-Live writes this file with user-only permissions where the platform supports it, rejects oversized or malformed auth files, and refuses to write auth through a symlink.

On future launches, users should not need to reconnect. GD-Live validates the saved Twitch login, refreshes the access token when Twitch allows it, and writes the updated token back to the same local file. A user only needs to link again if Twitch revokes the app, the user disconnects GD-Live from Twitch, the build changes to a different Client ID, or the public-client refresh token expires after a long period of inactivity.

## 3. Chat bridge

The mod renders chat from:

`%LOCALAPPDATA%\GeometryDash\geode\mods\vouie.gd-live\chat-bridge.txt`

After Twitch is linked, GD-Live automatically starts its native Windows Twitch chat bridge. It connects to Twitch chat and appends chat lines to that file.

Users do not need Python, a command prompt, a Twitch Client ID, or any manual token setup. The GD-Live menu shows live connection status and includes a `Start Chat` button if they want to reconnect.

Never ask users to paste OAuth tokens into chat, Discord, GitHub issues, or a command prompt.

## 4. GD-Live settings

In Geode settings for GD Live:

- Enable `Enable Overlays`
- Enable `Enable Chat Sidebar` if you want messages shown on-screen
- Enable `Enable Level Requests` if you want chat to queue levels
- Optional: enable `Enable Chat Commands`

Supported commands are `!randomshake` and `!colorshift`. GD-Live cooldown-gates them and blocks them on Demon / 10-star levels.

Level request commands are `!request <level id>`, `!level <level id>`, and `!lr <level id>`. Requests are saved to `level-requests.tsv`, shown in the GD-Live control menu, and opened with the `Play Next` button in the main menu, pause menu, or GD-Live popup.

Use the `Queue` button in the GD-Live hub or pause menu to open the full request list. The queue menu lets streamers play any request, remove bad requests, clear the queue, or refresh the saved queue from disk.

Level requests keep working even if the chat sidebar is disabled.

## Notes

- The bridge uses Twitch IRC over TLS on `irc.chat.twitch.tv:6697`.
- Read-only chat requires the `chat:read` scope.
- Tokens are validated and refreshed automatically when Twitch provides a refresh token from the in-game device-code flow.
- GD-Live stores auth locally only and does not upload Twitch tokens to any custom server.
