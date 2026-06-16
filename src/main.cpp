#include <Geode/Geode.hpp>
#include <Geode/loader/ModEvent.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GJSearchObject.hpp>
#include <Geode/binding/LevelBrowserLayer.hpp>
#include <Geode/binding/MenuLayer.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/Layout.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/Scrollbar.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/general.hpp>
#include <Geode/utils/web.hpp>

#ifdef GEODE_IS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace geode::prelude;
using namespace cocos2d;

#ifndef GDLIVE_TWITCH_CLIENT_ID
#define GDLIVE_TWITCH_CLIENT_ID ""
#endif

namespace gdlive {
    namespace {
        constexpr auto kOverlayID = "gdlive-overlay"_spr;
        constexpr auto kChatBridgeFile = "chat-bridge.txt";
        constexpr auto kBridgeStatusFile = "chat-bridge-status.txt";
        constexpr auto kBridgeReadmeFile = "chat-bridge-readme.txt";
        constexpr auto kTwitchSetupDoc = "twitch-setup.md";
        constexpr auto kTwitchAuthFile = "twitch-auth.json";
        constexpr auto kLevelRequestsFile = "level-requests.tsv";
        constexpr auto kTwitchScopes = "chat:read";
        constexpr auto kTwitchDeviceEndpoint = "https://id.twitch.tv/oauth2/device";
        constexpr auto kTwitchTokenEndpoint = "https://id.twitch.tv/oauth2/token";
        constexpr auto kTwitchValidateEndpoint = "https://id.twitch.tv/oauth2/validate";
        constexpr auto kMaxAuthFileBytes = 64 * 1024;

        using Clock = std::chrono::steady_clock;

        struct Settings {
            bool enableOverlays = true;
            bool showProgressHud = true;
            bool showClickTracker = true;
            bool showSessionAnalytics = true;
            bool enableChat = true;
            bool enableChatCommands = false;
            bool enableLevelRequests = true;
            std::string progressPosition = "Bottom";
            int progressX = 0;
            int progressY = 0;
            int sidebarX = 0;
            int sidebarY = 0;
            double commandCooldown = 8.0;
            int maxRequestQueue = 25;
            std::string twitchChannel;

            static Settings load() {
                auto mod = Mod::get();
                Settings settings;
                settings.enableOverlays = mod->getSettingValue<bool>("enable-overlays");
                settings.showProgressHud = mod->getSettingValue<bool>("show-progress-hud");
                settings.showClickTracker = mod->getSettingValue<bool>("show-click-tracker");
                settings.showSessionAnalytics = mod->getSettingValue<bool>("show-session-analytics");
                settings.enableChat = mod->getSettingValue<bool>("enable-chat");
                settings.enableChatCommands = mod->getSettingValue<bool>("enable-chat-commands");
                settings.enableLevelRequests = mod->getSettingValue<bool>("enable-level-requests");
                settings.progressPosition = mod->getSettingValue<std::string>("progress-position");
                settings.progressX = static_cast<int>(mod->getSettingValue<int64_t>("progress-x"));
                settings.progressY = static_cast<int>(mod->getSettingValue<int64_t>("progress-y"));
                settings.sidebarX = static_cast<int>(mod->getSettingValue<int64_t>("sidebar-x"));
                settings.sidebarY = static_cast<int>(mod->getSettingValue<int64_t>("sidebar-y"));
                settings.commandCooldown = mod->getSettingValue<double>("command-cooldown");
                settings.maxRequestQueue = static_cast<int>(mod->getSettingValue<int64_t>("max-request-queue"));
                settings.twitchChannel = mod->getSettingValue<std::string>("twitch-channel");
                settings.maxRequestQueue = std::clamp(settings.maxRequestQueue, 1, 100);
                settings.commandCooldown = std::clamp(settings.commandCooldown, 2.0, 60.0);
                return settings;
            }
        };

        struct LevelRequest {
            int levelID = 0;
            std::string requester;
            std::string message;
        };

        struct TwitchAuthState {
            std::string clientId;
            std::string username;
            std::string token;
            std::string refreshToken;
            std::string channel;
            int expiresIn = 0;
            int authorizedAt = 0;
        };

        struct TwitchDeviceTicket {
            std::string deviceCode;
            std::string userCode;
            std::string verificationUri;
            int expiresIn = 0;
            int interval = 5;
        };

        struct SessionState {
            PlayLayer* layer = nullptr;
            std::string levelName = "Unknown Level";
            int savedBestAtStart = 0;
            int bestPercent = 0;
            int currentAttempt = 1;
            int runCount = 0;
            int completionCount = 0;
            int clickCount = 0;
            int lastIntervalMs = 0;
            float lastPercent = 0.0f;
            double elapsedSeconds = 0.0;
            Clock::time_point sessionStart = Clock::now();
            Clock::time_point lastClick = Clock::now();

            void resetFor(PlayLayer* playLayer, GJGameLevel* level) {
                layer = playLayer;
                levelName = level ? std::string(level->m_levelName) : "Unknown Level";
                savedBestAtStart = level ? level->getNormalPercent() : 0;
                bestPercent = savedBestAtStart;
                currentAttempt = 1;
                runCount = 0;
                completionCount = 0;
                clickCount = 0;
                lastIntervalMs = 0;
                lastPercent = 0.0f;
                elapsedSeconds = 0.0;
                sessionStart = Clock::now();
                lastClick = sessionStart;
            }

            void updatePercent(float percent) {
                lastPercent = std::clamp(percent, 0.0f, 100.0f);
                bestPercent = std::max(bestPercent, static_cast<int>(std::floor(lastPercent + 0.5f)));
                elapsedSeconds = std::chrono::duration<double>(Clock::now() - sessionStart).count();
            }

            void recordClick() {
                auto now = Clock::now();
                lastIntervalMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - lastClick).count());
                lastClick = now;
                clickCount += 1;
            }

