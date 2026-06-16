# Support

## Twitch setup

Open `Geode > GD Live > Settings`, paste your Twitch account link or username, then press `Link Twitch`. Twitch opens in your browser and shows a code-entry page. After authorizing, GD Live stores the login locally and auto-starts chat on future launches.

For chat display, GD Live starts the native Windows Twitch bridge automatically while Geometry Dash is open.

Use the `Files` button next to the Twitch account setting to open the folder that contains `twitch-auth.json`, `chat-bridge.txt`, and `level-requests.tsv`.

## Troubleshooting

- If linking says Twitch is not configured, the build was packaged without the official Twitch application Client ID. Install the official release or ask the developer to rebuild it correctly.
- If linking says the Twitch app ID is invalid or Twitch returns `invalid client`, the build was packaged with the wrong Client ID. Rebuild with the public Client ID from the Twitch Developer Console, not the placeholder, app name, or Client Secret.
- If linking or startup says `Twitch app must be public`, the Twitch developer app is using a confidential-client setup. GD Live is a client mod and must use Twitch's public device-code client setup; rebuild with that public Client ID, then unlink/link once.
- If chat stays on `starting`, update to the latest build. GD-Live uses a native Windows chat bridge, so users do not need Python installed.
- If chat does not appear, confirm `Enable Chat Sidebar` is on and the bridge is writing to `chat-bridge.txt` in this mod's Geode save folder.
- If Twitch worked before but later says the session expired, use `Unlink` and `Link` once. Normal restarts should reuse and refresh the saved login automatically.
- If chat commands do nothing, confirm `Enable Chat Commands` is on. Commands are intentionally blocked on Demon / 10-star levels.

## Level requests

Enable `Enable Level Requests`, then chat can use `!request <level id>`, `!level <level id>`, or `!lr <level id>`. Use the GD Live `Queue` button in the hub or pause menu to open the full queue, play any request, remove bad requests, clear the list, or refresh it from disk.

## Privacy

GD Live stores Twitch tokens only in the user's local Geode save folder and tightens file permissions where possible. Do not share `twitch-auth.json`, screenshots of it, or OAuth tokens.