            int attemptsForDisplay(PlayLayer* playLayer) const {
                if (playLayer) {
                    return std::max(1, playLayer->m_attempts);
                }
                return std::max(1, currentAttempt);
            }
        };

        Settings s_settings;
        SessionState s_session;
        std::deque<LevelRequest> s_levelRequests;
        std::vector<std::string> s_chatLines;
        std::vector<std::string> s_pendingCommandMessages;
        std::string s_chatContentCache;
        size_t s_processedChatLines = 0;
        float s_chatPollTimer = 0.0f;
        Clock::time_point s_lastCommandTime = Clock::now() - std::chrono::seconds(120);
        std::atomic_bool s_twitchLinkInProgress = false;
        std::atomic_bool s_bridgeAutoStartInProgress = false;
        std::atomic_bool s_nativeBridgeRunning = false;
        std::atomic_bool s_nativeBridgeStopRequested = false;
        CCObject* s_bridgePoller = nullptr;

        std::string escapeJson(std::string_view input) {
            std::string output;
            output.reserve(input.size() + 8);
            for (char c : input) {
                switch (c) {
                case '\\':
                    output += "\\\\";
                    break;
                case '"':
                    output += "\\\"";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '\r':
                    output += "\\r";
                    break;
                case '\t':
                    output += "\\t";
                    break;
                default:
                    output += c;
                    break;
                }
            }
            return output;
        }

        std::vector<std::string> splitLines(std::string const& text) {
            std::vector<std::string> lines;
            std::stringstream stream(text);
            std::string line;
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                lines.push_back(line);
            }
            return lines;
        }

        bool startsWith(std::string_view text, std::string_view prefix) {
            return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
        }

        std::string trim(std::string line) {
            auto first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                return {};
            }
            auto last = line.find_last_not_of(" \t\r\n");
            return line.substr(first, last - first + 1);
        }

        std::string lowercase(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string normalizeTwitchChannel(std::string channel) {
            channel = trim(std::move(channel));
            if (channel.empty()) {
                return {};
            }

            auto lowered = lowercase(channel);
            auto scheme = lowered.find("://");
            if (scheme != std::string::npos) {
                channel = channel.substr(scheme + 3);
                lowered = lowered.substr(scheme + 3);
            }

            for (auto prefix : {"www.", "m."}) {
                if (startsWith(lowered, prefix)) {
                    channel = channel.substr(std::string_view(prefix).size());
                    lowered = lowered.substr(std::string_view(prefix).size());
                    break;
                }
            }

            if (startsWith(lowered, "twitch.tv/")) {
                channel = channel.substr(10);
            }
            if (auto separator = channel.find_first_of("/?#& "); separator != std::string::npos) {
                channel = channel.substr(0, separator);
            }
            if (!channel.empty() && channel.front() == '#') {
                channel.erase(channel.begin());
            }
            if (!channel.empty() && channel.front() == '@') {
                channel.erase(channel.begin());
            }
            std::transform(channel.begin(), channel.end(), channel.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return channel;
        }

        bool validTwitchChannel(std::string_view channel) {
            if (channel.size() < 3 || channel.size() > 32) {
                return false;
            }
            return std::all_of(channel.begin(), channel.end(),
                               [](unsigned char character) { return std::isalnum(character) != 0 || character == '_'; });
        }

        std::string urlEncode(std::string_view input) {
            std::ostringstream encoded;
            encoded << std::uppercase << std::hex;
            for (auto c : input) {
                auto value = static_cast<unsigned char>(c);
                if (std::isalnum(value) || c == '-' || c == '_' || c == '.' || c == '~') {
                    encoded << c;
                } else {
                    encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(value);
                }
            }
            return encoded.str();
        }

        std::filesystem::path saveDir() {
            return Mod::get()->getSaveDir();
        }

        std::filesystem::path chatBridgePath() {
            return saveDir() / kChatBridgeFile;
        }

        std::filesystem::path bridgeStatusPath() {
            return saveDir() / kBridgeStatusFile;
        }

        std::filesystem::path twitchAuthPath() {
            return saveDir() / kTwitchAuthFile;
        }

        std::filesystem::path levelRequestsPath() {
            return saveDir() / kLevelRequestsFile;
        }

        int unixTimeNow() {
            return static_cast<int>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
        }

        std::string bakedTwitchClientId() {
            return trim(std::string(GDLIVE_TWITCH_CLIENT_ID));
        }

        std::string currentTwitchClientId() {
            return bakedTwitchClientId();
        }

        bool safeStoredValue(std::string_view value, size_t maxLength) {
            if (value.size() > maxLength) {
                return false;
            }
            return std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
        }

        bool safeTwitchAuth(TwitchAuthState const& auth) {
            if (!safeStoredValue(auth.clientId, 128) || !safeStoredValue(auth.username, 64) || !safeStoredValue(auth.channel, 64)) {
                return false;
            }
            if (!safeStoredValue(auth.token, 4096) || !safeStoredValue(auth.refreshToken, 4096)) {
                return false;
            }

            auto clientId = currentTwitchClientId();
            return clientId.empty() || auth.clientId.empty() || auth.clientId == clientId;
        }

        void hardenPrivateFile(std::filesystem::path const& path) {
            std::error_code error;
            std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace, error);
            if (error) {
                log::debug("Could not tighten permissions for {}: {}", path.string(), error.message());
            }
        }

        geode::utils::web::WebResponse postForm(std::string const& url, std::vector<std::pair<std::string, std::string>> const& fields) {
            std::string body;
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i > 0) {
                    body += "&";
                }
                body += urlEncode(fields[i].first);
                body += "=";
                body += urlEncode(fields[i].second);
            }

            auto request = geode::utils::web::WebRequest();
            request.bodyString(body);
            request.header("Content-Type", "application/x-www-form-urlencoded");
            request.timeout(std::chrono::seconds(20));
            return request.postSync(url, Mod::get());
        }

        std::string responseText(geode::utils::web::WebResponse const& response) {
            return response.string().unwrapOr(std::string(response.errorMessage()));
        }

        std::string jsonString(matjson::Value const& json, std::string const& key) {
            return json[key].asString().unwrapOr("");
        }

        int jsonInt(matjson::Value const& json, std::string const& key, int fallback = 0) {
            return static_cast<int>(json[key].asInt().unwrapOr(fallback));
        }

        std::string twitchResponseMessage(geode::utils::web::WebResponse const& response) {
            if (auto parsed = response.json()) {
                auto root = parsed.unwrap();
                auto message = jsonString(root, "message");
                if (!message.empty()) {
                    return message;
                }
                auto error = jsonString(root, "error");
                if (!error.empty()) {
                    return error;
                }
            }
            return trim(responseText(response));
        }

        bool twitchRefreshNeedsPublicClient(std::string const& message) {
            auto lowered = lowercase(message);
            if (lowered.find("must be public") != std::string::npos) {
                return true;
            }
            return lowered.find("missing client secret") != std::string::npos ||
                   lowered.find("missing client_secret") != std::string::npos || lowered.find("client_secret") != std::string::npos;
        }

        std::string twitchPublicClientSetupMessage() {
            return "Twitch app must be public - rebuild GD-Live";
        }

        std::string twitchAuthSetupError(std::string const& twitchMessage, int statusCode) {
            auto lowered = lowercase(twitchMessage);
            if (lowered.find("invalid client") != std::string::npos || lowered.find("client") != std::string::npos) {
                return "Twitch app ID is invalid. Rebuild GD-Live with the real Client ID.";
            }
            if (!twitchMessage.empty()) {
                return fmt::format("Twitch link failed: {}", twitchMessage);
            }
            return fmt::format("Twitch link start failed ({})", statusCode);
        }

        std::optional<TwitchAuthState> loadTwitchAuth() {
            auto path = twitchAuthPath();
            std::error_code error;
            if (std::filesystem::exists(path, error)) {
                auto size = std::filesystem::file_size(path, error);
                if (!error && size > kMaxAuthFileBytes) {
                    log::warn("Ignoring oversized Twitch auth file");
                    return std::nullopt;
                }
            }

            auto content = utils::file::readString(path);
            if (!content) {
                return std::nullopt;
            }

            auto parsed = matjson::parse(content.unwrap());
            if (!parsed) {
                return std::nullopt;
            }

            auto root = parsed.unwrap();
            TwitchAuthState auth;
            auth.clientId = jsonString(root, "client_id");
            auth.username = jsonString(root, "username");
            auth.token = jsonString(root, "token");
            auth.refreshToken = jsonString(root, "refresh_token");
            auth.channel = jsonString(root, "channel");
            auth.expiresIn = jsonInt(root, "expires_in");
            auth.authorizedAt = jsonInt(root, "authorized_at");

            if (auth.username.empty() || auth.token.empty() || !safeTwitchAuth(auth)) {
                return std::nullopt;
            }
            hardenPrivateFile(path);
            return auth;
        }

        bool saveTwitchAuth(TwitchAuthState auth) {
            auth.channel = normalizeTwitchChannel(auth.channel);
            auto path = twitchAuthPath();
            (void)utils::file::createDirectoryAll(path.parent_path());

            std::ostringstream json;
            json << "{\n";
            json << "  \"schema\": \"gdlive.twitch-auth.v1\",\n";
            json << "  \"client_id\": \"" << escapeJson(auth.clientId) << "\",\n";
            json << "  \"username\": \"" << escapeJson(auth.username) << "\",\n";
            json << "  \"channel\": \"" << escapeJson(auth.channel) << "\",\n";
            json << "  \"token\": \"" << escapeJson(auth.token) << "\",\n";
            json << "  \"refresh_token\": \"" << escapeJson(auth.refreshToken) << "\",\n";
            json << "  \"scopes\": [\"chat:read\"],\n";
            json << "  \"expires_in\": " << auth.expiresIn << ",\n";
            json << "  \"authorized_at\": " << auth.authorizedAt << "\n";
            json << "}\n";

            auto write = utils::file::writeString(path, json.str());
            if (!write) {
                log::error("Failed to save Twitch auth: {}", write.unwrapErr());
                return false;
            }
            hardenPrivateFile(path);
            return true;
        }

        void syncTwitchChannelToAuth() {
            if (auto auth = loadTwitchAuth()) {
                auto channel = normalizeTwitchChannel(s_settings.twitchChannel);
                if (!channel.empty()) {
                    auth->channel = channel;
                }
                (void)saveTwitchAuth(*auth);
            }
        }

        void stopManagedBridgeProcesses() {
            s_nativeBridgeStopRequested = true;
            (void)utils::file::writeString(bridgeStatusPath(), "stopped\n");
        }

        void clearTwitchAuth() {
            stopManagedBridgeProcesses();
            std::error_code error;
            std::filesystem::remove(twitchAuthPath(), error);
            Notification::create(error ? "Could not unlink Twitch" : "Twitch unlinked",
                                 error ? NotificationIcon::Error : NotificationIcon::Info, 1.4f)
                ->show();
        }

        std::string twitchLinkStatusText() {
            if (auto auth = loadTwitchAuth()) {
                return fmt::format("Linked: {}", auth->username);
            }
            if (currentTwitchClientId().empty()) {
                return "Update needed - Twitch unavailable";
            }
            return "Not linked";
        }

        std::string shortenStatus(std::string status, size_t maxLength = 72) {
            status = trim(std::move(status));
            if (status.size() <= maxLength) {
                return status;
            }
            if (maxLength <= 3) {
                return status.substr(0, maxLength);
            }
            return status.substr(0, maxLength - 3) + "...";
        }

        std::string cleanBridgeStatusLine(std::string line) {
            line = trim(std::move(line));
            if (startsWith(line, "[bridge] ")) {
                line = line.substr(9);
            }
            if (startsWith(line, "[bridge ")) {
                auto end = line.find("] ");
                if (end != std::string::npos) {
                    line = line.substr(end + 2);
                }
            }
            return shortenStatus(std::move(line));
        }

        std::optional<int> fileAgeSeconds(std::filesystem::path const& path) {
            std::error_code error;
            auto writtenAt = std::filesystem::last_write_time(path, error);
            if (error) {
                return std::nullopt;
            }

            auto age = decltype(writtenAt)::clock::now() - writtenAt;
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(age).count();
            if (seconds < 0) {
                seconds = 0;
            }
            if (seconds > std::numeric_limits<int>::max()) {
                return std::numeric_limits<int>::max();
            }
            return static_cast<int>(seconds);
        }

        std::string lastNonEmptyLine(std::string const& text) {
            auto lines = splitLines(text);
            for (auto iterator = lines.rbegin(); iterator != lines.rend(); ++iterator) {
                auto line = trim(*iterator);
                if (!line.empty()) {
                    return line;
                }
            }
            return {};
        }

        void writeBridgeStatus(std::string status) {
            status = trim(std::move(status));
            if (status.empty()) {
                status = "not started";
            }
            (void)utils::file::writeString(bridgeStatusPath(), status + "\n");
        }

        void appendBridgeFileLine(std::string line) {
            line = trim(std::move(line));
            if (line.empty()) {
                return;
            }

            std::error_code error;
            std::filesystem::create_directories(chatBridgePath().parent_path(), error);
            std::ofstream bridge(chatBridgePath(), std::ios::app);
            if (!bridge) {
                return;
            }

            std::replace(line.begin(), line.end(), '\r', ' ');
            std::replace(line.begin(), line.end(), '\n', ' ');
            bridge << line << '\n';
        }

        void writeNativeBridgeStatus(std::string status) {
            status = trim(std::move(status));
            if (status.empty()) {
                status = "not started";
            }
            writeBridgeStatus(status);
            appendBridgeFileLine(fmt::format("[bridge] {}", status));
        }

        std::string currentBridgeStatusLine() {
            if (auto status = utils::file::readString(bridgeStatusPath())) {
                auto line = cleanBridgeStatusLine(lastNonEmptyLine(status.unwrap()));
                if (!line.empty()) {
                    return line;
                }
            }

            if (auto chat = utils::file::readString(chatBridgePath())) {
                auto line = cleanBridgeStatusLine(lastNonEmptyLine(chat.unwrap()));
                if (!line.empty()) {
                    return line;
                }
            }

            return {};
        }

        bool bridgeLooksRunning() {
            auto line = lowercase(currentBridgeStatusLine());
            if (!startsWith(line, "connected")) {
                return false;
            }
            return fileAgeSeconds(bridgeStatusPath()).value_or(std::numeric_limits<int>::max()) <= 45;
        }

        std::string bridgeRuntimeStatusText() {
            if (!loadTwitchAuth()) {
                return "Bridge: link Twitch first";
            }

            auto line = currentBridgeStatusLine();
            if (!line.empty()) {
                if (startsWith(lowercase(line), "connected") && !bridgeLooksRunning()) {
                    return "Bridge: reconnecting...";
                }
                return fmt::format("Bridge: {}", line);
            }

            return "Bridge: waiting to auto-start";
        }

        void notifyOnMain(std::string message, NotificationIcon icon) {
            geode::queueInMainThread([message = std::move(message), icon] { Notification::create(message.c_str(), icon, 2.0f)->show(); });
        }

        std::optional<TwitchDeviceTicket> requestTwitchDeviceCode(std::string const& clientId, std::string& error) {
            auto response = postForm(kTwitchDeviceEndpoint, {{"client_id", clientId}, {"scopes", kTwitchScopes}});
            auto parsed = response.json();
            if (!response.ok() || !parsed) {
                auto message = twitchResponseMessage(response);
                error = twitchAuthSetupError(message, response.code());
                log::warn("Twitch device request failed: {}", responseText(response));
                return std::nullopt;
            }

            auto root = parsed.unwrap();
            TwitchDeviceTicket ticket;
            ticket.deviceCode = jsonString(root, "device_code");
            ticket.userCode = jsonString(root, "user_code");
            ticket.verificationUri = jsonString(root, "verification_uri");
            ticket.expiresIn = std::max(30, jsonInt(root, "expires_in", 1800));
            ticket.interval = std::clamp(jsonInt(root, "interval", 5), 2, 30);
            if (ticket.deviceCode.empty() || ticket.userCode.empty() || ticket.verificationUri.empty()) {
                error = "Twitch did not return a valid link code";
                return std::nullopt;
            }
            return ticket;
        }

        std::optional<TwitchAuthState> validateTwitchAccessToken(std::string const& clientId, std::string const& token,
                                                                 std::string const& refreshToken, int expiresIn,
                                                                 std::string const& channelName, std::string& error) {
            auto request = geode::utils::web::WebRequest();
            request.header("Authorization", fmt::format("OAuth {}", token));
            request.timeout(std::chrono::seconds(20));
            auto response = request.getSync(kTwitchValidateEndpoint, Mod::get());
            auto parsed = response.json();
            if (!response.ok() || !parsed) {
                error = "Twitch token validation failed";
                log::warn("Twitch token validation failed: {}", responseText(response));
                return std::nullopt;
            }

            auto root = parsed.unwrap();
            auto username = jsonString(root, "login");
            if (username.empty()) {
                error = "Twitch did not return an account name";
                return std::nullopt;
            }

            auto responseClientId = jsonString(root, "client_id");
            if (!responseClientId.empty() && responseClientId != clientId) {
                error = "Twitch token belongs to a different app";
                return std::nullopt;
            }

            auto validatedExpiresIn = jsonInt(root, "expires_in", expiresIn);
            return TwitchAuthState{
                clientId,           username,     token, refreshToken, normalizeTwitchChannel(channelName.empty() ? username : channelName),
                validatedExpiresIn, unixTimeNow()};
        }

        std::optional<TwitchAuthState> refreshTwitchAccessToken(TwitchAuthState const& auth, std::string& error) {
            auto clientId = currentTwitchClientId();
            if (clientId.empty()) {
                error = "Twitch is not configured in this build";
                return std::nullopt;
            }
            if (auth.refreshToken.empty()) {
                error = "Twitch session expired - relink once";
                return std::nullopt;
            }

            auto response = postForm(kTwitchTokenEndpoint,
                                     {{"grant_type", "refresh_token"}, {"refresh_token", auth.refreshToken}, {"client_id", clientId}});
            auto parsed = response.json();
            if (!response.ok() || !parsed) {
                auto message = twitchResponseMessage(response);
                if (twitchRefreshNeedsPublicClient(message)) {
                    error = twitchPublicClientSetupMessage();
                } else if (message.empty()) {
                    error = "Twitch session expired - relink once";
                } else {
                    error = fmt::format("Twitch refresh failed: {}", message);
                }
                log::warn("Twitch token refresh failed: {}", responseText(response));
                return std::nullopt;
            }

            auto root = parsed.unwrap();
            auto token = jsonString(root, "access_token");
            if (token.empty()) {
                error = "Twitch returned an empty refreshed token";
                return std::nullopt;
            }

            auto refreshToken = jsonString(root, "refresh_token");
            if (refreshToken.empty()) {
                refreshToken = auth.refreshToken;
            }
            auto expiresIn = jsonInt(root, "expires_in", 14400);
            return validateTwitchAccessToken(clientId, token, refreshToken, expiresIn, auth.channel, error);
        }

        std::optional<TwitchAuthState> ensureUsableTwitchAuth(std::string& error) {
            auto auth = loadTwitchAuth();
            if (!auth) {
                error = "Link Twitch first";
                return std::nullopt;
            }

            auto clientId = currentTwitchClientId();
            if (clientId.empty()) {
                error = "Twitch is not configured in this build";
                return std::nullopt;
            }

            std::string validationError;
            if (auto validated =
                    validateTwitchAccessToken(clientId, auth->token, auth->refreshToken, auth->expiresIn, auth->channel, validationError)) {
                if (validated->channel.empty()) {
                    validated->channel = normalizeTwitchChannel(auth->channel.empty() ? validated->username : auth->channel);
                }
                (void)saveTwitchAuth(*validated);
                return validated;
            }

            writeNativeBridgeStatus("refreshing Twitch session...");
            if (auto refreshed = refreshTwitchAccessToken(*auth, error)) {
                (void)saveTwitchAuth(*refreshed);
                return refreshed;
            }

            if (error.empty()) {
                error = validationError.empty() ? "Twitch session expired - relink once" : validationError;
            }
            return std::nullopt;
        }

        std::optional<TwitchAuthState> pollTwitchDeviceToken(std::string const& clientId, TwitchDeviceTicket const& ticket,
                                                             std::string const& channelName, std::string& error) {
            auto interval = std::clamp(ticket.interval, 2, 30);
            auto deadline = Clock::now() + std::chrono::seconds(ticket.expiresIn);

            while (Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::seconds(interval));

                auto response = postForm(kTwitchTokenEndpoint, {{"client_id", clientId},
                                                                {"scopes", kTwitchScopes},
                                                                {"device_code", ticket.deviceCode},
                                                                {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}});

                auto parsed = response.json();
                if (response.ok() && parsed) {
                    auto root = parsed.unwrap();
                    auto token = jsonString(root, "access_token");
                    if (token.empty()) {
                        error = "Twitch returned an empty access token";
                        return std::nullopt;
                    }
                    auto refreshToken = jsonString(root, "refresh_token");
                    auto expiresIn = jsonInt(root, "expires_in", 14400);
                    return validateTwitchAccessToken(clientId, token, refreshToken, expiresIn, channelName, error);
                }

                std::string twitchError;
                if (parsed) {
                    auto root = parsed.unwrap();
                    twitchError = jsonString(root, "error");
                    if (twitchError.empty()) {
                        twitchError = jsonString(root, "message");
                    }
                }

                if (twitchError == "authorization_pending" || twitchError.find("pending") != std::string::npos) {
                    continue;
                }
                if (twitchError == "slow_down" || twitchError.find("slow") != std::string::npos) {
                    interval = std::min(interval + 5, 30);
                    continue;
                }

                error = twitchError.empty() ? fmt::format("Twitch authorization failed ({})", response.code()) : twitchError;
                log::warn("Twitch token poll failed: {}", responseText(response));
                return std::nullopt;
            }

            error = "Twitch link code expired";
            return std::nullopt;
        }

        bool levelAllowsChatEffects(PlayLayer* layer) {
            if (!layer || !layer->m_level) {
                return false;
            }
            auto level = layer->m_level;
            auto demon = static_cast<int>(level->m_demon);
            auto stars = static_cast<int>(level->m_stars);
            return demon == 0 && stars < 10;
        }

        void pushChatLine(std::string line) {
            line = trim(std::move(line));
            if (line.empty() || startsWith(line, "#")) {
                return;
            }
            s_chatLines.push_back(std::move(line));
            while (s_chatLines.size() > 6) {
                s_chatLines.erase(s_chatLines.begin());
            }
        }

        std::string cleanQueueField(std::string value) {
            std::replace(value.begin(), value.end(), '\t', ' ');
            std::replace(value.begin(), value.end(), '\r', ' ');
            std::replace(value.begin(), value.end(), '\n', ' ');
            return trim(std::move(value));
        }

        bool levelRequestQueued(int levelID) {
            return std::any_of(s_levelRequests.begin(), s_levelRequests.end(),
                               [levelID](LevelRequest const& request) { return request.levelID == levelID; });
        }

        void saveLevelRequests() {
            std::ostringstream data;
            for (auto const& request : s_levelRequests) {
                data << request.levelID << '\t' << cleanQueueField(request.requester) << '\t' << cleanQueueField(request.message) << '\n';
            }
            auto write = utils::file::writeString(levelRequestsPath(), data.str());
            if (!write) {
                log::warn("Failed to save level request queue: {}", write.unwrapErr());
            }
        }

        void loadLevelRequests() {
            s_levelRequests.clear();
            auto content = utils::file::readString(levelRequestsPath());
            if (!content) {
                return;
            }

            for (auto const& rawLine : splitLines(content.unwrap())) {
                auto line = trim(rawLine);
                if (line.empty() || startsWith(line, "#")) {
                    continue;
                }

                std::stringstream stream(line);
                std::string idText;
                std::string requester;
                std::string message;
                std::getline(stream, idText, '\t');
                std::getline(stream, requester, '\t');
                std::getline(stream, message);

                try {
                    auto id = std::stoi(trim(idText));
                    if (id > 0 && !levelRequestQueued(id) && static_cast<int>(s_levelRequests.size()) < s_settings.maxRequestQueue) {
                        s_levelRequests.push_back({id, cleanQueueField(requester), cleanQueueField(message)});
                    }
                } catch (...) {
                    log::warn("Skipped malformed GD-Live level request: {}", line);
                }
            }
        }

        std::optional<int> extractLevelID(std::string_view text) {
            std::string digits;
            for (char c : text) {
                if (std::isdigit(static_cast<unsigned char>(c))) {
                    if (digits.size() < 10) {
                        digits += c;
                    }
                } else if (!digits.empty()) {
                    break;
                }
            }

            if (digits.empty()) {
                return std::nullopt;
            }

            try {
                auto value = std::stoll(digits);
                if (value <= 0 || value > std::numeric_limits<int>::max()) {
                    return std::nullopt;
                }
                return static_cast<int>(value);
            } catch (...) {
                return std::nullopt;
            }
        }

        std::optional<LevelRequest> parseLevelRequest(std::string const& line) {
            if (!s_settings.enableLevelRequests) {
                return std::nullopt;
            }

            auto trimmed = trim(line);
            auto separator = trimmed.find(':');
            auto requester = separator == std::string::npos ? std::string("chat") : cleanQueueField(trimmed.substr(0, separator));
            auto message = trim(separator == std::string::npos ? trimmed : trimmed.substr(separator + 1));
            auto lowered = lowercase(message);

            for (auto command : {"!request", "!level", "!lr"}) {
                auto commandLength = std::string_view(command).size();
                auto commandPos = lowered.find(command);
                while (commandPos != std::string::npos) {
                    auto beforeOk = commandPos == 0 || std::isspace(static_cast<unsigned char>(lowered[commandPos - 1]));
                    auto afterIndex = commandPos + commandLength;
                    auto afterOk = afterIndex >= lowered.size() || std::isspace(static_cast<unsigned char>(lowered[afterIndex]));
                    if (beforeOk && afterOk) {
                        break;
                    }
                    commandPos = lowered.find(command, commandPos + 1);
                }

                if (commandPos == std::string::npos) {
                    continue;
                }

                auto id = extractLevelID(message.substr(commandPos + commandLength));
                if (!id) {
                    pushChatLine("GD-Live: request needs a level ID");
                    return std::nullopt;
                }

                if (requester.empty()) {
                    requester = "chat";
                }
                return LevelRequest{*id, requester, cleanQueueField(message)};
            }

            return std::nullopt;
        }

        void enqueueLevelRequest(LevelRequest request) {
            if (request.levelID <= 0) {
                return;
            }

            if (levelRequestQueued(request.levelID)) {
                pushChatLine(fmt::format("GD-Live: level {} is already queued", request.levelID));
                return;
            }

            if (static_cast<int>(s_levelRequests.size()) >= s_settings.maxRequestQueue) {
                pushChatLine(fmt::format("GD-Live: request queue full ({})", s_settings.maxRequestQueue));
                return;
            }

            s_levelRequests.push_back(std::move(request));
            saveLevelRequests();
            auto const& queued = s_levelRequests.back();
            pushChatLine(fmt::format("GD-Live: queued {} from {} (#{})", queued.levelID, queued.requester, s_levelRequests.size()));
        }

        void processLevelRequest(std::string const& line) {
            if (auto request = parseLevelRequest(line)) {
                enqueueLevelRequest(std::move(*request));
            }
        }

        CCNode* findNodeByIDRecursive(CCNode* root, char const* id) {
            if (!root || !id) {
                return nullptr;
            }
            if (root->getID() == id) {
                return root;
            }
            if (auto direct = root->getChildByID(id)) {
                return direct;
            }

            for (auto child : root->getChildrenExt()) {
                if (auto found = findNodeByIDRecursive(child, id)) {
                    return found;
                }
            }
            return nullptr;
        }

        CCMenu* findMenuByID(CCNode* root, std::initializer_list<char const*> ids) {
            for (auto id : ids) {
                if (auto menu = typeinfo_cast<CCMenu*>(findNodeByIDRecursive(root, id))) {
                    return menu;
                }
            }
            return nullptr;
        }

        ccColor4B rgba(int r, int g, int b, int a) {
            return {static_cast<GLubyte>(std::clamp(r, 0, 255)), static_cast<GLubyte>(std::clamp(g, 0, 255)),
                    static_cast<GLubyte>(std::clamp(b, 0, 255)), static_cast<GLubyte>(std::clamp(a, 0, 255))};
        }

        CCLayerColor* colorBlock(ccColor4B color, CCSize size, CCPoint position) {
            auto block = CCLayerColor::create(color, size.width, size.height);
            block->setAnchorPoint({0.0f, 0.0f});
            block->setPosition(position);
            return block;
        }

        CCMenuItemSpriteExtra* createGDLiveButton(char const* label, char const* texture, CCObject* target, SEL_MenuHandler selector,
                                                  float scale = 0.52f) {
            auto text = std::string(label ? label : "");
            auto compact = scale < 0.46f;
            auto width = std::clamp(42 + static_cast<int>(text.size()) * (compact ? 5 : 6), compact ? 56 : 68, compact ? 94 : 118);
            auto height = compact ? 24.0f : 28.0f;
            auto textScale = std::clamp(scale * 0.88f, 0.34f, 0.54f);
            auto background = texture ? texture : "GJ_button_01.png";
            auto button = CCMenuItemSpriteExtra::create(
                ButtonSprite::create(text.c_str(), width, 0, textScale, true, "bigFont.fnt", background, height),
                ButtonSprite::create(text.c_str(), width, 0, textScale, true, "bigFont.fnt", background, height), target, selector);
            button->setPosition({0.0f, 0.0f});
            return button;
        }

        CCMenu* createLayoutMenu(CCNode* parent, char const* id, CCPoint position, CCSize size, int zOrder = 20) {
            auto menu = CCMenu::create();
            menu->setID(id);
            menu->setPosition(position);
            menu->setContentSize(size);
            menu->setAnchorPoint({0.5f, 0.5f});
            menu->setLayout(RowLayout::create()
                                ->setGap(8.0f)
                                ->setAutoScale(true)
                                ->setAxisAlignment(AxisAlignment::Center)
                                ->setCrossAxisAlignment(AxisAlignment::Center));
            parent->addChild(menu, zOrder);
            return menu;
        }

        void addButtonToMenu(CCMenu* menu, CCMenuItemSpriteExtra* button) {
            if (!menu || !button) {
                return;
            }
            button->setPosition({0.0f, 0.0f});
            menu->addChild(button);
            menu->updateLayout();
        }

        CCPoint clampPanelPosition(CCPoint desired, CCSize panelSize, CCSize winSize, float margin = 8.0f) {
            auto maxX = std::max(margin, winSize.width - panelSize.width - margin);
            auto maxY = std::max(margin, winSize.height - panelSize.height - margin);
            return {std::clamp(desired.x, margin, maxX), std::clamp(desired.y, margin, maxY)};
        }

        std::string nextRequestLabel() {
            if (s_levelRequests.empty()) {
                return "Queue empty";
            }
            auto const& next = s_levelRequests.front();
            return fmt::format("Next: {} by {}", next.levelID, next.requester.empty() ? "chat" : next.requester);
        }

        void skipNextLevelRequest() {
            if (s_levelRequests.empty()) {
                Notification::create("Request queue is empty", NotificationIcon::Info, 1.4f)->show();
                return;
            }

            auto skipped = s_levelRequests.front();
            s_levelRequests.pop_front();
            saveLevelRequests();
            pushChatLine(fmt::format("GD-Live: skipped level {}", skipped.levelID));
            Notification::create(fmt::format("Skipped level {}", skipped.levelID).c_str(), NotificationIcon::Info, 1.4f)->show();
        }

        void removeLevelRequestAt(size_t index) {
            if (index >= s_levelRequests.size()) {
                Notification::create("That request is gone", NotificationIcon::Info, 1.2f)->show();
                return;
            }

            auto removed = s_levelRequests[index];
            s_levelRequests.erase(s_levelRequests.begin() + static_cast<std::ptrdiff_t>(index));
            saveLevelRequests();
            pushChatLine(fmt::format("GD-Live: removed request {}", removed.levelID));
            Notification::create(fmt::format("Removed level {}", removed.levelID).c_str(), NotificationIcon::Info, 1.2f)->show();
        }

        void clearLevelRequests() {
            s_levelRequests.clear();
            saveLevelRequests();
            pushChatLine("GD-Live: request queue cleared");
            Notification::create("Level request queue cleared", NotificationIcon::Info, 1.4f)->show();
        }

        void openLevelRequestAt(size_t index) {
            if (index >= s_levelRequests.size()) {
                Notification::create("No requested levels queued", NotificationIcon::Info, 1.4f)->show();
                return;
            }

            auto request = s_levelRequests[index];
            s_levelRequests.erase(s_levelRequests.begin() + static_cast<std::ptrdiff_t>(index));
            saveLevelRequests();

            auto searchObject = GJSearchObject::create(SearchType::Search, gd::string(std::to_string(request.levelID)));
            if (!searchObject) {
                s_levelRequests.insert(s_levelRequests.begin() + static_cast<std::ptrdiff_t>(std::min(index, s_levelRequests.size())),
                                       std::move(request));
                saveLevelRequests();
                Notification::create("Could not open requested level", NotificationIcon::Error, 1.8f)->show();
                return;
            }

            auto scene = LevelBrowserLayer::scene(searchObject);
            if (!scene) {
                s_levelRequests.insert(s_levelRequests.begin() + static_cast<std::ptrdiff_t>(std::min(index, s_levelRequests.size())),
                                       std::move(request));
                saveLevelRequests();
                Notification::create("Could not open requested level", NotificationIcon::Error, 1.8f)->show();
                return;
            }

            pushChatLine(fmt::format("GD-Live: opening requested level {}", request.levelID));
            CCDirector::get()->replaceScene(CCTransitionFade::create(0.25f, scene));
        }

        void openNextLevelRequest() {
            openLevelRequestAt(0);
        }

        void queueCommandMessage(std::string message) {
            s_pendingCommandMessages.push_back(std::move(message));
        }

        void processChatCommand(PlayLayer* layer, std::string const& line) {
            if (!s_settings.enableChatCommands || !layer) {
                return;
            }

            auto lowered = line;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            auto commandPos = lowered.find('!');
            if (commandPos == std::string::npos) {
                return;
            }

            auto command = lowered.substr(commandPos);
            if (!startsWith(command, "!randomshake") && !startsWith(command, "!colorshift")) {
                return;
            }

            auto now = Clock::now();
            auto sinceLast = std::chrono::duration<double>(now - s_lastCommandTime).count();
            if (sinceLast < s_settings.commandCooldown) {
                queueCommandMessage(fmt::format("GD-Live: command cooled down ({:.0f}s)", s_settings.commandCooldown - sinceLast));
                return;
            }
            if (!levelAllowsChatEffects(layer)) {
                queueCommandMessage("GD-Live: effects locked on Demon/10-star runs");
                return;
            }

            s_lastCommandTime = now;
            if (startsWith(command, "!randomshake")) {
                layer->shakeCamera(0.24f, 0.32f, 0.04f);
                queueCommandMessage("GD-Live: chat triggered camera shake");
            } else if (startsWith(command, "!colorshift")) {
                layer->playFlashEffect(0.28f, 3, 0.35f);
                queueCommandMessage("GD-Live: chat triggered color flash");
            }
        }

        void pollChatBridge(PlayLayer* layer, float dt) {
            if (!s_settings.enableChat && !s_settings.enableLevelRequests && !s_settings.enableChatCommands) {
                return;
            }
            s_chatPollTimer += dt;
            if (s_chatPollTimer < 0.75f) {
                return;
            }
            s_chatPollTimer = 0.0f;

            auto path = chatBridgePath();
            if (!std::filesystem::exists(path)) {
                return;
            }

            auto content = utils::file::readString(path);
            if (!content) {
                return;
            }

            auto text = content.unwrap();
            if (text == s_chatContentCache) {
                return;
            }
            s_chatContentCache = text;

            auto lines = splitLines(s_chatContentCache);
            if (lines.size() < s_processedChatLines) {
                s_processedChatLines = 0;
            }

            for (size_t index = s_processedChatLines; index < lines.size(); ++index) {
                auto line = trim(lines[index]);
                if (line.empty() || startsWith(line, "#")) {
                    continue;
                }
                if (s_settings.enableChat) {
                    pushChatLine(line);
                }
                processLevelRequest(line);
                processChatCommand(layer, line);
            }
            s_processedChatLines = lines.size();
        }

        void flushCommandMessagesToChat() {
            for (auto& message : s_pendingCommandMessages) {
                pushChatLine(std::move(message));
            }
            s_pendingCommandMessages.clear();
        }

        class BridgePoller : public CCObject {
          public:
            void tick(float dt) {
                pollChatBridge(nullptr, dt);
                flushCommandMessagesToChat();
            }
        };

        void installBridgePoller() {
            if (s_bridgePoller) {
                return;
            }

            auto director = CCDirector::get();
            if (!director) {
                return;
            }

            auto scheduler = director->getScheduler();
            if (!scheduler) {
                return;
            }

            auto poller = new BridgePoller();
            poller->retain();
            s_bridgePoller = poller;
            scheduler->scheduleSelector(schedule_selector(BridgePoller::tick), poller, 0.25f, false);
        }

        std::string normalizeOAuthToken(std::string token) {
            token = trim(std::move(token));
            if (startsWith(token, "oauth:")) {
                return token;
            }
            return fmt::format("oauth:{}", token);
        }

        std::optional<std::pair<std::string, std::string>> parseIrcPrivmsg(std::string line) {
            std::string displayName;
            if (startsWith(line, "@")) {
                auto tagsEnd = line.find(' ');
                auto tags = tagsEnd == std::string::npos ? line.substr(1) : line.substr(1, tagsEnd - 1);
                std::stringstream tagStream(tags);
                std::string tag;
                while (std::getline(tagStream, tag, ';')) {
                    auto separator = tag.find('=');
                    if (separator != std::string::npos && tag.substr(0, separator) == "display-name") {
                        displayName = tag.substr(separator + 1);
                        break;
                    }
                }
                line = tagsEnd == std::string::npos ? std::string() : line.substr(tagsEnd + 1);
            }

            std::string prefix;
            if (startsWith(line, ":")) {
                auto prefixEnd = line.find(' ');
                prefix = prefixEnd == std::string::npos ? line.substr(1) : line.substr(1, prefixEnd - 1);
                line = prefixEnd == std::string::npos ? std::string() : line.substr(prefixEnd + 1);
            }

            auto commandEnd = line.find(' ');
            auto command = commandEnd == std::string::npos ? line : line.substr(0, commandEnd);
            if (command != "PRIVMSG") {
                return std::nullopt;
            }

            auto messageStart = line.find(" :");
            if (messageStart == std::string::npos) {
                return std::nullopt;
            }

            if (displayName.empty() && !prefix.empty()) {
                displayName = prefix.substr(0, prefix.find('!'));
            }
            if (displayName.empty()) {
                displayName = "viewer";
            }

            auto message = line.substr(messageStart + 2);
            if (message.empty()) {
                return std::nullopt;
            }
            return std::make_pair(displayName, message);
        }

#ifdef GEODE_IS_WINDOWS
        struct WinHttpHandle {
            HINTERNET handle = nullptr;

            WinHttpHandle() = default;
            explicit WinHttpHandle(HINTERNET value) : handle(value) {}
            WinHttpHandle(WinHttpHandle const&) = delete;
            WinHttpHandle& operator=(WinHttpHandle const&) = delete;

            WinHttpHandle(WinHttpHandle&& other) noexcept : handle(other.handle) {
                other.handle = nullptr;
            }

            WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
                if (this != &other) {
                    reset();
                    handle = other.handle;
                    other.handle = nullptr;
                }
                return *this;
            }

            ~WinHttpHandle() {
                reset();
            }

            void reset(HINTERNET value = nullptr) {
                if (handle) {
                    WinHttpCloseHandle(handle);
                }
                handle = value;
            }

            HINTERNET release() {
                auto value = handle;
                handle = nullptr;
                return value;
            }

            explicit operator bool() const {
                return handle != nullptr;
            }
        };

        struct NativeBridgeRunGuard {
            ~NativeBridgeRunGuard() {
                s_nativeBridgeRunning = false;
            }
        };

        bool sendWebSocketText(HINTERNET webSocket, std::string const& text) {
            auto result = WinHttpWebSocketSend(webSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(text.data()),
                                               static_cast<DWORD>(text.size()));
            return result == NO_ERROR;
        }

        bool sendIrcCommand(HINTERNET webSocket, std::string command) {
            command += "\r\n";
            return sendWebSocketText(webSocket, command);
        }

        void handleNativeIrcLine(HINTERNET webSocket, std::string line) {
            line = trim(std::move(line));
            if (line.empty()) {
                return;
            }

            if (startsWith(line, "PING ")) {
                (void)sendIrcCommand(webSocket, fmt::format("PONG {}", line.substr(5)));
                return;
            }

            if (line.find("Login authentication failed") != std::string::npos ||
                line.find("Improperly formatted auth") != std::string::npos) {
                writeNativeBridgeStatus("disconnected: Twitch login failed, relink account");
                s_nativeBridgeStopRequested = true;
                return;
            }

            if (auto parsed = parseIrcPrivmsg(std::move(line))) {
                auto formatted = fmt::format("{}: {}", parsed->first, parsed->second);
                appendBridgeFileLine(formatted);
                writeBridgeStatus(fmt::format("received chat from {}", parsed->first));
            }
        }

        bool runNativeTwitchBridge(TwitchAuthState auth) {
            NativeBridgeRunGuard guard;
            s_nativeBridgeStopRequested = false;

            auto channel = normalizeTwitchChannel(auth.channel.empty() ? auth.username : auth.channel);
            auto username = lowercase(auth.username);
            if (channel.empty() || username.empty() || auth.token.empty()) {
                writeNativeBridgeStatus("disconnected: Twitch auth is incomplete, relink account");
                return false;
            }

            writeNativeBridgeStatus(fmt::format("connecting to twitch channel #{}", channel));

            WinHttpHandle session(
                WinHttpOpen(L"GD-Live/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
            if (!session) {
                writeNativeBridgeStatus("disconnected: could not start Windows network session");
                return false;
            }

            WinHttpSetTimeouts(session.handle, 30000, 30000, 30000, 5000);

            WinHttpHandle connection(WinHttpConnect(session.handle, L"irc-ws.chat.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, 0));
            if (!connection) {
                writeNativeBridgeStatus("disconnected: could not reach Twitch chat");
                return false;
            }

            WinHttpHandle request(WinHttpOpenRequest(connection.handle, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
            if (!request) {
                writeNativeBridgeStatus("disconnected: could not create Twitch chat request");
                return false;
            }

            if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
                !WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(request.handle, nullptr)) {
                writeNativeBridgeStatus("disconnected: Twitch chat websocket failed");
                return false;
            }

            WinHttpHandle webSocket(WinHttpWebSocketCompleteUpgrade(request.handle, 0));
            request.release();
            if (!webSocket) {
                writeNativeBridgeStatus("disconnected: Twitch chat upgrade failed");
                return false;
            }

            if (!sendIrcCommand(webSocket.handle, fmt::format("PASS {}", normalizeOAuthToken(auth.token))) ||
                !sendIrcCommand(webSocket.handle, fmt::format("NICK {}", username)) ||
                !sendIrcCommand(webSocket.handle, "CAP REQ :twitch.tv/tags twitch.tv/commands") ||
                !sendIrcCommand(webSocket.handle, fmt::format("JOIN #{}", channel))) {
                writeNativeBridgeStatus("disconnected: could not authenticate Twitch chat");
                return false;
            }

            writeNativeBridgeStatus(fmt::format("connected to twitch channel #{} (waiting for chat)", channel));

            std::array<char, 8192> buffer{};
            std::string frame;
            auto lastHeartbeat = Clock::now();

            while (!s_nativeBridgeStopRequested) {
                DWORD bytesRead = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType{};
                auto result =
                    WinHttpWebSocketReceive(webSocket.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, &bufferType);

                if (result == ERROR_WINHTTP_TIMEOUT) {
                    if (std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - lastHeartbeat).count() >= 15) {
                        writeBridgeStatus(fmt::format("connected to twitch channel #{} (waiting for chat)", channel));
                        lastHeartbeat = Clock::now();
                    }
                    continue;
                }

                if (result != NO_ERROR) {
                    writeNativeBridgeStatus("disconnected: Twitch chat connection dropped");
                    return false;
                }

                if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                    writeNativeBridgeStatus("disconnected: Twitch chat closed");
                    return false;
                }

                if (bytesRead == 0) {
                    continue;
                }

                frame.append(buffer.data(), bytesRead);
                if (bufferType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
                    bufferType == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
                    continue;
                }

                if (bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                    bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                    auto lines = splitLines(frame);
                    for (auto& line : lines) {
                        handleNativeIrcLine(webSocket.handle, std::move(line));
                    }
                    frame.clear();
                }
            }

            WinHttpWebSocketClose(webSocket.handle, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            writeNativeBridgeStatus("stopped");
            return true;
        }
#endif

        void startTwitchBridgeAsync(bool, bool notify) {
            if (!loadTwitchAuth()) {
                if (notify) {
                    Notification::create("Link Twitch first", NotificationIcon::Info, 1.4f)->show();
                }
                return;
            }

            if (bridgeLooksRunning()) {
                if (notify) {
                    Notification::create("Twitch chat already connected", NotificationIcon::Success, 1.2f)->show();
                }
                return;
            }

            if (s_bridgeAutoStartInProgress.exchange(true)) {
                if (notify) {
                    Notification::create("Twitch chat is already starting", NotificationIcon::Info, 1.2f)->show();
                }
                return;
            }

            writeBridgeStatus("starting native chat bridge...");
            if (notify) {
                Notification::create("Starting Twitch chat...", NotificationIcon::Info, 1.2f)->show();
            }

#ifdef GEODE_IS_WINDOWS
            if (s_nativeBridgeRunning.exchange(true)) {
                s_bridgeAutoStartInProgress = false;
                if (notify) {
                    Notification::create("Twitch chat is already starting", NotificationIcon::Info, 1.2f)->show();
                }
                return;
            }

            std::thread([notify] {
                s_bridgeAutoStartInProgress = false;
                std::string error;
                auto auth = ensureUsableTwitchAuth(error);
                if (!auth) {
                    s_nativeBridgeRunning = false;
                    writeNativeBridgeStatus(fmt::format("disconnected: {}", error.empty() ? "relink Twitch account" : error));
                    if (notify) {
                        notifyOnMain(error.empty() ? "Relink Twitch first" : error, NotificationIcon::Error);
                    }
                    return;
                }

                auto ok = runNativeTwitchBridge(*auth);
                if (!ok && notify) {
                    notifyOnMain(bridgeRuntimeStatusText(), NotificationIcon::Error);
                }
            }).detach();
#else
            s_bridgeAutoStartInProgress = false;
            writeBridgeStatus("Twitch chat is only available on Windows");
            if (notify) {
                Notification::create("Twitch chat is Windows-only", NotificationIcon::Error, 1.8f)->show();
            }
#endif
        }

        void startTwitchLinkForChannel(std::string channelName);

        class TwitchAccountLinkPopup : public Popup {
          protected:
            TextInput* m_accountInput = nullptr;

            bool setup() {
                if (!Popup::init(406.0f, 224.0f)) {
                    return false;
                }

                this->setTitle("Connect Twitch", "goldFont.fnt", 0.70f, 20.0f);

                auto panel = CCLayerColor::create({20, 12, 42, 118}, 330.0f, 90.0f);
                panel->setPosition({38.0f, 78.0f});
                m_mainLayer->addChild(panel);

                auto accent = CCLayerColor::create({150, 88, 255, 190}, 4.0f, 90.0f);
                accent->setPosition({38.0f, 78.0f});
                m_mainLayer->addChild(accent);

                auto body = CCLabelBMFont::create("Paste your Twitch account link or username.\nGD-Live handles the rest.", "bigFont.fnt",
                                                  316.0f, kCCTextAlignmentCenter);
                body->setScale(0.29f);
                body->setColor(ccc3(220, 245, 255));
                body->setPosition({203.0f, 144.0f});
                m_mainLayer->addChild(body);

                m_accountInput = TextInput::create(286.0f, "twitch.tv/yourname");
                m_accountInput->setPosition({203.0f, 108.0f});
                m_accountInput->setMaxCharCount(128);
                m_accountInput->setCommonFilter(CommonFilter::Any);
                m_accountInput->setTextAlign(TextInputAlign::Left);
                if (!s_settings.twitchChannel.empty()) {
                    m_accountInput->setString(fmt::format("twitch.tv/{}", s_settings.twitchChannel));
                }
                m_mainLayer->addChild(m_accountInput);

                auto row = createLayoutMenu(m_mainLayer, "gdlive-twitch-account-actions"_spr, {203.0f, 45.0f}, {344.0f, 34.0f}, 5);
                addButtonToMenu(
                    row, createGDLiveButton("Paste", "GJ_button_02.png", this, menu_selector(TwitchAccountLinkPopup::onPaste), 0.48f));
                addButtonToMenu(row, createGDLiveButton("Continue", "GJ_button_01.png", this,
                                                        menu_selector(TwitchAccountLinkPopup::onContinue), 0.48f));

                return true;
            }

            void onPaste(CCObject*) {
                auto account = trim(utils::clipboard::read());
                if (account.empty()) {
                    Notification::create("Clipboard is empty", NotificationIcon::Info, 1.2f)->show();
                    return;
                }
                m_accountInput->setString(account);
            }

            void onContinue(CCObject*) {
                auto channel = normalizeTwitchChannel(std::string(m_accountInput->getString()));
                if (!validTwitchChannel(channel)) {
                    Notification::create("Enter a Twitch link or username", NotificationIcon::Error, 1.8f)->show();
                    return;
                }

                Mod::get()->setSettingValue<std::string>("twitch-channel", channel);
                s_settings = Settings::load();
                syncTwitchChannelToAuth();
                this->onClose(nullptr);
                geode::queueInMainThread([channel] { startTwitchLinkForChannel(channel); });
            }

          public:
            static TwitchAccountLinkPopup* create() {
                auto ret = new TwitchAccountLinkPopup();
                if (ret && ret->setup()) {
                    ret->autorelease();
                    return ret;
                }
                delete ret;
                return nullptr;
            }
        };

        class TwitchLinkPopup : public Popup {
          protected:
            std::string m_verificationUri;
            std::string m_userCode;

            bool setup(std::string const& userCode, std::string const& verificationUri) {
                if (!Popup::init(382.0f, 208.0f)) {
                    return false;
                }

                m_userCode = userCode;
                m_verificationUri = verificationUri;
                this->setTitle("Link Twitch", "goldFont.fnt", 0.72f, 22.0f);

                auto panel = CCLayerColor::create({24, 12, 46, 112}, 316.0f, 76.0f);
                panel->setPosition({33.0f, 72.0f});
                m_mainLayer->addChild(panel);

                auto accent = CCLayerColor::create({170, 92, 255, 180}, 4.0f, 76.0f);
                accent->setPosition({33.0f, 72.0f});
                m_mainLayer->addChild(accent);

                auto body = CCLabelBMFont::create("Open Twitch and enter this code.\nGD-Live will finish linking automatically.",
                                                  "bigFont.fnt", 310.0f, kCCTextAlignmentCenter);
                body->setScale(0.32f);
                body->setPosition({191.0f, 129.0f});
                m_mainLayer->addChild(body);

                auto code = CCLabelBMFont::create(userCode.c_str(), "goldFont.fnt");
                code->setScale(0.76f);
                code->setPosition({191.0f, 91.0f});
                m_mainLayer->addChild(code);

                auto row = createLayoutMenu(m_mainLayer, "gdlive-twitch-actions"_spr, {191.0f, 42.0f}, {300.0f, 34.0f}, 5);
                addButtonToMenu(
                    row, createGDLiveButton("Open Twitch", "GJ_button_05.png", this, menu_selector(TwitchLinkPopup::onOpenTwitch), 0.54f));
                addButtonToMenu(
                    row, createGDLiveButton("Copy Code", "GJ_button_02.png", this, menu_selector(TwitchLinkPopup::onCopyCode), 0.54f));

                return true;
            }

            void onOpenTwitch(CCObject*) {
                geode::utils::web::openLinkInBrowser(m_verificationUri);
            }

            void onCopyCode(CCObject*) {
                utils::clipboard::write(m_userCode);
                Notification::create("Twitch code copied", NotificationIcon::Success, 1.2f)->show();
            }

          public:
            static TwitchLinkPopup* create(std::string const& userCode, std::string const& verificationUri) {
                auto ret = new TwitchLinkPopup();
                if (ret && ret->setup(userCode, verificationUri)) {
                    ret->autorelease();
                    return ret;
                }
                delete ret;
                return nullptr;
            }
        };

        struct TwitchLinkGuard {
            ~TwitchLinkGuard() {
                s_twitchLinkInProgress = false;
            }
        };

        void startTwitchLinkForChannel(std::string channelName) {
            auto clientId = currentTwitchClientId();
            if (clientId.empty()) {
                Notification::create("This GD-Live build is missing Twitch setup", NotificationIcon::Error, 2.2f)->show();
                return;
            }
            if (s_twitchLinkInProgress.exchange(true)) {
                Notification::create("Twitch link already in progress", NotificationIcon::Info, 1.4f)->show();
                return;
            }

            channelName = normalizeTwitchChannel(std::move(channelName));
            Notification::create("Starting Twitch link...", NotificationIcon::Info, 1.2f)->show();

            std::thread([clientId, channelName] {
                TwitchLinkGuard guard;

                std::string error;
                auto ticket = requestTwitchDeviceCode(clientId, error);
                if (!ticket) {
                    notifyOnMain(error.empty() ? "Could not start Twitch link" : error, NotificationIcon::Error);
                    return;
                }

                geode::queueInMainThread([ticket = *ticket] {
                    if (auto popup = TwitchLinkPopup::create(ticket.userCode, ticket.verificationUri)) {
                        popup->show();
                    }
                });

                auto auth = pollTwitchDeviceToken(clientId, *ticket, channelName, error);
                if (!auth) {
                    notifyOnMain(error.empty() ? "Twitch link failed" : error, NotificationIcon::Error);
                    return;
                }

                std::string refreshCheckError;
                if (auto refreshed = refreshTwitchAccessToken(*auth, refreshCheckError)) {
                    auth = std::move(refreshed);
                } else if (twitchRefreshNeedsPublicClient(refreshCheckError)) {
                    notifyOnMain(twitchPublicClientSetupMessage(), NotificationIcon::Error);
                    return;
                }

                if (!saveTwitchAuth(*auth)) {
                    notifyOnMain("Could not save Twitch link", NotificationIcon::Error);
                    return;
                }

                geode::queueInMainThread([username = auth->username] {
                    pushChatLine(fmt::format("GD-Live: Twitch linked as {}", username));
                    Notification::create(fmt::format("Twitch linked: {}", username).c_str(), NotificationIcon::Success, 2.0f)->show();
                    startTwitchBridgeAsync(true, true);
                });
            }).detach();
        }

        void beginTwitchLink() {
            if (currentTwitchClientId().empty()) {
                Notification::create("This GD-Live build is missing Twitch setup", NotificationIcon::Error, 2.2f)->show();
                return;
            }
            if (auto popup = TwitchAccountLinkPopup::create()) {
                popup->show();
            } else {
                Notification::create("Could not open Twitch link setup", NotificationIcon::Error, 2.0f)->show();
            }
        }

        class TwitchAccountSetting : public SettingV3 {
          public:
            static Result<std::shared_ptr<SettingV3>> parse(std::string key, std::string modID, matjson::Value const& json) {
                auto res = std::make_shared<TwitchAccountSetting>();
                auto root = checkJson(json, "TwitchAccountSetting");
                res->parseBaseProperties(std::move(key), std::move(modID), root);
                return root.ok(std::static_pointer_cast<SettingV3>(res));
            }

            bool load(matjson::Value const&) override {
                return true;
            }

            bool save(matjson::Value&) const override {
                return true;
            }

            bool isDefaultValue() const override {
                return !loadTwitchAuth().has_value();
            }

            void reset() override {
                std::error_code error;
                std::filesystem::remove(twitchAuthPath(), error);
            }

            SettingNodeV3* createNode(float width) override;
        };

        class TwitchAccountSettingNode : public SettingNodeV3 {
          protected:
            bool init(std::shared_ptr<TwitchAccountSetting> setting, float width) {
                if (!SettingNodeV3::init(setting, width)) {
                    return false;
                }

                this->setContentHeight(44.0f);
                this->getButtonMenu()->setContentWidth(std::min(310.0f, width - 130.0f));

                auto linkButton =
                    createGDLiveButton("Link", "GJ_button_01.png", this, menu_selector(TwitchAccountSettingNode::onLink), 0.48f);
                this->getButtonMenu()->addChild(linkButton);

                auto unlinkButton =
                    createGDLiveButton("Unlink", "GJ_button_06.png", this, menu_selector(TwitchAccountSettingNode::onUnlink), 0.48f);
                this->getButtonMenu()->addChild(unlinkButton);

                auto startButton = createGDLiveButton("Start Chat", "GJ_button_02.png", this,
                                                      menu_selector(TwitchAccountSettingNode::onStartBridge), 0.48f);
                this->getButtonMenu()->addChild(startButton);

                this->getButtonMenu()->setLayout(
                    RowLayout::create()->setGap(6.0f)->setAutoScale(true)->setAxisAlignment(AxisAlignment::Center));
                this->getButtonMenu()->updateLayout();
                this->updateState(nullptr);
                return true;
            }

            void onLink(CCObject*) {
                beginTwitchLink();
                this->updateState(nullptr);
            }

            void onUnlink(CCObject*) {
                clearTwitchAuth();
                this->updateState(nullptr);
            }

            void onStartBridge(CCObject*) {
                startTwitchBridgeAsync(true, true);
                this->updateState(nullptr);
            }

            void updateState(CCNode* invoker) override {
                SettingNodeV3::updateState(invoker);
                auto status = twitchLinkStatusText();
                this->getStatusLabel()->setVisible(true);
                this->getStatusLabel()->setString(status.c_str());
                this->getStatusLabel()->setColor(startsWith(status, "Linked:") ? ccc3(83, 255, 112) : ccWHITE);
            }

            void onCommit() override {}

            void onResetToDefault() override {
                this->updateState(nullptr);
            }

          public:
            static TwitchAccountSettingNode* create(std::shared_ptr<TwitchAccountSetting> setting, float width) {
                auto ret = new TwitchAccountSettingNode();
                if (ret && ret->init(setting, width)) {
                    ret->autorelease();
                    return ret;
                }
                delete ret;
                return nullptr;
            }

            bool hasUncommittedChanges() const override {
                return false;
            }

            bool hasNonDefaultValue() const override {
                return loadTwitchAuth().has_value();
            }
        };

        SettingNodeV3* TwitchAccountSetting::createNode(float width) {
            return TwitchAccountSettingNode::create(std::static_pointer_cast<TwitchAccountSetting>(shared_from_this()), width);
        }

        void registerCustomSettings() {
            auto result = Mod::get()->registerCustomSettingType("gdlive-twitch-account", &TwitchAccountSetting::parse);
            if (!result) {
                log::warn("Failed to register Twitch account setting: {}", result.unwrapErr());
            }
        }

        void copyBundledSupportFilesAsync(std::filesystem::path resourcesDir, std::filesystem::path targetDir) {
            std::thread([resourcesDir = std::move(resourcesDir), targetDir = std::move(targetDir)] {
                std::this_thread::sleep_for(std::chrono::seconds(2));

                std::error_code error;
                auto doc = resourcesDir / "twitch.md";
                if (!std::filesystem::exists(doc, error)) {
                    doc = resourcesDir / "docs" / "twitch.md";
                }
                if (std::filesystem::exists(doc, error)) {
                    std::filesystem::copy_file(doc, targetDir / kTwitchSetupDoc, std::filesystem::copy_options::overwrite_existing, error);
                    if (error) {
                        log::warn("Failed to copy Twitch setup doc: {}", error.message());
                    }
                }
            }).detach();
        }

        class GDLiveOverlay : public CCNode {
          protected:
            CCLayerColor* m_progressBg = nullptr;
            CCLayerColor* m_progressFill = nullptr;
            CCLabelBMFont* m_percentLabel = nullptr;
            CCLayerColor* m_clickPanel = nullptr;
            CCLabelBMFont* m_clickLabel = nullptr;
            CCLayerColor* m_sessionPanel = nullptr;
            CCLabelBMFont* m_sessionLabel = nullptr;
            CCLayerColor* m_chatPanel = nullptr;
            CCLayerColor* m_chatHeader = nullptr;
            CCLabelBMFont* m_chatTitle = nullptr;
            std::vector<CCLabelBMFont*> m_chatLabels;

            bool init() override {
                if (!CCNode::init()) {
                    return false;
                }
                this->setID(kOverlayID);
                auto winSize = CCDirector::get()->getWinSize();
                this->setContentSize(winSize);
                this->setAnchorPoint({0.0f, 0.0f});

                m_progressBg = CCLayerColor::create({4, 8, 16, 165}, 270.0f, 16.0f);
                m_progressBg->setAnchorPoint({0.0f, 0.0f});
                this->addChild(m_progressBg);

                m_progressFill = CCLayerColor::create({0, 210, 255, 225}, 0.0f, 10.0f);
                m_progressFill->setAnchorPoint({0.0f, 0.0f});
                m_progressFill->setPosition({3.0f, 3.0f});
                m_progressBg->addChild(m_progressFill);

                m_percentLabel = CCLabelBMFont::create("0.00%", "bigFont.fnt");
                m_percentLabel->setScale(0.31f);
                m_percentLabel->setAnchorPoint({0.5f, 0.5f});
                m_percentLabel->setPosition({135.0f, 8.0f});
                m_progressBg->addChild(m_percentLabel);

                m_clickPanel = CCLayerColor::create({4, 8, 16, 145}, 176.0f, 28.0f);
                m_clickPanel->setAnchorPoint({0.0f, 0.0f});
                this->addChild(m_clickPanel);

                m_clickLabel = CCLabelBMFont::create("Clicks: 0", "goldFont.fnt", 360.0f, kCCTextAlignmentLeft);
                m_clickLabel->setScale(0.30f);
                m_clickLabel->setAnchorPoint({0.0f, 0.5f});
                m_clickLabel->setPosition({10.0f, 14.0f});
                m_clickPanel->addChild(m_clickLabel);

                m_sessionPanel = CCLayerColor::create({4, 8, 16, 135}, 204.0f, 78.0f);
                m_sessionPanel->setAnchorPoint({0.0f, 0.0f});
                this->addChild(m_sessionPanel);

                m_sessionLabel = CCLabelBMFont::create("Session", "bigFont.fnt");
                m_sessionLabel->setScale(0.27f);
                m_sessionLabel->setAnchorPoint({0.0f, 1.0f});
                m_sessionLabel->setPosition({10.0f, 70.0f});
                m_sessionPanel->addChild(m_sessionLabel);

                m_chatPanel = CCLayerColor::create({4, 8, 16, 138}, 188.0f, 116.0f);
                m_chatPanel->setAnchorPoint({0.0f, 0.0f});
                this->addChild(m_chatPanel);

                m_chatHeader = CCLayerColor::create({0, 180, 255, 52}, 188.0f, 20.0f);
                m_chatHeader->setAnchorPoint({0.0f, 0.0f});
                m_chatHeader->setPosition({0.0f, 96.0f});
                m_chatPanel->addChild(m_chatHeader);

                m_chatTitle = CCLabelBMFont::create("GD-Live Chat", "goldFont.fnt");
                m_chatTitle->setScale(0.30f);
                m_chatTitle->setAnchorPoint({0.0f, 0.5f});
                m_chatTitle->setPosition({8.0f, 106.0f});
                m_chatPanel->addChild(m_chatTitle);

                for (int i = 0; i < 6; ++i) {
                    auto label = CCLabelBMFont::create("", "bigFont.fnt", 360.0f, kCCTextAlignmentLeft);
                    label->setScale(0.23f);
                    label->setAnchorPoint({0.0f, 0.5f});
                    label->setPosition({8.0f, 84.0f - i * 13.0f});
                    m_chatPanel->addChild(label);
                    m_chatLabels.push_back(label);
                }

                return true;
            }

          public:
            static GDLiveOverlay* create() {
                auto ret = new GDLiveOverlay();
                if (ret && ret->init()) {
                    ret->autorelease();
                    return ret;
                }
                delete ret;
                return nullptr;
            }

            void addClickPulse() {
                if (!s_settings.enableOverlays || !s_settings.showClickTracker) {
                    return;
                }
                auto pulse = CCLayerColor::create({0, 220, 255, 190}, 12.0f, 12.0f);
                pulse->setAnchorPoint({0.5f, 0.5f});
                pulse->setPosition({16.0f, 14.0f});
                pulse->setScale(0.35f);
                m_clickPanel->addChild(pulse, 20);
                auto sequence = CCSequence::create(CCEaseOut::create(CCScaleTo::create(0.18f, 1.25f), 2.0f), CCFadeOut::create(0.18f),
                                                   CCRemoveSelf::create(), nullptr);
                pulse->runAction(sequence);
            }

            void refresh(PlayLayer* layer) {
                if (!layer) {
                    return;
                }

                auto winSize = CCDirector::get()->getWinSize();
                this->setContentSize(winSize);

                auto percent = std::clamp(layer->getCurrentPercent(), 0.0f, 100.0f);
                s_session.updatePercent(percent);

                auto barWidth = 270.0f;
                auto barHeight = 16.0f;
                auto barX = (winSize.width - barWidth) / 2.0f + static_cast<float>(s_settings.progressX);
                auto barY =
                    (s_settings.progressPosition == "Top" ? winSize.height - 30.0f : 12.0f) + static_cast<float>(s_settings.progressY);
                auto barPos = clampPanelPosition({barX, barY}, {barWidth, barHeight}, winSize, 8.0f);
                m_progressBg->setPosition(barPos);
                m_progressFill->setContentSize({std::max(0.0f, 264.0f * percent / 100.0f), 10.0f});
                m_percentLabel->setString(fmt::format("{:.2f}%", percent).c_str());

                m_progressBg->setVisible(s_settings.enableOverlays && s_settings.showProgressHud);

                auto clickPos = s_settings.progressPosition == "Bottom" ? CCPoint{10.0f, std::min(winSize.height - 36.0f, barPos.y + 28.0f)}
                                                                        : CCPoint{10.0f, 12.0f};
                m_clickPanel->setPosition(clampPanelPosition(clickPos, {176.0f, 28.0f}, winSize, 8.0f));
                m_clickLabel->setString(fmt::format("Clicks: {}  Gap {}ms", s_session.clickCount, s_session.lastIntervalMs).c_str());
                m_clickPanel->setVisible(s_settings.enableOverlays && s_settings.showClickTracker);

                auto attempts = s_session.attemptsForDisplay(layer);
                auto successRate =
                    attempts > 0 ? (static_cast<float>(s_session.completionCount) / static_cast<float>(attempts)) * 100.0f : 0.0f;
                auto sessionPos = clampPanelPosition({10.0f, winSize.height - 88.0f}, {204.0f, 78.0f}, winSize, 8.0f);
                m_sessionPanel->setPosition(sessionPos);
                m_sessionLabel->setString(
                    fmt::format("GD-Live  //  Live Stats\n{}\nAttempt {}  |  Best {}%\nTime {:.0f}s  |  Clear {:.0f}%", s_session.levelName,
                                attempts, s_session.bestPercent, s_session.elapsedSeconds, successRate)
                        .c_str());
                m_sessionPanel->setVisible(s_settings.enableOverlays && s_settings.showSessionAnalytics);

                auto panelX = winSize.width - 198.0f + static_cast<float>(s_settings.sidebarX);
                auto panelY = winSize.height - 126.0f + static_cast<float>(s_settings.sidebarY);
                m_chatPanel->setPosition(clampPanelPosition({panelX, panelY}, {188.0f, 116.0f}, winSize, 8.0f));
                m_chatPanel->setVisible(s_settings.enableOverlays && s_settings.enableChat);
                m_chatTitle->setString(s_levelRequests.empty() ? "GD-Live Chat"
                                                               : fmt::format("GD-Live Chat | Queue {}", s_levelRequests.size()).c_str());

                for (size_t i = 0; i < m_chatLabels.size(); ++i) {
                    auto text = i < s_chatLines.size() ? s_chatLines[i] : std::string();
                    if (text.size() > 62) {
                        text = text.substr(0, 59) + "...";
                    }
                    m_chatLabels[i]->setString(text.c_str());
                }
            }
        };

        class LevelRequestQueuePopup : public Popup {
          protected:
            ScrollLayer* m_list = nullptr;
            CCLabelBMFont* m_countLabel = nullptr;

            bool setup() {
                if (!Popup::init(430.0f, 282.0f)) {
                    return false;
                }

                this->setTitle("Level Requests", "goldFont.fnt", 0.68f, 18.0f);

                addHeaderPanel();

                auto listBackground = CCLayerColor::create({4, 8, 18, 120}, 358.0f, 154.0f);
                listBackground->setPosition({28.0f, 70.0f});
                m_mainLayer->addChild(listBackground, -1);
                m_mainLayer->addChild(colorBlock(rgba(92, 174, 255, 150), {4.0f, 154.0f}, {28.0f, 70.0f}));

                m_list = ScrollLayer::create({354.0f, 150.0f});
                m_list->setPosition({32.0f, 72.0f});
                m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(4.0f));
                m_mainLayer->addChild(m_list, 5);

                auto scrollbar = Scrollbar::create(m_list);
                scrollbar->setPosition({392.0f, 72.0f});
                m_mainLayer->addChild(scrollbar, 6);

                auto bottomRow = createLayoutMenu(m_mainLayer, "gdlive-request-queue-bottom"_spr, {215.0f, 38.0f}, {348.0f, 30.0f}, 10);
                addButtonToMenu(bottomRow, createGDLiveButton("Play Next", "GJ_button_02.png", this,
                                                              menu_selector(LevelRequestQueuePopup::onPlayNext), 0.42f));
                addButtonToMenu(bottomRow, createGDLiveButton("Clear All", "GJ_button_06.png", this,
                                                              menu_selector(LevelRequestQueuePopup::onClear), 0.42f));
                addButtonToMenu(bottomRow, createGDLiveButton("Refresh", "GJ_button_05.png", this,
                                                              menu_selector(LevelRequestQueuePopup::onRefresh), 0.42f));

                refreshList();
                return true;
            }

            void addHeaderPanel() {
                auto panel = CCLayerColor::create({14, 20, 44, 120}, 358.0f, 42.0f);
                panel->setPosition({28.0f, 222.0f});
                m_mainLayer->addChild(panel, -1);
                m_mainLayer->addChild(colorBlock(rgba(0, 232, 196, 165), {4.0f, 42.0f}, {28.0f, 222.0f}));

                m_countLabel = CCLabelBMFont::create("", "bigFont.fnt", 312.0f, kCCTextAlignmentCenter);
                m_countLabel->setScale(0.27f);
                m_countLabel->setColor(ccc3(220, 250, 255));
                m_countLabel->setPosition({215.0f, 245.0f});
                m_mainLayer->addChild(m_countLabel);

                auto hint = CCLabelBMFont::create("Play any requested level, or remove bad requests.", "bigFont.fnt", 330.0f,
                                                  kCCTextAlignmentCenter);
                hint->setScale(0.20f);
                hint->setColor(ccc3(165, 220, 255));
                hint->setPosition({215.0f, 229.0f});
                m_mainLayer->addChild(hint);
            }

            CCNode* createRequestRow(LevelRequest const& request, size_t index) {
                auto row = CCNode::create();
                row->setContentSize({354.0f, 40.0f});

                row->addChild(colorBlock(rgba(index % 2 == 0 ? 12 : 18, 18, 36, 118), {346.0f, 38.0f}, {4.0f, 1.0f}), -1);
                row->addChild(colorBlock(rgba(92, 174, 255, 110), {3.0f, 38.0f}, {4.0f, 1.0f}));

                auto title = CCLabelBMFont::create(
                    fmt::format("#{}  {}  by {}", index + 1, request.levelID, request.requester.empty() ? "chat" : request.requester)
                        .c_str(),
                    "goldFont.fnt", 218.0f, kCCTextAlignmentLeft);
                title->setAnchorPoint({0.0f, 0.5f});
                title->setScale(0.26f);
                title->setPosition({16.0f, 27.0f});
                row->addChild(title);

                auto messageText = request.message.empty() ? std::string("No message") : shortenStatus(request.message, 48);
                auto message = CCLabelBMFont::create(messageText.c_str(), "bigFont.fnt", 218.0f, kCCTextAlignmentLeft);
                message->setAnchorPoint({0.0f, 0.5f});
                message->setScale(0.18f);
                message->setColor(ccc3(190, 230, 255));
                message->setPosition({16.0f, 12.0f});
                row->addChild(message);

                auto actionMenu = createLayoutMenu(row, "gdlive-request-row-actions"_spr, {294.0f, 20.0f}, {102.0f, 26.0f}, 5);
                auto playButton =
                    createGDLiveButton("Play", "GJ_button_01.png", this, menu_selector(LevelRequestQueuePopup::onPlayRequest), 0.34f);
                playButton->setTag(static_cast<int>(index));
                addButtonToMenu(actionMenu, playButton);

                auto removeButton =
                    createGDLiveButton("X", "GJ_button_06.png", this, menu_selector(LevelRequestQueuePopup::onRemoveRequest), 0.34f);
                removeButton->setTag(static_cast<int>(index));
                addButtonToMenu(actionMenu, removeButton);

                return row;
            }

            void refreshList() {
                if (!m_list) {
                    return;
                }

                auto queueSuffix = s_levelRequests.empty() ? std::string() : fmt::format(" - {}", nextRequestLabel());
                m_countLabel->setString(fmt::format("{} queued{}", s_levelRequests.size(), queueSuffix).c_str());

                m_list->m_contentLayer->removeAllChildren();
                if (s_levelRequests.empty()) {
                    auto empty = CCLabelBMFont::create("No level requests yet.\nChat can use !request <level id>.", "bigFont.fnt", 330.0f,
                                                       kCCTextAlignmentCenter);
                    empty->setScale(0.28f);
                    empty->setColor(ccc3(210, 235, 255));
                    empty->setPosition({177.0f, 75.0f});
                    m_list->m_contentLayer->addChild(empty);
                } else {
                    for (size_t index = 0; index < s_levelRequests.size(); ++index) {
                        m_list->m_contentLayer->addChild(createRequestRow(s_levelRequests[index], index));
                    }
                }
                m_list->m_contentLayer->updateLayout();
                m_list->scrollToTop();
            }

            void onPlayRequest(CCObject* sender) {
                auto index = static_cast<size_t>(std::max(0, sender->getTag()));
                this->onClose(nullptr);
                openLevelRequestAt(index);
            }

            void onRemoveRequest(CCObject* sender) {
                auto index = static_cast<size_t>(std::max(0, sender->getTag()));
                removeLevelRequestAt(index);
                refreshList();
            }

            void onPlayNext(CCObject*) {
                this->onClose(nullptr);
                openNextLevelRequest();
            }

            void onClear(CCObject*) {
                clearLevelRequests();
                refreshList();
            }

            void onRefresh(CCObject*) {
                loadLevelRequests();
                refreshList();
            }

          public:
            static LevelRequestQueuePopup* create() {
                auto ret = new LevelRequestQueuePopup();
                if (ret && ret->setup()) {
                    ret->autorelease();
                    return ret;
                }
                delete ret;
                return nullptr;
            }
        };

        class GDLiveControlPopup : public Popup {
          protected:
            CCLabelBMFont* m_statusLabel = nullptr;
            CCLabelBMFont* m_hintLabel = nullptr;
            CCLabelBMFont* m_queueLabel = nullptr;
            CCLabelBMFont* m_controlsLabel = nullptr;

            bool setup() {
                if (!Popup::init(402.0f, 258.0f)) {
                    return false;
                }

                this->setTitle("GD-Live Hub", "goldFont.fnt", 0.62f, 16.0f);

                addDashboardPanel({24.0f, 164.0f}, {354.0f, 50.0f}, rgba(8, 16, 30, 118), rgba(0, 232, 196, 165));
                addSectionLabel("STREAM", {36.0f, 202.0f}, ccc3(90, 255, 222));

                m_statusLabel = CCLabelBMFont::create("", "goldFont.fnt", 318.0f, kCCTextAlignmentCenter);
                m_statusLabel->setScale(0.32f);
                m_statusLabel->setPosition({201.0f, 190.0f});
                m_mainLayer->addChild(m_statusLabel);

                m_hintLabel = CCLabelBMFont::create("", "bigFont.fnt", 318.0f, kCCTextAlignmentCenter);
                m_hintLabel->setScale(0.22f);
                m_hintLabel->setPosition({201.0f, 174.0f});
                m_hintLabel->setColor(ccc3(190, 245, 255));
                m_mainLayer->addChild(m_hintLabel);

                auto mainRow = createPopupRow("gdlive-primary-actions"_spr, {201.0f, 146.0f}, {342.0f, 30.0f});
                addButton(mainRow, "Connect", "GJ_button_01.png", menu_selector(GDLiveControlPopup::onLinkTwitch), 0.44f);
                addButton(mainRow, "Start Chat", "GJ_button_05.png", menu_selector(GDLiveControlPopup::onStartBridge), 0.44f);
                addButton(mainRow, "Files", "GJ_button_02.png", menu_selector(GDLiveControlPopup::onOpenFiles), 0.44f);

                addDashboardPanel({24.0f, 92.0f}, {354.0f, 38.0f}, rgba(7, 12, 26, 96), rgba(92, 174, 255, 135));
                addSectionLabel("REQUEST QUEUE", {36.0f, 120.0f}, ccc3(160, 214, 255));

                m_queueLabel = CCLabelBMFont::create("", "bigFont.fnt", 312.0f, kCCTextAlignmentCenter);
                m_queueLabel->setScale(0.25f);
                m_queueLabel->setPosition({201.0f, 105.0f});
                m_queueLabel->setColor(ccc3(245, 245, 245));
                m_mainLayer->addChild(m_queueLabel);

                auto queueRow = createPopupRow("gdlive-queue-actions"_spr, {201.0f, 74.0f}, {342.0f, 28.0f});
                addButton(queueRow, "Play Next", "GJ_button_02.png", menu_selector(GDLiveControlPopup::onPlayNext), 0.40f);
                addButton(queueRow, "Queue", "GJ_button_05.png", menu_selector(GDLiveControlPopup::onOpenQueue), 0.40f);
                addButton(queueRow, "Clear", "GJ_button_06.png", menu_selector(GDLiveControlPopup::onClearQueue), 0.40f);

                addDashboardPanel({24.0f, 34.0f}, {354.0f, 28.0f}, rgba(8, 10, 24, 82), rgba(177, 117, 255, 125));
                addSectionLabel("MODES", {36.0f, 54.0f}, ccc3(220, 190, 255));
                m_controlsLabel = CCLabelBMFont::create("", "bigFont.fnt", 300.0f, kCCTextAlignmentCenter);
                m_controlsLabel->setScale(0.20f);
                m_controlsLabel->setPosition({216.0f, 48.0f});
                m_controlsLabel->setColor(ccc3(220, 245, 255));
                m_mainLayer->addChild(m_controlsLabel);

                auto toggleRow = createPopupRow("gdlive-mode-actions"_spr, {201.0f, 20.0f}, {342.0f, 28.0f});
                addButton(toggleRow, "Overlay", "GJ_button_05.png", menu_selector(GDLiveControlPopup::onToggleOverlays), 0.38f);
                addButton(toggleRow, "Chat", "GJ_button_05.png", menu_selector(GDLiveControlPopup::onToggleChat), 0.38f);
                addButton(toggleRow, "Requests", "GJ_button_05.png", menu_selector(GDLiveControlPopup::onToggleRequests), 0.38f);

                refresh();
                return true;
            }

            void addDashboardPanel(CCPoint position, CCSize size, ccColor4B fill, ccColor4B accentColor) {
                m_mainLayer->addChild(colorBlock(rgba(0, 0, 0, 52), size, {position.x + 2.0f, position.y - 2.0f}), -2);
                m_mainLayer->addChild(colorBlock(fill, size, position), -1);
                m_mainLayer->addChild(colorBlock(accentColor, {4.0f, size.height}, position));
                m_mainLayer->addChild(
                    colorBlock(rgba(255, 255, 255, 18), {size.width - 8.0f, 1.0f}, {position.x + 6.0f, position.y + size.height - 7.0f}));
            }

            CCMenu* createPopupRow(char const* id, CCPoint position, CCSize size) {
                return createLayoutMenu(m_mainLayer, id, position, size, 5);
            }

            void addSectionLabel(char const* text, CCPoint position, ccColor3B color) {
                auto label = CCLabelBMFont::create(text, "bigFont.fnt");
                label->setAnchorPoint({0.0f, 0.5f});
                label->setScale(0.20f);
                label->setColor(color);
                label->setPosition(position);
                m_mainLayer->addChild(label, 2);
            }

            void addButton(CCMenu* row, char const* label, char const* texture, SEL_MenuHandler selector, float scale = 0.54f) {
                addButtonToMenu(row, createGDLiveButton(label, texture, this, selector, scale));
            }

            void refresh() {
                auto linked = loadTwitchAuth().has_value();
                auto connected = bridgeLooksRunning();
                auto needsSetup = !linked && currentTwitchClientId().empty();
                auto statusTitle = needsSetup  ? std::string("Twitch update needed")
                                   : !linked   ? std::string("Connect Twitch to start")
                                   : connected ? std::string("Ready for stream")
                                               : std::string("Saved Twitch - auto-starting");
                m_statusLabel->setString(statusTitle.c_str());
                m_statusLabel->setColor(connected ? ccc3(105, 255, 155) : linked ? ccc3(255, 225, 104) : ccc3(255, 255, 255));

                m_hintLabel->setString(linked       ? bridgeRuntimeStatusText().c_str()
                                       : needsSetup ? "This build needs the official Twitch app setup from the developer."
                                                    : "Paste your Twitch link once. Then chat + requests auto-connect.");

                m_queueLabel->setString(fmt::format("{} queued - {}", s_levelRequests.size(), nextRequestLabel()).c_str());

                m_controlsLabel->setString(fmt::format("Overlay {}   Chat {}   Requests {}", s_settings.enableOverlays ? "ON" : "OFF",
                                                       s_settings.enableChat ? "ON" : "OFF", s_settings.enableLevelRequests ? "ON" : "OFF")
                                               .c_str());
            }

            void toggleSetting(char const* key, char const* label) {
                auto value = !Mod::get()->getSettingValue<bool>(key);
                Mod::get()->setSettingValue<bool>(key, value);
                s_settings = Settings::load();
                Notification::create(fmt::format("{} {}", label, value ? "enabled" : "disabled").c_str(),
                                     value ? NotificationIcon::Success : NotificationIcon::Info, 1.2f)
                    ->show();
                refresh();
            }

            void onLinkTwitch(CCObject*) {
                beginTwitchLink();
                refresh();
            }

            void onStartBridge(CCObject*) {
                startTwitchBridgeAsync(true, true);
                refresh();
            }

            void onOpenFiles(CCObject*) {
                utils::file::openFolder(saveDir());
                Notification::create("GD-Live data folder opened", NotificationIcon::Info, 1.5f)->show();
            }

            void onPlayNext(CCObject*) {
                this->onClose(nullptr);
                openNextLevelRequest();
            }

            void onSkip(CCObject*) {
                skipNextLevelRequest();
                refresh();
            }

            void onOpenQueue(CCObject*) {
                if (auto popup = LevelRequestQueuePopup::create()) {
                    popup->show();
                }
            }

            void onClearQueue(CCObject*) {
                clearLevelRequests();
                refresh();
            }

            void onToggleOverlays(CCObject*) {
                toggleSetting("enable-overlays", "Overlays");
            }

            void onToggleChat(CCObject*) {
                toggleSetting("enable-chat", "Chat");
            }

            void onToggleRequests(CCObject*) {
                toggleSetting("enable-level-requests", "Level requests");
            }

          public:
            static GDLiveControlPopup* create() {
                auto ret = new GDLiveControlPopup();
                if (ret && ret->setup()) {
                    ret->autorelease();
                    return ret;
                }
                delete ret;
                return nullptr;
            }
        };

        GDLiveOverlay* findOverlay(PlayLayer* layer) {
            if (!layer || !layer->m_uiLayer) {
                return nullptr;
            }
            return typeinfo_cast<GDLiveOverlay*>(layer->m_uiLayer->getChildByID(kOverlayID));
        }

        GDLiveOverlay* ensureOverlay(PlayLayer* layer) {
            if (!layer || !layer->m_uiLayer) {
                return nullptr;
            }
            if (auto overlay = findOverlay(layer)) {
                return overlay;
            }
            auto overlay = GDLiveOverlay::create();
            layer->m_uiLayer->addChild(overlay, 1000);
            return overlay;
        }

        void bootstrapBridgeFiles() {
            auto dir = saveDir();
            (void)utils::file::createDirectoryAll(dir);

            auto chatPath = dir / kChatBridgeFile;
            if (!std::filesystem::exists(chatPath)) {
                (void)utils::file::writeString(chatPath, "");
            }

            auto statusPath = bridgeStatusPath();
            if (!std::filesystem::exists(statusPath)) {
                (void)utils::file::writeString(statusPath, "not started\n");
            }

            auto requestsPath = levelRequestsPath();
            if (!std::filesystem::exists(requestsPath)) {
                (void)utils::file::writeString(requestsPath, "");
            }

            auto readmePath = dir / kBridgeReadmeFile;
            (void)utils::file::writeString(
                readmePath,
                "GD-Live local chat bridge\n"
                "Link Twitch in GD-Live once. GD-Live saves the login, refreshes it when possible, and auto-starts chat and level requests "
                "after that.\n"
                "If refresh says the Twitch app must be public, the developer must rebuild GD-Live with a public device-code Client ID.\n"
                "The native Windows bridge appends chat messages to chat-bridge.txt as plain text, one message per line.\n"
                "Example: viewer42: !randomshake\n"
                "Supported commands: !randomshake, !colorshift\n"
                "Level requests: !request <level id>, !level <level id>, or !lr <level id>\n"
                "Queued requests are stored in level-requests.tsv and can be opened from the Queue button in the GD-Live hub or pause "
                "menu.\n"
                "Commands are disabled by default and blocked on Demon / 10-star levels.\n"
                "Twitch tokens are stored locally in twitch-auth.json. Do not share this file.\n");
            copyBundledSupportFilesAsync(Mod::get()->getResourcesDir(), dir);
        }
    } // namespace

    void showControlPopup() {
        pollChatBridge(nullptr, 1.0f);
        flushCommandMessagesToChat();
        if (auto popup = GDLiveControlPopup::create()) {
            popup->show();
        }
    }

    void playNextRequestedLevel() {
        openNextLevelRequest();
    }

    void showRequestQueuePopup() {
        pollChatBridge(nullptr, 1.0f);
        flushCommandMessagesToChat();
        if (auto popup = LevelRequestQueuePopup::create()) {
            popup->show();
        }
    }

    void attachMainMenuButton(CCLayer* layer, CCObject* target, SEL_MenuHandler selector) {
        if (!layer || findNodeByIDRecursive(layer, "gdlive-open"_spr)) {
            return;
        }

        auto button = createGDLiveButton("Live", "GJ_button_01.png", target, selector, 0.42f);
        button->setID("gdlive-open"_spr);

        if (auto menu = findMenuByID(layer, {"right-side-menu", "more-games-menu", "bottom-menu"})) {
            addButtonToMenu(menu, button);
            return;
        }

        auto winSize = CCDirector::get()->getWinSize();
        auto menu = createLayoutMenu(layer, "gdlive-main-menu"_spr, {winSize.width - 54.0f, 44.0f}, {108.0f, 32.0f});
        addButtonToMenu(menu, button);
    }

    void attachPauseMenuButtons(CCLayer* layer, CCObject* target, SEL_MenuHandler settingsSelector, SEL_MenuHandler queueSelector,
                                SEL_MenuHandler nextSelector) {
        if (!layer || findNodeByIDRecursive(layer, "gdlive-open"_spr)) {
            return;
        }

        auto winSize = CCDirector::get()->getWinSize();
        auto panelPosition = CCPoint{winSize.width - 196.0f, winSize.height - 48.0f};
        auto panel = colorBlock(rgba(5, 12, 24, 92), {180.0f, 38.0f}, panelPosition);
        panel->setID("gdlive-pause-panel"_spr);
        layer->addChild(panel, 98);

        auto accent = colorBlock(rgba(0, 225, 195, 150), {4.0f, 38.0f}, panelPosition);
        layer->addChild(accent, 99);

        auto title = CCLabelBMFont::create("GD-LIVE", "bigFont.fnt");
        title->setAnchorPoint({0.0f, 0.5f});
        title->setScale(0.18f);
        title->setColor(ccc3(135, 255, 230));
        title->setPosition({panelPosition.x + 12.0f, panelPosition.y + 28.0f});
        layer->addChild(title, 100);

        auto settingsButton = createGDLiveButton("Hub", "GJ_button_01.png", target, settingsSelector, 0.34f);
        settingsButton->setID("gdlive-open"_spr);

        auto queueButton = createGDLiveButton("Queue", "GJ_button_05.png", target, queueSelector, 0.34f);
        queueButton->setID("gdlive-request-queue"_spr);

        auto nextButton = createGDLiveButton("Next", "GJ_button_02.png", target, nextSelector, 0.34f);
        nextButton->setID("gdlive-play-next"_spr);

        auto menu =
            createLayoutMenu(layer, "gdlive-pause-menu"_spr, {panelPosition.x + 110.0f, panelPosition.y + 13.0f}, {148.0f, 24.0f}, 100);
        addButtonToMenu(menu, settingsButton);
        addButtonToMenu(menu, queueButton);
        addButtonToMenu(menu, nextButton);
    }

    void reloadSettings() {
        s_settings = Settings::load();
    }

    void bootstrap() {
        registerCustomSettings();
        reloadSettings();
        bootstrapBridgeFiles();
        loadLevelRequests();
        syncTwitchChannelToAuth();
        installBridgePoller();
        startTwitchBridgeAsync(false, false);

        listenForSettingChanges<bool>("enable-overlays", [](bool) { reloadSettings(); });
        listenForSettingChanges<bool>("show-progress-hud", [](bool) { reloadSettings(); });
        listenForSettingChanges<bool>("show-click-tracker", [](bool) { reloadSettings(); });
        listenForSettingChanges<bool>("show-session-analytics", [](bool) { reloadSettings(); });
        listenForSettingChanges<bool>("enable-chat", [](bool) { reloadSettings(); });
        listenForSettingChanges<bool>("enable-chat-commands", [](bool) { reloadSettings(); });
        listenForSettingChanges<bool>("enable-level-requests", [](bool) { reloadSettings(); });
        listenForSettingChanges<std::string>("progress-position", [](std::string) { reloadSettings(); });
        listenForSettingChanges<int64_t>("progress-x", [](int64_t) { reloadSettings(); });
        listenForSettingChanges<int64_t>("progress-y", [](int64_t) { reloadSettings(); });
        listenForSettingChanges<int64_t>("sidebar-x", [](int64_t) { reloadSettings(); });
        listenForSettingChanges<int64_t>("sidebar-y", [](int64_t) { reloadSettings(); });
        listenForSettingChanges<double>("command-cooldown", [](double) { reloadSettings(); });
        listenForSettingChanges<int64_t>("max-request-queue", [](int64_t) {
            reloadSettings();
            while (static_cast<int>(s_levelRequests.size()) > s_settings.maxRequestQueue) {
                s_levelRequests.pop_back();
            }
            saveLevelRequests();
        });
        listenForSettingChanges<std::string>("twitch-channel", [](std::string) {
            reloadSettings();
            syncTwitchChannelToAuth();
            startTwitchBridgeAsync(true, false);
        });

        log::info("GD-Live runtime initialized");
    }

    void onPlayLayerInit(PlayLayer* layer, GJGameLevel* level) {
        reloadSettings();
        s_session.resetFor(layer, level);
        s_chatLines.clear();
        s_pendingCommandMessages.clear();
        s_chatPollTimer = 0.0f;
        (void)ensureOverlay(layer);
        pushChatLine("GD-Live: session started");
    }

    void onPostUpdate(PlayLayer* layer, float dt) {
        if (!layer) {
            return;
        }

        pollChatBridge(layer, dt);
        flushCommandMessagesToChat();
        if (auto overlay = ensureOverlay(layer)) {
            overlay->refresh(layer);
        }
    }

    void onIdleUpdate(float dt) {
        pollChatBridge(nullptr, dt);
        flushCommandMessagesToChat();
    }

    void onButton(PlayLayer* layer, int, bool) {
        if (!layer || layer != s_session.layer) {
            return;
        }
        s_session.recordClick();
        if (auto overlay = findOverlay(layer)) {
            overlay->addClickPulse();
        }
    }

    void onBeforeReset(PlayLayer* layer) {
        if (layer) {
            auto percent = std::clamp(layer->getCurrentPercent(), 0.0f, 100.0f);
            s_session.bestPercent = std::max(s_session.bestPercent, static_cast<int>(std::floor(percent + 0.5f)));
        }
        s_session.runCount += 1;
    }

    void onAfterReset(PlayLayer* layer) {
        if (!layer) {
            return;
        }
        s_session.currentAttempt = s_session.attemptsForDisplay(layer);
    }

    void onBeforeComplete(PlayLayer* layer) {
        s_session.completionCount += 1;
        s_session.bestPercent = 100;
    }

    void onQuit(PlayLayer* layer) {
        if (layer == s_session.layer) {
            s_session.layer = nullptr;
        }
    }
} // namespace gdlive

$on_mod(Loaded) {
    gdlive::bootstrap();
}

struct $modify(GDLivePlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        gdlive::onPlayLayerInit(this, level);
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        gdlive::onPostUpdate(this, dt);
    }

    void resetLevel() {
        gdlive::onBeforeReset(this);
        PlayLayer::resetLevel();
        gdlive::onAfterReset(this);
    }

    void levelComplete() {
        gdlive::onBeforeComplete(this);
        PlayLayer::levelComplete();
    }

    void onQuit() {
        gdlive::onQuit(this);
        PlayLayer::onQuit();
    }
};

struct $modify(GDLiveBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (!down) {
            return;
        }
        if (auto playLayer = typeinfo_cast<PlayLayer*>(this)) {
            gdlive::onButton(playLayer, button, isPlayer1);
        }
    }
};

struct $modify(GDLiveMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

        gdlive::attachMainMenuButton(this, this, menu_selector(GDLiveMenuLayer::onGDLive));
        return true;
    }

    void onGDLive(CCObject*) {
        gdlive::showControlPopup();
    }
};

struct $modify(GDLivePauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        gdlive::attachPauseMenuButtons(this, this, menu_selector(GDLivePauseLayer::onGDLive),
                                       menu_selector(GDLivePauseLayer::onGDLiveQueue), menu_selector(GDLivePauseLayer::onGDLiveNext));
    }

    void onGDLive(CCObject*) {
        gdlive::showControlPopup();
    }

    void onGDLiveQueue(CCObject*) {
        gdlive::showRequestQueuePopup();
    }

    void onGDLiveNext(CCObject*) {
        gdlive::playNextRequestedLevel();
    }
};
