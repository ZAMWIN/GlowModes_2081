// Per-Gamemode Glow
//
// Geometry Dash applies one global glow colour + one global "glow enabled" flag to every
// player gamemode. This mod keeps the game's glow sprites exactly as they are and only
// overrides their per-sprite parameters (tint, opacity, blend function) while a given
// gamemode is being drawn, using values from a JSON config.
//
// Injection points (verified against the GD 2.2091 bindings, and confirmed live in the
// log: "Enabled PlayerObject::update hook at <base>+0x4e6fc0"):
//   - PlayerObject::update(float)      win 0x4e6fc0   -> applied every frame (self healing)
//   - PlayerObject::updatePlayerArt()  win 0x4f6bb0   -> applied immediately on art/mode change
//   - CCKeyboardDispatcher::dispatchKeyboardMSG        -> config hotkeys (read-only, always
//                                                        forwards to the original)
//
// PlayerObject::updateGlowColor() and PlayerObject::updatePlayerGlow() are NOT hookable on
// Windows: the bindings expose no win address for either of them, so we deliberately do not
// touch them. For the same reason the mod does not call enableCustomGlowColor() /
// disableCustomGlowColor() either: both are inlined on Windows, but they are plain writes to
// PlayerObject::m_hasCustomGlowColor and PlayerObject::m_glowColor, the two fields the game's
// own glow update reads -- so the mod writes those fields directly.
//
// <Geode/Geode.hpp> is deliberately not included: it also pulls in Geode's UI headers, and
// Geode 5.11.2's ScrollLayer/Scrollbar override `scrollWheel` with 2.2081's `void` return
// type, while the 2.2091 cocos2d declarations in sdk-compat have `bool` (the game changed
// it). Those are the loader's UI classes, which this mod never touches, so include the same
// umbrellas Geode.hpp includes minus UI.hpp.
#include <Geode/Bindings.hpp>
#include <Geode/Loader.hpp>
#include <Geode/Utils.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/modify/Modify.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ColorPickPopup.hpp>
#include <Geode/ui/SliderNode.hpp>
#include <Geode/ui/TextInput.hpp>

#include <matjson.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;
namespace fs = std::filesystem;

namespace pgg {
void openGui();
}

namespace pgg {

// ---------------------------------------------------------------------------------------
// Mode table
// ---------------------------------------------------------------------------------------

enum class Mode : std::size_t {
    Cube = 0,
    Ship,
    Ball,
    Ufo,
    Wave,
    Robot,
    Spider,
    Swing,
    Count,
};

constexpr std::size_t kModeCount = static_cast<std::size_t>(Mode::Count);

constexpr std::string_view kModeKeys[kModeCount] = {
    "cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing",
};

// ---------------------------------------------------------------------------------------
// Hotkeys
// ---------------------------------------------------------------------------------------

enum class Hotkey : std::size_t {
    Reload = 0,
    PresetNext,
    PresetPrev,
    HueDown,
    HueUp,
    BrightnessDown,
    BrightnessUp,
    RgbToggle,
    RgbSlower,
    RgbFaster,
    OpenGui,
    Count,
};

constexpr std::size_t kHotkeyCount = static_cast<std::size_t>(Hotkey::Count);

constexpr std::string_view kHotkeyKeys[kHotkeyCount] = {
    "reload", "preset-next", "preset-prev",
    "hue-down", "hue-up", "brightness-down", "brightness-up", "rgb-toggle",
    "rgb-slower", "rgb-faster", "open-menu",
};

// Defaults avoid everything GD itself binds (Esc, Space, arrows, Ctrl/Shift combos, the
// F1/F2/F3/F4 debug & F11 fullscreen keys); a profile can override every one of them.
// F12 can double as Steam's screenshot key: the key still reaches the game, so the toggle
// works, but anyone who does not want the screenshots can move it in the config.
constexpr cocos2d::enumKeyCodes kDefaultHotkeys[kHotkeyCount] = {
    KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_PageUp, KEY_PageDown, KEY_F4,
};

constexpr float kHueStep         = 15.0f;  // degrees per hue key press
constexpr float kBrightnessStep  = 0.1f;   // opacity multiplier per brightness key press

/// Wheel speeds the rgb-slower / rgb-faster hotkeys walk through, in turns per second. Round
/// values on purpose: whatever the log prints can be pasted straight into the config.
constexpr float kRgbSpeedLadder[] = {
    0.005f, 0.0075f, 0.01f, 0.015f, 0.02f, 0.025f, 0.03f, 0.04f, 0.05f, 0.06f,
    0.08f, 0.1f, 0.13f, 0.16f, 0.2f, 0.25f, 0.3f, 0.4f, 0.5f,
};
constexpr std::size_t kRgbSpeedLadderSize =
    sizeof(kRgbSpeedLadder) / sizeof(kRgbSpeedLadder[0]);
constexpr float kRgbMaxSpeed = 5.0f;   // full turns around the colour wheel per second
/// Seconds a colour change is eased over (see Manager::fadeColor).
constexpr float kDefaultFadeSeconds = 0.4f;
constexpr auto  kAutoReloadDelay = std::chrono::milliseconds(500);
constexpr auto  kDebugLogDelay   = std::chrono::milliseconds(1000);
/// A second key-down for the same physical press is ignored within this window (see
/// Manager::onKeyEvent); a longer gap is treated as a fresh press, so a key-up that never
/// arrived cannot disable a hotkey for the rest of the session.
constexpr auto  kHotkeyHeldTimeout = std::chrono::milliseconds(600);

// ---------------------------------------------------------------------------------------
// Profile / preset
// ---------------------------------------------------------------------------------------

struct Profile {
    bool          enabled    = false;
    ccColor3B     color      = { 255, 255, 255 };
    float         brightness = 1.0f;   // multiplies the glow sprite's opacity (0..2)
    bool          additive   = false;  // false = normal alpha, true = additive blend
    float         pulse      = 0.0f;   // seconds per pulse cycle; <= 0 disables pulsing
};

/// The "rgb" block: a rainbow the mod produces itself, overriding every mode while it is on.
/// It lives outside the per-mode profiles on purpose -- it has to work on a fresh config,
/// where every mode is still "enabled": false, and the toggle hotkey has to do something.
struct RgbSettings {
    bool  enabled    = false;
    /// Full turns around the colour wheel per second. 0.02 is one slow, even rainbow in 50 s
    /// (faster reads as flicker); 0 keeps the colour still.
    float speed      = 0.02f;
    float saturation = 1.0f;   // 0 = white, 1 = fully saturated rainbow
    float brightness = 1.0f;   // multiplies the glow sprite's opacity (0..2)
    /// Normal blending by default, not additive: additive *adds* the colour to whatever is
    /// behind it, so over a saturated level background the wheel collapses into two hues
    /// (red over a blue background reads violet, green reads cyan -- "turquoise or purple").
    /// Normal blending puts the colour on top instead, so the whole wheel is visible; anyone
    /// who prefers the additive look can set "blending": "additive" in the rgb block.
    bool  additive   = false;
};

/// One mode's smoothing state: the colour currently on the sprites and the time it was last
/// eased, so a target colour is approached over "fade" seconds instead of jumping.
struct FadeState {
    ccColor3B color { 255, 255, 255 };
    float     time = 0.0f;
    bool      valid = false;
};

/// Tracks the game's own scale for a glow sprite so the brightness overdrive can multiply
/// it without compounding frame after frame (apply() runs twice per frame).
struct GlowScaleState {
    float base    = 1.0f;
    float applied = 0.0f;
};

/// A named set of per-mode colours that can be switched at runtime with a hotkey. Only
/// colours are overridden: brightness/blending/pulse always come from the mode profile.
struct Preset {
    std::string name;
    std::array<std::optional<ccColor3B>, kModeCount> colors {};
};

// ---------------------------------------------------------------------------------------
// Small JSON helpers (matjson returns Result, so everything is optional-tolerant)
// ---------------------------------------------------------------------------------------

std::optional<std::string> jsonString(matjson::Value const& obj, std::string_view key) {
    if (!obj.isObject() || !obj.contains(key)) return std::nullopt;
    if (auto res = obj[key].asString()) return res.unwrap();
    return std::nullopt;
}

std::optional<bool> jsonBool(matjson::Value const& obj, std::string_view key) {
    if (!obj.isObject() || !obj.contains(key)) return std::nullopt;
    if (auto res = obj[key].asBool()) return res.unwrap();
    return std::nullopt;
}

std::optional<double> jsonNumber(matjson::Value const& obj, std::string_view key) {
    if (!obj.isObject() || !obj.contains(key)) return std::nullopt;
    if (auto res = obj[key].asDouble()) return res.unwrap();
    return std::nullopt;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Accepts "#RRGGBB", "RRGGBB", "#RGB" and "RGB".
std::optional<ccColor3B> parseColor(std::string_view text) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);

    for (char c : text) {
        if (hexDigit(c) < 0) return std::nullopt;
    }

    // "#RRGGBB": each channel is two hex digits.
    if (text.size() == 6) {
        return ccColor3B {
            static_cast<GLubyte>(hexDigit(text[0]) * 16 + hexDigit(text[1])),
            static_cast<GLubyte>(hexDigit(text[2]) * 16 + hexDigit(text[3])),
            static_cast<GLubyte>(hexDigit(text[4]) * 16 + hexDigit(text[5])),
        };
    }
    // "#RGB": each digit is doubled (0xF -> 0xFF), the usual CSS shorthand.
    if (text.size() == 3) {
        return ccColor3B {
            static_cast<GLubyte>(hexDigit(text[0]) * 17),
            static_cast<GLubyte>(hexDigit(text[1]) * 17),
            static_cast<GLubyte>(hexDigit(text[2]) * 17),
        };
    }
    return std::nullopt;
}

std::string colorToString(ccColor3B color) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color.r, color.g, color.b);
    return buffer;
}

/// Leading/trailing whitespace removed, for names typed into the preset text field.
std::string trimCopy(std::string_view text) {
    auto first = text.begin();
    auto last = text.end();
    while (first != last && std::isspace(static_cast<unsigned char>(*first))) ++first;
    while (first != last && std::isspace(static_cast<unsigned char>(*(last - 1)))) --last;
    return std::string(first, last);
}

/// Accepts booleans as well as the strings "normal", "additive", "add", "blend", "blending".
std::optional<bool> parseBlending(matjson::Value const& obj) {
    if (!obj.contains("blending")) return std::nullopt;

    if (auto asBool = obj["blending"].asBool()) return asBool.unwrap();

    if (auto asStr = obj["blending"].asString()) {
        auto value = asStr.unwrap();
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        auto norm = lower(value);
        if (norm == "additive" || norm == "add" || norm == "blend" || norm == "blending") {
            return true;
        }
        if (norm == "normal" || norm == "alpha" || norm == "none") {
            return false;
        }
    }
    return std::nullopt;
}

/// Decimal or "0x" hex integer without throwing (used by parseKey).
std::optional<int> parseInt(std::string_view text);

/// Key names accepted in "hotkeys": F1..F24, a..z, 0..9, the names in kNamedKeys, or a raw
/// key code as a decimal / 0x hex number (which is what GD's own key list uses).
std::optional<cocos2d::enumKeyCodes> parseKey(std::string_view text) {
    std::string key;
    key.reserve(text.size());
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (key.empty() || key == "none" || key == "off" || key == "disabled") {
        return KEY_None;
    }

    if (key.size() == 1) {
        char c = key[0];
        if (c >= 'a' && c <= 'z') return static_cast<cocos2d::enumKeyCodes>(KEY_A + (c - 'a'));
        if (c >= '0' && c <= '9') return static_cast<cocos2d::enumKeyCodes>(KEY_Zero + (c - '0'));
    }

    if (key.size() >= 2 && key[0] == 'f') {
        if (auto number = parseInt(key.substr(1)); number && *number >= 1 && *number <= 24) {
            return static_cast<cocos2d::enumKeyCodes>(KEY_F1 + (*number - 1));
        }
    }

    static constexpr std::pair<std::string_view, cocos2d::enumKeyCodes> kNamedKeys[] = {
        { "space", KEY_Space }, { "enter", KEY_Enter }, { "return", KEY_Enter },
        { "tab", KEY_Tab }, { "escape", KEY_Escape }, { "esc", KEY_Escape },
        { "backspace", KEY_Backspace }, { "shift", KEY_Shift }, { "ctrl", KEY_Control },
        { "control", KEY_Control }, { "alt", KEY_Alt }, { "capslock", KEY_CapsLock },
        { "pause", KEY_Pause }, { "insert", KEY_Insert }, { "delete", KEY_Delete },
        { "del", KEY_Delete }, { "home", KEY_Home }, { "end", KEY_End },
        { "pageup", KEY_PageUp }, { "pagedown", KEY_PageDown },
        { "up", KEY_Up }, { "down", KEY_Down }, { "left", KEY_Left }, { "right", KEY_Right },
        { "numlock", KEY_Numlock }, { "scrolllock", KEY_ScrollLock },
    };
    for (auto const& [name, code] : kNamedKeys) {
        if (key == name) return code;
    }

    if (auto number = parseInt(key)) {
        if (*number > 0 && *number < 0x1000) {
            return static_cast<cocos2d::enumKeyCodes>(*number);
        }
    }
    return std::nullopt;
}

std::optional<int> parseInt(std::string_view text) {
    if (text.empty()) return std::nullopt;

    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }

    int value = 0;
    for (char c : text) {
        int digit = (base == 16) ? hexDigit(c) : (c >= '0' && c <= '9' ? c - '0' : -1);
        if (digit < 0 || digit >= base) return std::nullopt;
        value = value * base + digit;
        if (value > 0x100000) return std::nullopt;
    }
    return value;
}

// ---------------------------------------------------------------------------------------
// Runtime colour tweaking (hue / brightness hotkeys)
// ---------------------------------------------------------------------------------------

struct Hsv { float h; float s; float v; };

Hsv rgbToHsv(ccColor3B color) {
    float r = color.r / 255.0f, g = color.g / 255.0f, b = color.b / 255.0f;
    float maxValue = std::max({ r, g, b });
    float minValue = std::min({ r, g, b });
    float delta = maxValue - minValue;

    Hsv hsv { 0.0f, maxValue > 0.0f ? delta / maxValue : 0.0f, maxValue };
    if (delta > 0.0f) {
        if (maxValue == r)      hsv.h = 60.0f * std::fmod((g - b) / delta, 6.0f);
        else if (maxValue == g) hsv.h = 60.0f * ((b - r) / delta + 2.0f);
        else                    hsv.h = 60.0f * ((r - g) / delta + 4.0f);
    }
    if (hsv.h < 0.0f) hsv.h += 360.0f;
    return hsv;
}

ccColor3B hsvToRgb(Hsv hsv) {
    float c = hsv.v * hsv.s;
    float h = std::fmod(hsv.h, 360.0f) / 60.0f;
    if (h < 0.0f) h += 6.0f;
    float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float m = hsv.v - c;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if      (h < 1.0f) { r = c; g = x; }
    else if (h < 2.0f) { r = x; g = c; }
    else if (h < 3.0f) { g = c; b = x; }
    else if (h < 4.0f) { g = x; b = c; }
    else if (h < 5.0f) { r = x; b = c; }
    else               { r = c; b = x; }

    auto channel = [&](float value) {
        return static_cast<GLubyte>(std::lround(std::clamp(value + m, 0.0f, 1.0f) * 255.0f));
    };
    return ccColor3B { channel(r), channel(g), channel(b) };
}

// ---------------------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------------------

/// Seconds since the first call, from a monotonic clock. Deliberately not the game's timers:
/// the RGB phase and the colour fade must keep running across respawns and level restarts,
/// and must not be affected by pauses or by the game's own time scaling.
float monotonicSeconds() {
    static auto const epoch = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - epoch).count();
}

ccColor3B lerpColor(ccColor3B from, ccColor3B to, float t) {
    auto mix = [t](GLubyte a, GLubyte b) {
        return static_cast<GLubyte>(std::lround(a + (b - a) * t));
    };
    return ccColor3B { mix(from.r, to.r), mix(from.g, to.g), mix(from.b, to.b) };
}

class Manager {
public:
    static Manager& get() {
        static Manager instance;
        return instance;
    }

    /// Reads (and, on first run, creates) <config dir>/per_gamemode_glow.json
    void loadConfig() {
        // Mod::get()->getConfigDir() already creates the per-mod folder
        // (Mod::Impl::getConfigDir(create = true)). The explicit call is kept as
        // belt and braces: neither writeStringSafe nor readJson create parent
        // folders themselves, and a fresh install should never end up with the
        // write failing and the mod silently doing nothing.
        auto configDir = Mod::get()->getConfigDir();
        (void) utils::file::createDirectoryAll(configDir);

        m_configPath = configDir / "per_gamemode_glow.json";

        if (!fs::exists(m_configPath)) {
            writeDefaultConfig();
        }

        // Everything is reset first so a reload really is "the file is the truth":
        // profiles, presets and the runtime colour tweaks all go back to defaults.
        resetToDefaults();

        auto json = utils::file::readJson(m_configPath);
        if (!json) {
            log::error("Per-Gamemode Glow: cannot read {}: {}",
                utils::string::pathToString(m_configPath), json.unwrapErr());
            return;
        }

        auto const& root = json.unwrap();
        if (!root.isObject()) {
            log::error("Per-Gamemode Glow: {} is not a JSON object",
                utils::string::pathToString(m_configPath));
            return;
        }

        // Kept so saveConfig() can update only the keys the GUI owns and leave hotkeys,
        // presets and any hand-written extras exactly as they were.
        m_rawConfig = root;

        if (auto v = jsonBool(root, "debug")) m_debug = *v;
        if (auto v = jsonNumber(root, "fade")) {
            m_fadeSeconds = std::clamp(static_cast<float>(*v), 0.0f, 3.0f);
        }

        std::size_t found = 0;
        for (std::size_t i = 0; i < kModeCount; ++i) {
            auto key = std::string(kModeKeys[i]);
            if (!root.contains(key)) continue;

            auto const& entry = root[key];
            if (!entry.isObject()) continue;

            auto& profile = m_profiles[i];
            profile = Profile {};

            if (auto v = jsonBool(entry, "enabled"))     profile.enabled = *v;
            if (auto v = jsonNumber(entry, "brightness")) profile.brightness = static_cast<float>(*v);
            if (auto v = jsonNumber(entry, "pulse"))      profile.pulse = static_cast<float>(*v);
            if (auto v = parseBlending(entry))            profile.additive = *v;
            if (auto v = jsonString(entry, "color")) {
                if (auto color = parseColor(*v)) {
                    profile.color = *color;
                } else {
                    log::warn("Per-Gamemode Glow: '{}' has an invalid colour '{}' (expected #RRGGBB)",
                        key, *v);
                }
            }

            profile.brightness = std::clamp(profile.brightness, 0.0f, 2.0f);
            profile.pulse      = std::max(profile.pulse, 0.0f);
            ++found;
        }

        loadHotkeys(root);
        loadPresets(root);
        loadRgb(root);

        log::info("Per-Gamemode Glow: loaded {} mode profile(s), {} preset(s), RGB {} from {}",
            found, m_presets.size(), m_rgbOn ? "on" : "off",
            utils::string::pathToString(m_configPath));

        // Geometry Dash draws the robot's and the spider's glow itself, from the icon kit's glow
        // colour, and ignores the per-player custom glow those two modes would need -- verified
        // with the debug dump (MODLOG section 9). Say so once instead of letting an enabled
        // profile silently do nothing.
        for (auto index : { std::size_t(Mode::Robot), std::size_t(Mode::Spider) }) {
            if (!m_profiles[index].enabled) continue;
            log::warn("Per-Gamemode Glow: '{}' is enabled, but Geometry Dash draws this mode's glow "
                      "itself from the icon kit's glow colour and ignores the mod's colour",
                kModeKeys[index]);
        }

        // Remembered file state, so the watcher below can tell "changed on disk" from
        // "the file we just read".
        std::error_code ec;
        m_configWriteTime = fs::last_write_time(m_configPath, ec);
    }

    /// Applies the profile for the player's current gamemode to its glow sprites.
    /// Called every frame and on art changes, so it always wins over the game's own writes.
    void apply(PlayerObject* player, float elapsed, bool& customGlowOwned) {
        if (!player) return;

        // Hot reload: re-read the config when the file changes on disk, so colours can be
        // adjusted in any editor without restarting the game. Checked at most twice a
        // second, and only ever does anything if the timestamp actually moved.
        pollConfigFile();

        auto mode = modeOf(player);
        m_lastMode = mode;           // the hotkeys tweak "the mode the player is in"

        auto const& profile = m_profiles[static_cast<std::size_t>(mode)];

        // Every way this mode can mean "leave it vanilla", in one place, so that turning a
        // profile off really does hand the glow colour back to the game. RGB mode is an
        // override of every mode at once, so while it is on a disabled profile still glows.
        auto vanilla = !Mod::get()->getSettingValue<bool>("enabled")
            || (!m_rgbOn && !profile.enabled);
        if (!vanilla && Mod::get()->getSettingValue<bool>("respect-global-toggle")) {
            auto* gameManager = GameManager::get();
            vanilla = gameManager && !gameManager->getPlayerGlow();
        }
        if (vanilla) {
            releaseCustomGlow(player, customGlowOwned);
            return;
        }

        auto const index = static_cast<std::size_t>(mode);

        // RGB mode: the hue walks around the colour wheel as the player exists. Derived from
        // the time the player has been alive rather than accumulated per frame, so the two
        // apply() call sites (update and updatePlayerArt) always agree on the colour.
        ccColor3B color;
        float brightness;
        bool additive;
        if (m_rgbOn) {
            float turns = 0.0f;
            if (m_rgb.speed > 0.0f) {
                // Wall-clock based, not the player's own timer: a respawn or a level restart
                // used to send the wheel back to red, which read as an abrupt jump.
                turns = std::fmod(monotonicSeconds() * m_rgb.speed, 1.0f);
            }
            auto hue = turns * 360.0f + static_cast<float>(m_hueShift[index]);
            color = hsvToRgb(Hsv { hue, m_rgb.saturation, 1.0f });
            brightness = m_rgb.brightness * m_brightnessMul[index];
            additive = m_rgb.additive;
        } else {
            color = effectiveColor(index);
            brightness = profile.brightness * m_brightnessMul[index];
            if (profile.pulse > 0.0f) {
                constexpr float kTau = 6.28318530718f;
                brightness *= 0.5f + 0.5f * std::sin(kTau * elapsed / profile.pulse);
            }
            additive = profile.additive;
        }

        // Never snap to a new colour in a single frame: turning the RGB mode on or off, or
        // editing a colour in the config, otherwise jumps the glow from one colour to another.
        color = fadeColor(index, color);

        // Keep the unclamped value: opacity saturates at 1, but everything above it is used
        // as a real "overdrive" (brighter tint + slightly larger glow), so the GUI's 0..2
        // brightness slider does something across its whole range instead of flat-lining
        // between 1 and 2.
        float const overdrive = std::max(brightness, 0.0f);
        brightness = std::clamp(brightness, 0.0f, 1.0f);
        auto const alpha = static_cast<GLubyte>(std::lround(brightness * 255.0f));

        ccBlendFunc const blend = additive
            ? ccBlendFunc { GL_SRC_ALPHA, GL_ONE }
            : ccBlendFunc { GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };

        logDebug(player, mode, index, color, alpha, additive);

        // The game's own per-player custom glow colour: this is the colour the game itself
        // tints glow with, and it is what makes the robot and the spider work at all -- both
        // hide m_iconGlow and m_vehicleGlow (debug log shows vis=0 for each) and draw their
        // glow as part of the limb animation, so tinting sprites never reached them.
        //
        // PlayerObject::enableCustomGlowColor() / disableCustomGlowColor() are "Out of line"
        // on Windows (inlined, no callable symbol), but they are no-ops beyond these two
        // field writes, which is exactly what the game's own glow update reads.
        player->m_glowColor = color;
        player->m_hasCustomGlowColor = true;
        customGlowOwned = true;

        for (auto* sprite : { player->m_iconGlow, player->m_vehicleGlow }) {
            applyGlowSprite(sprite, color, alpha, blend, overdrive);
        }

        applyPartGlow(player, mode, color, alpha, blend);
    }

    /// Handles one keyboard event. Never consumes the key: the caller always forwards it.
    ///
    /// One physical press must fire exactly once. Both Geode and the game itself can route the
    /// same key-down through dispatchKeyboardMSG, which fired every hotkey twice -- harmless for
    /// "add 15 degrees", but a toggle (the RGB mode) immediately undid itself and looked dead.
    void onKeyEvent(cocos2d::enumKeyCodes key, bool isKeyDown, bool isKeyRepeat) {
        if (!m_hotkeysEnabled || key == KEY_None) return;

        for (std::size_t i = 0; i < kHotkeyCount; ++i) {
            if (m_hotkeys[i] != key) continue;

            if (!isKeyDown) {
                if (m_debug && m_hotkeyHeld[i]) {
                    log::info("Per-Gamemode Glow: [debug] '{}' key up", kHotkeyKeys[i]);
                }
                m_hotkeyHeld[i] = false;
                return;
            }

            if (m_debug) {
                log::info("Per-Gamemode Glow: [debug] '{}' key down (repeat={})",
                    kHotkeyKeys[i], isKeyRepeat ? 1 : 0);
            }

            // The OS auto-repeat of a key that is already down is not a new press.
            if (isKeyRepeat) return;

            auto now = std::chrono::steady_clock::now();
            if (m_hotkeyHeld[i] && now - m_hotkeyDownTime[i] < kHotkeyHeldTimeout) {
                if (m_debug) {
                    log::info("Per-Gamemode Glow: [debug] '{}' duplicate key-down ignored",
                        kHotkeyKeys[i]);
                }
                return;
            }

            m_hotkeyHeld[i] = true;
            m_hotkeyDownTime[i] = now;
            fireHotkey(i);
            return;
        }
    }

    void fireHotkey(std::size_t i) {
        switch (static_cast<Hotkey>(i)) {
            case Hotkey::Reload: {
                log::info("Per-Gamemode Glow: '{}' pressed - reloading {}",
                    kHotkeyKeys[i], utils::string::pathToString(m_configPath));
                loadConfig();
                break;
            }
            case Hotkey::PresetNext: cyclePreset(+1); break;
            case Hotkey::PresetPrev: cyclePreset(-1); break;
            case Hotkey::HueDown:    shiftHue(-kHueStep); break;
            case Hotkey::HueUp:      shiftHue(+kHueStep); break;
            case Hotkey::BrightnessDown: shiftBrightness(-kBrightnessStep); break;
            case Hotkey::BrightnessUp:   shiftBrightness(+kBrightnessStep); break;
            case Hotkey::RgbToggle:      toggleRgb(); break;
            case Hotkey::RgbSlower:      adjustRgbSpeed(-1); break;
            case Hotkey::RgbFaster:      adjustRgbSpeed(+1); break;
            case Hotkey::OpenGui:        openGui(); break;
            case Hotkey::Count: break;
        }
    }

    // -------------------------------------------------------------------------------------
    // GUI API
    //
    // The settings popup edits exactly the same state the hotkeys and the JSON loader use:
    // every setter applies on the next frame (PlayerObject::update re-reads Manager), and
    // saveConfig() writes the whole state back to per_gamemode_glow.json. The raw parsed
    // document is kept so saving only touches the keys the GUI owns -- hotkeys, presets and
    // any keys a hand-written config added survive a save untouched.
    // -------------------------------------------------------------------------------------

    struct ProfileView {
        bool      enabled    = false;
        ccColor3B color      = { 255, 255, 255 };
        float     brightness = 1.0f;
        bool      additive   = false;
        float     pulse      = 0.0f;
    };

    static std::string_view modeKey(std::size_t index) {
        return index < kModeCount ? kModeKeys[index] : std::string_view {};
    }

    ProfileView profileView(std::size_t index) const {
        auto const& profile = m_profiles[index];
        // effectiveColor() includes the active preset and the runtime hue shift, so the GUI
        // swatch always shows the colour that is actually on the player's glow.
        return ProfileView { profile.enabled, effectiveColor(index), profile.brightness,
                             profile.additive, profile.pulse };
    }

    void setProfileEnabled(std::size_t index, bool value) {
        if (index < kModeCount) m_profiles[index].enabled = value;
    }

    /// A colour picked in the GUI is the colour the player should see, so it also clears the
    /// runtime hue shift (F8/F9) for that mode -- otherwise the swatch and the glow would
    /// disagree until the next config reload. When a preset is active the edit is written
    /// into that preset too, so picking a colour never looks like a no-op.
    void setProfileColor(std::size_t index, ccColor3B color) {
        if (index >= kModeCount) return;
        m_profiles[index].color = color;
        m_hueShift[index] = 0;
        if (m_activePreset >= 0 && m_activePreset < static_cast<int>(m_presets.size())) {
            m_presets[m_activePreset].colors[index] = color;
        }
    }

    /// Likewise resets the hotkey brightness multiplier, so the slider value is the real one.
    void setProfileBrightness(std::size_t index, float value) {
        if (index >= kModeCount) return;
        m_profiles[index].brightness = std::clamp(value, 0.0f, 2.0f);
        m_brightnessMul[index] = 1.0f;
    }

    void setProfileAdditive(std::size_t index, bool value) {
        if (index < kModeCount) m_profiles[index].additive = value;
    }

    void setProfilePulse(std::size_t index, float value) {
        if (index < kModeCount) m_profiles[index].pulse = std::max(value, 0.0f);
    }

    RgbSettings const& rgb() const { return m_rgb; }
    bool rgbOn() const { return m_rgbOn; }
    void setRgbOn(bool value) { m_rgbOn = value; }
    void setRgbSpeed(float value) { m_rgb.speed = std::clamp(value, 0.0f, kRgbMaxSpeed); }
    void setRgbSaturation(float value) { m_rgb.saturation = std::clamp(value, 0.0f, 1.0f); }
    void setRgbBrightness(float value) { m_rgb.brightness = std::clamp(value, 0.0f, 2.0f); }
    void setRgbAdditive(bool value) { m_rgb.additive = value; }

    bool debug() const { return m_debug; }
    void setDebug(bool value) { m_debug = value; }
    float fadeSeconds() const { return m_fadeSeconds; }
    void setFadeSeconds(float value) { m_fadeSeconds = std::clamp(value, 0.0f, 3.0f); }

    // ---- presets (GUI) ---------------------------------------------------------------
    //
    // A preset is a named set of per-mode colours (see Preset above). The GUI can cycle
    // them, turn them off ("custom colours"), and create / overwrite / delete them. The
    // active preset is persisted as "preset": "<name>" when saveConfig() runs.

    int presetCount() const { return static_cast<int>(m_presets.size()); }
    int activePreset() const { return m_activePreset; }

    std::string presetName(int index) const {
        if (index < 0 || index >= static_cast<int>(m_presets.size())) return {};
        return m_presets[static_cast<std::size_t>(index)].name;
    }

    void setActivePreset(int index) {
        if (index < -1 || index >= static_cast<int>(m_presets.size())) return;
        m_activePreset = index;
    }

    void stepPreset(int step) { cyclePreset(step); }

    /// "Preset 1", "Preset 2", ... skipping names that already exist.
    std::string suggestPresetName() const {
        for (int n = 1; n < 1000; ++n) {
            auto name = "Preset " + std::to_string(n);
            if (findPreset(name) < 0) return name;
        }
        return "Preset";
    }

    /// Stores the colours currently visible on the player under `name`. If a preset with
    /// that name already exists it is overwritten; otherwise a new one is appended. Returns
    /// the final name (an empty name becomes the next free "Preset N").
    std::string savePreset(std::string name) {
        name = trimCopy(name);
        if (name.empty()) name = suggestPresetName();
        if (name.size() > 40) name.resize(40);

        auto index = findPreset(name);
        if (index < 0) {
            Preset preset;
            preset.name = name;
            m_presets.push_back(std::move(preset));
            index = static_cast<int>(m_presets.size()) - 1;
        }

        auto& preset = m_presets[static_cast<std::size_t>(index)];
        for (std::size_t i = 0; i < kModeCount; ++i) {
            preset.colors[i] = effectiveColor(i);
        }
        m_activePreset = index;
        return preset.name;
    }

    bool deleteActivePreset() {
        if (m_activePreset < 0 || m_activePreset >= static_cast<int>(m_presets.size())) {
            return false;
        }
        m_presets.erase(m_presets.begin() + m_activePreset);
        m_activePreset = -1;
        return true;
    }

    std::string configPathString() const {
        return m_configPath.empty() ? std::string("(not loaded yet)")
                                    : utils::string::pathToString(m_configPath);
    }

    void reloadConfig() { loadConfig(); }

    bool saveConfig() {
        if (m_configPath.empty()) return false;

        auto root = m_rawConfig.isObject() ? m_rawConfig : matjson::Value::object();
        root.set("debug", m_debug);
        root.set("fade", static_cast<double>(m_fadeSeconds));

        if (!root.contains("rgb") || !root["rgb"].isObject()) {
            root.set("rgb", matjson::Value::object());
        }
        auto& rgb = root["rgb"];
        rgb.set("enabled", m_rgbOn);
        rgb.set("speed", static_cast<double>(m_rgb.speed));
        rgb.set("saturation", static_cast<double>(m_rgb.saturation));
        rgb.set("brightness", static_cast<double>(m_rgb.brightness));
        rgb.set("blending", std::string(m_rgb.additive ? "additive" : "normal"));

        for (std::size_t i = 0; i < kModeCount; ++i) {
            auto key = std::string(kModeKeys[i]);
            if (!root.contains(key) || !root[key].isObject()) {
                root.set(key, matjson::Value::object());
            }

            auto& entry = root[key];
            auto const& profile = m_profiles[i];
            entry.set("enabled", profile.enabled);
            entry.set("color", colorToString(profile.color));
            entry.set("brightness", static_cast<double>(profile.brightness));
            entry.set("blending", std::string(profile.additive ? "additive" : "normal"));
            entry.set("pulse", static_cast<double>(profile.pulse));
        }

        // Presets are owned by the GUI now, so the whole array is rewritten. Anything a
        // hand-written preset stored outside "name"/"colors" is intentionally dropped; the
        // documented format is exactly those two keys.
        auto presets = matjson::Value::array();
        for (auto const& preset : m_presets) {
            auto entry = matjson::Value::object();
            entry.set("name", preset.name);

            auto colors = matjson::Value::object();
            for (std::size_t i = 0; i < kModeCount; ++i) {
                if (preset.colors[i]) {
                    colors.set(std::string(kModeKeys[i]), colorToString(*preset.colors[i]));
                }
            }
            entry.set("colors", std::move(colors));
            presets.push(std::move(entry));
        }
        root.set("presets", std::move(presets));
        root.set("preset", m_activePreset >= 0 && m_activePreset < static_cast<int>(m_presets.size())
            ? m_presets[static_cast<std::size_t>(m_activePreset)].name
            : std::string(""));

        auto written = utils::file::writeStringSafe(m_configPath, root.dump(4));
        if (!written) {
            log::error("Per-Gamemode Glow: could not save config: {}", written.unwrapErr());
            return false;
        }

        m_rawConfig = std::move(root);
        std::error_code ec;
        m_configWriteTime = fs::last_write_time(m_configPath, ec);
        log::info("Per-Gamemode Glow: saved config to {}", configPathString());
        return true;
    }

private:
    Manager() = default;

    static Mode modeOf(PlayerObject* player) {
        if (player->m_isShip)   return Mode::Ship;
        if (player->m_isBird)   return Mode::Ufo;
        if (player->m_isBall)   return Mode::Ball;
        if (player->m_isDart)   return Mode::Wave;
        if (player->m_isRobot)  return Mode::Robot;
        if (player->m_isSpider) return Mode::Spider;
        if (player->m_isSwing)  return Mode::Swing;
        return Mode::Cube;
    }

    /// The robot and the spider do not use m_iconGlow / m_vehicleGlow at all: the game hides both
    /// (debug log: "iconGlow was #0040FF a=76 vis=0 | vehicleGlow was #0040FF a=76 vis=0") and
    /// draws the outline the player sees with its own glow pass, which follows the icon kit's
    /// glow colour and ignores the per-player custom glow this mod sets. Their sprite
    /// (GJSpiderSprite derives from GJRobotSprite, so both share the fields) still has the two
    /// glow sprites below, so they are tinted anyway -- it costs nothing and would make the mode
    /// work on a build whose robot/spider glow does read the custom glow.
    static GJRobotSprite* partSpriteOf(PlayerObject* player, Mode mode) {
        if (mode == Mode::Robot) return player->m_robotSprite;
        if (mode == Mode::Spider) return player->m_spiderSprite;
        return nullptr;
    }

    /// GD names every glow frame "<icon>_<id>_glow_<frame>.png", so a glow part of a
    /// robot/spider is one whose frame name says so.
    static bool isGlowPart(cocos2d::CCNode* node) {
        auto* part = typeinfo_cast<CCSpritePart*>(node);
        return part && part->m_spriteFrameName.find("glow") != std::string::npos;
    }

    static void tintSprite(cocos2d::CCSprite* sprite, ccColor3B color, GLubyte alpha,
                           ccBlendFunc blend) {
        if (!sprite) return;
        sprite->setColor(color);
        sprite->setOpacity(alpha);
        sprite->setBlendFunc(blend);
    }

    /// Brightness above 1 cannot be expressed as sprite opacity (255 is the maximum), so it
    /// also brightens the tint and grows the glow slightly. That makes the top half of the
    /// slider visibly different from the middle instead of being silently clamped.
    static ccColor3B overdriveColor(ccColor3B color, float brightness) {
        if (brightness <= 1.0f) return color;
        float const factor = std::min(brightness, 2.0f);
        auto channel = [factor](GLubyte value) {
            return static_cast<GLubyte>(std::clamp<long>(std::lround(value * factor), 0, 255));
        };
        return ccColor3B { channel(color.r), channel(color.g), channel(color.b) };
    }

    void applyGlowSprite(cocos2d::CCSprite* sprite, ccColor3B color, GLubyte alpha,
                         ccBlendFunc blend, float brightness) {
        if (!sprite) return;

        sprite->setColor(overdriveColor(color, brightness));
        sprite->setOpacity(alpha);
        sprite->setBlendFunc(blend);

        // The game rewrites the glow sprite's scale during art updates, so remember the last
        // value *we* applied: a different current scale means the game just set a new base.
        if (m_glowScale.size() > 64) m_glowScale.clear();

        float const overdrive = 1.0f + std::max(brightness - 1.0f, 0.0f) * 0.35f;
        auto& state = m_glowScale[sprite];
        float const current = sprite->getScale();
        if (state.applied <= 0.0f || std::abs(current - state.applied) > 0.0001f) {
            state.base = current;
        }
        float const target = state.base * overdrive;
        sprite->setScale(target);
        state.applied = target;
    }

    /// Hands the glow colour back to the game, but only if we are the ones who took it over:
    /// a level's own custom glow colour must not be cleared by a mode this mod leaves alone.
    static void releaseCustomGlow(PlayerObject* player, bool& owned) {
        if (!owned) return;
        player->m_hasCustomGlowColor = false;
        owned = false;
    }

    static void applyPartGlow(PlayerObject* player, Mode mode, ccColor3B color,
                              GLubyte alpha, ccBlendFunc blend) {
        auto* part = partSpriteOf(player, mode);
        if (!part) return;

        tintSprite(part->m_glowSprite, color, alpha, blend);

        // The spider's eye pupil is m_extraSprite, and tinting it is the one thing that makes
        // this mod's colour visible on that icon at all (GD draws the spider's outline itself,
        // see MODLOG section 9). This started as a side effect of a failed fix for the outline
        // and was kept on request: a glowing eye at least shows the mode is doing something.
        // Spider only, on purpose: a robot's m_extraSprite is a different piece of the icon (the
        // game gates it with m_hasExtra) and recolouring icon art is not what this mod does.
        if (mode == Mode::Spider) {
            tintSprite(part->m_extraSprite, color, alpha, blend);
        }
        auto tintGlowParts = [color, alpha, blend](cocos2d::CCArray* array) {
            if (!array) return;
            for (auto* object : CCArrayExt<cocos2d::CCObject*>(array)) {
                auto* node = typeinfo_cast<cocos2d::CCNode*>(object);
                if (isGlowPart(node)) tintSprite(typeinfo_cast<cocos2d::CCSprite*>(node), color, alpha, blend);
            }
        };
        tintGlowParts(part->m_unkArray);
        tintGlowParts(part->m_secondArray);
        if (part->m_paSprite) tintGlowParts(part->m_paSprite->m_spriteParts);
    }

    /// "<frame>:<colour> a=<alpha> vis=<visible>" for one node. Frame names come only from
    /// CCSpritePart's own field: CCSprite::displayFrame() / CCSpriteFrame::getFrameName()
    /// dispatch through a vtable that does not match the vendored cocos2d headers, and the
    /// resulting std::string crashed the game with std::length_error.
    static std::string describeNode(cocos2d::CCNode* node) {
        if (!node) return "null";

        auto* sprite = typeinfo_cast<cocos2d::CCSprite*>(node);
        if (!sprite) {
            char other[32];
            std::snprintf(other, sizeof(other), "node tag=%d", node->getTag());
            return std::string(other);
        }

        std::string name = "-";
        if (auto* part = typeinfo_cast<CCSpritePart*>(sprite)) name = part->m_spriteFrameName;

        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s:%s a=%d vis=%d", name.c_str(),
            colorToString(sprite->getColor()).c_str(), sprite->getOpacity(),
            sprite->isVisible() ? 1 : 0);
        return std::string(buffer);
    }

    /// " <prefix>0=<describeNode> <prefix>1=..." for up to `limit` children of a node. The child
    /// list is how a sprite that is not one of the robot's own fields can still be found.
    static std::string describeChildren(cocos2d::CCNode* node, std::string_view prefix, int limit) {
        std::string out;
        if (!node) return out;

        auto* children = node->getChildren();
        if (!children) return out;

        int index = 0;
        for (auto* child : CCArrayExt<cocos2d::CCNode*>(children)) {
            if (index >= limit) { break; }
            char label[32];
            std::snprintf(label, sizeof(label), "%.8s%d", prefix.data(), index++);
            out += ' ';
            out += label;
            out += '=';
            out += describeNode(child);
        }
        return out;
    }

    /// Debug-only: what a robot/spider sprite is made of at the moment we look at it. This exists
    /// because GD does not draw either of those two modes' own glow sprites: the dump below is
    /// what proved that the outline the player sees is drawn by the game itself -- every visible
    /// sprite in the tree carries the icon's own colours, never the glow colour, and both glow
    /// sprites end up holding our colour without being drawn from it (MODLOG section 9).
    /// Everything here sits in a try/catch on purpose: a debug dump must never take the game
    /// down (an earlier version called CCSprite::displayFrame() and crashed with length_error).
    static std::string describeRobot(GJRobotSprite* robot) {
        if (!robot) return "-";

        try {
            std::string out;
            auto append = [&out](std::string_view tag, cocos2d::CCNode* node) {
                out += ' ';
                out += tag;
                out += '=';
                out += describeNode(node);
            };
            auto appendArray = [&append](std::string_view tag, cocos2d::CCArray* array) {
                if (!array) return;
                int index = 0;
                for (auto* object : CCArrayExt<cocos2d::CCObject*>(array)) {
                    if (index >= 8) { break; }
                    char label[24];
                    std::snprintf(label, sizeof(label), "%.8s%d", tag.data(), index++);
                    append(label, typeinfo_cast<cocos2d::CCNode*>(object));
                }
            };

            char own[80];
            std::snprintf(own, sizeof(own), " ownColor=%s/%s extra=%d",
                colorToString(robot->m_color).c_str(), colorToString(robot->m_secondColor).c_str(),
                robot->m_hasExtra ? 1 : 0);
            out += own;

            append("glow", robot->m_glowSprite);
            append("extra", robot->m_extraSprite);
            append("base", robot->m_sprite);
            append("fbf", robot->m_fbfSprite);
            append("head", robot->m_headSprite);
            append("foot", robot->m_footSprite);
            appendArray("unk", robot->m_unkArray);
            appendArray("second", robot->m_secondArray);
            appendArray("part", robot->m_paSprite ? robot->m_paSprite->m_spriteParts : nullptr);
            out += describeChildren(robot, "child", 4);

            return out;
        } catch (std::exception const& e) {
            return std::string("dump failed: ") + e.what();
        } catch (...) {
            return "dump failed";
        }
    }

    void resetToDefaults() {
        m_profiles = {};
        m_rgb = RgbSettings {};
        m_rgbOn = false;
        m_fade = {};
        m_fadeSeconds = kDefaultFadeSeconds;
        m_presets.clear();
        m_activePreset = -1;
        m_debug = false;
        m_hotkeysEnabled = true;
        for (std::size_t i = 0; i < kHotkeyCount; ++i) {
            m_hotkeys[i] = kDefaultHotkeys[i];
        }
        m_hueShift = {};
        for (auto& value : m_brightnessMul) value = 1.0f;
    }

    void loadHotkeys(matjson::Value const& root) {
        if (!root.contains("hotkeys")) return;
        auto const& hotkeys = root["hotkeys"];
        if (!hotkeys.isObject()) {
            log::warn("Per-Gamemode Glow: 'hotkeys' is not an object, keeping the defaults");
            return;
        }

        if (auto v = jsonBool(hotkeys, "enabled")) m_hotkeysEnabled = *v;

        for (std::size_t i = 0; i < kHotkeyCount; ++i) {
            auto name = std::string(kHotkeyKeys[i]);
            if (!hotkeys.contains(name)) continue;

            auto value = jsonString(hotkeys, name);
            if (!value) {
                log::warn("Per-Gamemode Glow: hotkey '{}' must be a string, keeping the default", name);
                continue;
            }
            if (auto key = parseKey(*value)) {
                m_hotkeys[i] = *key;
            } else {
                log::warn("Per-Gamemode Glow: hotkey '{}' has an unknown key '{}' (keeping the default)",
                    name, *value);
            }
        }
    }

    void loadPresets(matjson::Value const& root) {
        auto activeName = jsonString(root, "preset");

        if (root.contains("presets")) {
            auto const& presets = root["presets"];
            if (!presets.isArray()) {
                log::warn("Per-Gamemode Glow: 'presets' must be an array of "
                          "{{ \"name\": ..., \"colors\": {{ mode: \"#RRGGBB\" }} }}");
            } else {
                for (auto const& entry : presets) {
                    if (!entry.isObject()) continue;

                    Preset preset;
                    preset.name = jsonString(entry, "name").value_or("preset");

                    if (auto const& colors = entry.contains("colors") ? entry["colors"] : entry;
                        colors.isObject()) {
                        for (std::size_t i = 0; i < kModeCount; ++i) {
                            auto key = std::string(kModeKeys[i]);
                            if (!colors.contains(key)) continue;

                            if (auto value = jsonString(colors, key)) {
                                if (auto color = parseColor(*value)) {
                                    preset.colors[i] = *color;
                                } else {
                                    log::warn("Per-Gamemode Glow: preset '{}' has an invalid colour '{}' for '{}'",
                                        preset.name, *value, key);
                                }
                            }
                        }
                    }
                    m_presets.push_back(std::move(preset));
                }
            }
        }

        // "preset": "name" selects one right away (and survives a restart); "" or a missing
        // field means "use the per-mode colours from this file".
        if (activeName && !activeName->empty()) {
            m_activePreset = findPreset(*activeName);
            if (m_activePreset < 0) {
                log::warn("Per-Gamemode Glow: 'preset' names an unknown preset '{}' (using the per-mode colours)",
                    *activeName);
            }
        }
    }

    int findPreset(std::string_view name) const {
        for (std::size_t i = 0; i < m_presets.size(); ++i) {
            if (m_presets[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

    /// Eases the glow towards a target colour with an exponential approach, so no colour change
    /// is ever a single-frame snap. "fade" in the config is the time constant in seconds; 0
    /// applies the colour immediately. Both call sites in a frame share the state, so the second
    /// one (with no time passed) simply keeps the colour the first one faded to.
    ccColor3B fadeColor(std::size_t index, ccColor3B target) {
        auto now = monotonicSeconds();
        auto& state = m_fade[index];

        if (m_fadeSeconds <= 0.0f) {
            state = FadeState { target, now, true };
            return target;
        }

        if (state.valid) {
            float dt = std::max(now - state.time, 0.0f);
            if (dt > 0.0f) {
                float t = std::clamp(1.0f - std::exp(-dt / m_fadeSeconds), 0.0f, 1.0f);
                state.color = lerpColor(state.color, target, t);
            }
        } else {
            state.color = target;
        }
        state.time = now;
        state.valid = true;
        return state.color;
    }

    ccColor3B effectiveColor(std::size_t index) const {
        auto color = m_profiles[index].color;

        if (m_activePreset >= 0 && m_activePreset < static_cast<int>(m_presets.size())) {
            if (auto const& presetColor = m_presets[m_activePreset].colors[index]) {
                color = *presetColor;
            }
        }
        if (m_hueShift[index] != 0) {
            auto hsv = rgbToHsv(color);
            hsv.h += static_cast<float>(m_hueShift[index]);
            color = hsvToRgb(hsv);
        }
        return color;
    }

    void cyclePreset(int step) {
        if (m_presets.empty()) {
            log::info("Per-Gamemode Glow: no presets defined in the config");
            return;
        }

        // -1 means "no preset" (per-mode colours), so cycling starts from the first one.
        auto count = static_cast<int>(m_presets.size());
        m_activePreset = m_activePreset < 0
            ? (step > 0 ? 0 : count - 1)
            : (m_activePreset + step + count) % count;

        auto const& preset = m_presets[m_activePreset];
        log::info("Per-Gamemode Glow: preset {}/{} -> '{}'",
            m_activePreset + 1, count, preset.name);
    }

    void shiftHue(float amount) {
        if (m_lastMode == Mode::Count) {
            log::info("Per-Gamemode Glow: hue keys work while playing (no gamemode seen yet)");
            return;
        }
        auto index = static_cast<std::size_t>(m_lastMode);

        auto shift = m_hueShift[index] + static_cast<int>(amount);
        shift = ((shift % 360) + 360) % 360;
        if (shift > 180) shift -= 360;   // keep it in (-180, 180]
        m_hueShift[index] = shift;

        log::info("Per-Gamemode Glow: '{}' hue {:+d} deg -> {}",
            kModeKeys[index], m_hueShift[index], colorToString(effectiveColor(index)));
    }

    /// Reads the "rgb" block. Absent or malformed values keep their defaults, so a config
    /// written before this block existed behaves exactly as it did.
    void loadRgb(matjson::Value const& root) {
        m_rgb = RgbSettings {};
        m_rgbOn = false;

        if (!root.contains("rgb")) return;

        auto const& node = root["rgb"];
        if (!node.isObject()) {
            log::warn("Per-Gamemode Glow: 'rgb' must be an object like "
                      "{{ \"enabled\": false, \"speed\": 0.25 }} (keeping the defaults)");
            return;
        }

        if (auto v = jsonBool(node, "enabled"))      m_rgb.enabled = *v;
        if (auto v = jsonNumber(node, "speed"))      m_rgb.speed = std::clamp(static_cast<float>(*v), 0.0f, kRgbMaxSpeed);
        if (auto v = jsonNumber(node, "saturation")) m_rgb.saturation = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
        if (auto v = jsonNumber(node, "brightness")) m_rgb.brightness = std::clamp(static_cast<float>(*v), 0.0f, 2.0f);
        if (auto v = parseBlending(node))            m_rgb.additive = *v;

        // The hotkey flips m_rgbOn at runtime only; a reload (and F5) puts it back to whatever
        // the file says, exactly like the hue and brightness tweaks.
        m_rgbOn = m_rgb.enabled;
    }

    void toggleRgb() {
        m_rgbOn = !m_rgbOn;
        // Runtime only: the file is the truth, so a reload (or F5) puts it back.
        log::info("Per-Gamemode Glow: RGB mode {} (speed {:.2f} turns/s, {} blending, "
                  "overrides every mode)",
            m_rgbOn ? "on" : "off", m_rgb.speed, m_rgb.additive ? "additive" : "normal");
    }

    /// The RGB wheel speed is the one value that really has to be tuned by eye, so it gets a
    /// pair of hotkeys that walk a ladder of round values. Runtime only, like the hue and
    /// brightness tweaks; the log line prints the value ready to paste into the config.
    void adjustRgbSpeed(int step) {
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < kRgbSpeedLadderSize; ++i) {
            if (std::abs(kRgbSpeedLadder[i] - m_rgb.speed)
                < std::abs(kRgbSpeedLadder[nearest] - m_rgb.speed)) {
                nearest = i;
            }
        }

        auto next = static_cast<int>(nearest) + step;
        next = std::clamp(next, 0, static_cast<int>(kRgbSpeedLadderSize) - 1);
        m_rgb.speed = kRgbSpeedLadder[next];

        auto secondsPerTurn = m_rgb.speed > 0.0f ? 1.0f / m_rgb.speed : 0.0f;
        log::info("Per-Gamemode Glow: RGB speed {:.4g} turns/s = one full colour wheel in "
                  "{:.0f} s - put \"speed\": {:.4g} in the \"rgb\" block to keep it",
            m_rgb.speed, secondsPerTurn, m_rgb.speed);
    }

    void shiftBrightness(float amount) {
        if (m_lastMode == Mode::Count) {
            log::info("Per-Gamemode Glow: brightness keys work while playing (no gamemode seen yet)");
            return;
        }
        auto index = static_cast<std::size_t>(m_lastMode);

        m_brightnessMul[index] = std::clamp(m_brightnessMul[index] + amount, 0.0f, 2.0f);

        log::info("Per-Gamemode Glow: '{}' brightness x{:.2f} (file value {:.2f}) -> effective {:.2f}",
            kModeKeys[index], m_brightnessMul[index], m_profiles[index].brightness,
            std::clamp(m_profiles[index].brightness * m_brightnessMul[index], 0.0f, 1.0f));
    }

    void pollConfigFile() {
        auto now = std::chrono::steady_clock::now();
        if (now - m_lastReloadCheck < kAutoReloadDelay) return;
        m_lastReloadCheck = now;

        std::error_code ec;
        auto writeTime = fs::last_write_time(m_configPath, ec);
        if (ec || writeTime == m_configWriteTime) return;

        log::info("Per-Gamemode Glow: {} changed on disk - reloading",
            utils::string::pathToString(m_configPath));
        loadConfig();
    }

    /// With "debug": true, logs what is applied and what the game had put in the sprite
    /// before, which is how the robot/spider glow question can be answered from the log.
    void logDebug(PlayerObject* player, Mode mode, std::size_t index, ccColor3B color,
                  GLubyte alpha, bool additive) {
        if (!m_debug) return;

        auto now = std::chrono::steady_clock::now();
        if (m_lastDebugMode == mode && now - m_lastDebugLog < kDebugLogDelay) return;
        m_lastDebugMode = mode;
        m_lastDebugLog = now;

        // The game writes the glow colour itself, so logging the sprites' colour right
        // before we overwrite them shows what the game had decided for this mode (and, on
        // the next line, whether anything wrote over us in between). Both glow sprites are
        // logged: in the vehicle modes (ship/ufo/wave/robot/spider/swing) the game hides
        // m_iconGlow and the visible glow is m_vehicleGlow, and vice versa for the cube.
        auto describe = [](cocos2d::CCSprite* sprite) {
            if (!sprite) return std::string("-");
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%s a=%d vis=%d",
                colorToString(sprite->getColor()).c_str(), sprite->getOpacity(),
                sprite->isVisible() ? 1 : 0);
            return std::string(buffer);
        };

        auto const iconWas = player ? describe(player->m_iconGlow) : std::string("n/a");
        auto const vehicleWas = player ? describe(player->m_vehicleGlow) : std::string("n/a");
        auto const partWas = player ? describeRobot(partSpriteOf(player, mode)) : std::string("n/a");
        auto const customWas = player && player->m_hasCustomGlowColor
            ? colorToString(player->m_glowColor) : std::string("off");

        log::info("Per-Gamemode Glow: [debug] {} color={} alpha={} blend={} preset={} rgb={} "
                  "hue={:+d} brightness*{:.2f} | customGlow was {} | iconGlow was {} | "
                  "vehicleGlow was {} | robot:{}",
            kModeKeys[index], colorToString(color), alpha, additive ? "additive" : "normal",
            m_activePreset < 0 ? "none" : m_presets[m_activePreset].name,
            m_rgbOn ? "on" : "off",
            m_hueShift[index], m_brightnessMul[index], customWas, iconWas, vehicleWas, partWas);
    }

    void writeDefaultConfig() const {
        auto root = matjson::Value::object();

        root.set("debug", false);
        root.set("fade", 0.4);
        root.set("preset", std::string(""));

        auto hotkeys = matjson::Value::object();
        hotkeys.set("enabled", true);
        hotkeys.set("reload", std::string("F5"));
        hotkeys.set("preset-next", std::string("F6"));
        hotkeys.set("preset-prev", std::string("F7"));
        hotkeys.set("hue-down", std::string("F8"));
        hotkeys.set("hue-up", std::string("F9"));
        hotkeys.set("brightness-down", std::string("F10"));
        hotkeys.set("brightness-up", std::string("F11"));
        hotkeys.set("rgb-toggle", std::string("F12"));
        hotkeys.set("rgb-slower", std::string("PageUp"));
        hotkeys.set("rgb-faster", std::string("PageDown"));
        hotkeys.set("open-menu", std::string("F4"));
        root.set("hotkeys", std::move(hotkeys));

        // Empty by default; the README shows what a preset looks like.
        root.set("presets", matjson::Value::array());

        auto rgb = matjson::Value::object();
        rgb.set("enabled", false);
        rgb.set("speed", 0.02);
        rgb.set("saturation", 1.0);
        rgb.set("brightness", 1.0);
        rgb.set("blending", std::string("normal"));
        root.set("rgb", std::move(rgb));

        for (std::size_t i = 0; i < kModeCount; ++i) {
            auto entry = matjson::Value::object();
            entry.set("enabled", false);
            entry.set("color", std::string("#FFFFFF"));
            entry.set("brightness", 1.0);
            entry.set("blending", std::string("normal"));
            entry.set("pulse", 0.0);
            root.set(std::string(kModeKeys[i]), std::move(entry));
        }

        auto written = utils::file::writeStringSafe(m_configPath, root.dump(4));
        if (written) {
            log::info("Per-Gamemode Glow: wrote default config to {}",
                utils::string::pathToString(m_configPath));
        } else {
            log::error("Per-Gamemode Glow: could not write default config: {}", written.unwrapErr());
        }
    }

    std::array<Profile, kModeCount> m_profiles {};
    std::vector<Preset> m_presets;
    int m_activePreset = -1;

    std::array<cocos2d::enumKeyCodes, kHotkeyCount> m_hotkeys = [] {
        std::array<cocos2d::enumKeyCodes, kHotkeyCount> keys {};
        for (std::size_t i = 0; i < kHotkeyCount; ++i) keys[i] = kDefaultHotkeys[i];
        return keys;
    }();
    bool m_hotkeysEnabled = true;
    /// Which hotkey keys are currently held down, and since when: used to drop a duplicate
    /// key-down for one press and to recover if a key-up never arrives.
    std::array<bool, kHotkeyCount> m_hotkeyHeld {};
    std::array<std::chrono::steady_clock::time_point, kHotkeyCount> m_hotkeyDownTime {};

    std::array<int, kModeCount>   m_hueShift {};
    std::array<float, kModeCount> m_brightnessMul = [] {
        std::array<float, kModeCount> values {};
        for (auto& value : values) value = 1.0f;
        return values;
    }();

    Mode m_lastMode = Mode::Count;
    bool m_debug = false;

    RgbSettings m_rgb;
    /// Runtime state of the RGB mode: starts at whatever the config says and is flipped by the
    /// rgb-toggle hotkey (never written back to the file).
    bool m_rgbOn = false;

    /// Colour smoothing state (one per mode) and its time constant in seconds.
    std::array<FadeState, kModeCount> m_fade {};
    float m_fadeSeconds = kDefaultFadeSeconds;

    /// Last scale this mod wrote for each glow sprite (see applyGlowSprite).
    std::unordered_map<cocos2d::CCSprite*, GlowScaleState> m_glowScale;

    fs::path m_configPath;
    matjson::Value m_rawConfig;
    fs::file_time_type m_configWriteTime {};
    std::chrono::steady_clock::time_point m_lastReloadCheck {};
    std::chrono::steady_clock::time_point m_lastDebugLog {};
    Mode m_lastDebugMode = Mode::Count;
};

// ---------------------------------------------------------------------------------------
// Settings popup
//
// One window with a tab per gamemode, a Rainbow tab and a Global tab. It edits Manager's
// live state (so every change is visible on the next frame, without restarting the level),
// and writes the JSON on Save or when the window is closed with unsaved changes. Hotkeys,
// presets and hand-written keys in the config are left untouched (see saveConfig()).
// ---------------------------------------------------------------------------------------

class GlowPopup : public geode::Popup {
public:
    static GlowPopup* create() {
        auto ret = new GlowPopup();
        if (ret && ret->init(450.f, 300.f)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

protected:
    static constexpr int kTabCount = static_cast<int>(kModeCount) + 3;
    static constexpr int kRgbTab = static_cast<int>(kModeCount);
    static constexpr int kGlobalTab = kRgbTab + 1;
    static constexpr int kPresetTab = kGlobalTab + 1;

    static constexpr char const* kTabNames[kTabCount] = {
        "Cube", "Ship", "Ball", "UFO", "Wave", "Robot", "Spider", "Swing",
        "Rainbow", "Global", "Presets",
    };

    bool init(float width, float height) {
        if (!Popup::init(width, height, "GJ_square02.png")) return false;

        this->setID("pgg-glow-popup");
        this->setTitle("Per-Gamemode Glow", "goldFont.fnt", 0.65f);

        // Popup::init gives m_mainLayer the popup's content size, so child coordinates are
        // measured from its bottom-left corner. Everything below is laid out around (0, 0),
        // so it all goes into a node placed at the centre of the popup.
        auto content = CCNode::create();
        content->setPosition({ width / 2.f, height / 2.f });
        m_mainLayer->addChild(content, 1);

        // A darker inner panel on each side separates the sidebar and the controls from the
        // popup background. NineSlice keeps the GD border corners crisp at any size.
        auto sidePanel = NineSlice::create("GJ_square02.png", {}, { 10.f, 10.f, 10.f, 10.f });
        if (sidePanel) {
            sidePanel->setContentSize({ 104.f, 276.f });
            sidePanel->setPosition({ -168.f, -2.f });
            sidePanel->setColor({ 70, 90, 150 });
            sidePanel->setOpacity(190);
            content->addChild(sidePanel, -1);
        }

        auto mainPanel = NineSlice::create("GJ_square02.png", {}, { 10.f, 10.f, 10.f, 10.f });
        if (mainPanel) {
            mainPanel->setContentSize({ 300.f, 288.f });
            mainPanel->setPosition({ 45.f, -3.f });
            mainPanel->setColor({ 70, 90, 150 });
            mainPanel->setOpacity(170);
            content->addChild(mainPanel, -1);
        }

        // Thin gold rule under the title, spanning the main panel only.
        auto accent = CCSprite::createWithSpriteFrameName("whiteSquare60_001.png");
        if (accent) {
            accent->setScaleX(300.f / accent->getContentSize().width);
            accent->setScaleY(2.f / accent->getContentSize().height);
            accent->setPosition({ 45.f, 118.f });
            accent->setColor({ 255, 205, 80 });
            accent->setOpacity(200);
            content->addChild(accent, -1);
        }

        // Geode's default close button sits half outside the corner (it is anchored at the
        // popup's top-left with a 3px inset). Hide it and put a normal-sized one inside the
        // top-right corner, where a GD player expects it.
        if (m_closeBtn) {
            m_closeBtn->setVisible(false);

            auto closeMenu = CCMenu::create();
            closeMenu->setPosition({ 0.f, 0.f });
            m_mainLayer->addChild(closeMenu, 10);

            auto closeBtn = CCMenuItemSpriteExtra::create(
                CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png"),
                this, menu_selector(GlowPopup::onClose));
            closeBtn->setScale(0.7f);
            closeBtn->setPosition({ width - 18.f, height - 18.f });
            closeMenu->addChild(closeBtn);
        }

        // ---- tab sidebar ---------------------------------------------------------------
        auto tabMenu = CCMenu::create();
        tabMenu->setPosition({ 0.f, 0.f });
        content->addChild(tabMenu, 3);

        for (int i = 0; i < kTabCount; ++i) {
            auto spr = ButtonSprite::create(
                kTabNames[i], 80, true, "bigFont.fnt", "GJ_button_01.png", 19.f, 0.34f);
            if (!spr) continue;

            auto item = CCMenuItemSpriteExtra::create(spr, this, menu_selector(GlowPopup::onTab));
            item->setTag(i);
            // Every tab is the same fixed size and sits fully inside the sidebar panel.
            item->setPosition({ -168.f, 104.f - 21.5f * static_cast<float>(i) });
            tabMenu->addChild(item);
            m_tabSprites[i] = spr;
        }

        // ---- mode panel ----------------------------------------------------------------
        m_modePanel = CCNode::create();
        m_modePanel->setPosition({ 0.f, 0.f });
        content->addChild(m_modePanel, 2);

        auto modeMenu = CCMenu::create();
        modeMenu->setPosition({ 0.f, 0.f });
        m_modePanel->addChild(modeMenu, 2);

        m_modeTitle = makeLabel(m_modePanel, "Cube", { 45.f, 104.f }, 0.65f, "goldFont.fnt");
        m_modeEnableSpr = addButton(modeMenu, "Enabled: OFF", { 45.f, 74.f },
            menu_selector(GlowPopup::onModeEnabled));

        // Label + colour chip are one compact row, centred on the panel. The chip has a
        // NineSlice border so it reads as a button, not a loose square.
        m_modeColorLabel = makeLabel(m_modePanel, "Colour", { 24.f, 38.f }, 0.5f);

        auto chip = CCNode::create();
        chip->setContentSize({ 36.f, 36.f });

        auto chipBorder = NineSlice::create("GJ_square02.png", {}, { 10.f, 10.f, 10.f, 10.f });
        if (chipBorder) {
            chipBorder->setContentSize({ 36.f, 36.f });
            chipBorder->setPosition({ 18.f, 18.f });
            chipBorder->setColor({ 255, 255, 255 });
            chipBorder->setOpacity(255);
            chip->addChild(chipBorder);
        }

        m_modeColorSpr = CCSprite::createWithSpriteFrameName("whiteSquare60_001.png");
        if (!m_modeColorSpr) m_modeColorSpr = CCSprite::create("GJ_square01.png");
        if (m_modeColorSpr) {
            m_modeColorSpr->setScale(0.47f);
            m_modeColorSpr->setPosition({ 18.f, 18.f });
            chip->addChild(m_modeColorSpr, 1);

            auto colorItem = CCMenuItemSpriteExtra::create(
                chip, this, menu_selector(GlowPopup::onModeColor));
            colorItem->setPosition({ 80.f, 38.f });
            modeMenu->addChild(colorItem);
        }

        m_modeBrightLabel = makeLabel(m_modePanel, "Brightness 1.00", { 45.f, 8.f }, 0.45f);
        m_modeBrightSlider = addSlider(m_modePanel, { 45.f, -10.f }, 0.f, 2.f, 1.f,
            [this](SliderNode*, float value) {
                if (m_syncing || m_tab >= static_cast<int>(kModeCount)) return;
                Manager::get().setProfileBrightness(static_cast<std::size_t>(m_tab), value);
                m_dirty = true;
                refreshModeLabels();
            });

        m_modePulseLabel = makeLabel(m_modePanel, "Pulse: off", { 45.f, -38.f }, 0.45f);
        m_modePulseSlider = addSlider(m_modePanel, { 45.f, -56.f }, 0.f, 5.f, 0.f,
            [this](SliderNode*, float value) {
                if (m_syncing || m_tab >= static_cast<int>(kModeCount)) return;
                Manager::get().setProfilePulse(static_cast<std::size_t>(m_tab), value);
                m_dirty = true;
                refreshModeLabels();
            });

        m_modeBlendSpr = addButton(modeMenu, "Blending: Normal", { 45.f, -84.f },
            menu_selector(GlowPopup::onModeBlending));

        // ---- rainbow panel -------------------------------------------------------------
        m_rgbPanel = CCNode::create();
        m_rgbPanel->setPosition({ 0.f, 0.f });
        content->addChild(m_rgbPanel, 2);

        auto rgbMenu = CCMenu::create();
        rgbMenu->setPosition({ 0.f, 0.f });
        m_rgbPanel->addChild(rgbMenu, 2);

        makeLabel(m_rgbPanel, "Rainbow", { 45.f, 100.f }, 0.65f, "goldFont.fnt");
        m_rgbEnableSpr = addButton(rgbMenu, "Rainbow: OFF", { 45.f, 72.f },
            menu_selector(GlowPopup::onRgbEnabled));

        m_rgbSpeedLabel = makeLabel(m_rgbPanel, "Speed 0.020 (50 s per turn)", { 45.f, 46.f }, 0.45f);
        m_rgbSpeedSlider = addSlider(m_rgbPanel, { 45.f, 28.f }, 0.f, 0.5f, 0.02f,
            [this](SliderNode*, float value) {
                if (m_syncing) return;
                Manager::get().setRgbSpeed(value);
                m_dirty = true;
                refreshRgbLabels();
            });

        m_rgbSatLabel = makeLabel(m_rgbPanel, "Saturation 1.00", { 45.f, 6.f }, 0.45f);
        m_rgbSatSlider = addSlider(m_rgbPanel, { 45.f, -12.f }, 0.f, 1.f, 1.f,
            [this](SliderNode*, float value) {
                if (m_syncing) return;
                Manager::get().setRgbSaturation(value);
                m_dirty = true;
                refreshRgbLabels();
            });

        m_rgbBrightLabel = makeLabel(m_rgbPanel, "Brightness 1.00", { 45.f, -32.f }, 0.45f);
        m_rgbBrightSlider = addSlider(m_rgbPanel, { 45.f, -50.f }, 0.f, 2.f, 1.f,
            [this](SliderNode*, float value) {
                if (m_syncing) return;
                Manager::get().setRgbBrightness(value);
                m_dirty = true;
                refreshRgbLabels();
            });

        m_rgbBlendSpr = addButton(rgbMenu, "Blending: Normal", { 45.f, -76.f },
            menu_selector(GlowPopup::onRgbBlending));

        makeLabel(m_rgbPanel, "Overrides every gamemode while on.",
            { 45.f, -94.f }, 0.3f, nullptr, false, 300.f);

        // ---- global panel --------------------------------------------------------------
        m_globalPanel = CCNode::create();
        m_globalPanel->setPosition({ 0.f, 0.f });
        content->addChild(m_globalPanel, 2);

        auto globalMenu = CCMenu::create();
        globalMenu->setPosition({ 0.f, 0.f });
        m_globalPanel->addChild(globalMenu, 2);

        makeLabel(m_globalPanel, "Global", { 45.f, 100.f }, 0.65f, "goldFont.fnt");
        m_modEnabledSpr = addButton(globalMenu, "Mod: ON", { 45.f, 72.f },
            menu_selector(GlowPopup::onModEnabled));
        m_respectGlowSpr = addButton(globalMenu, "Respect glow: ON", { 45.f, 44.f },
            menu_selector(GlowPopup::onRespectGlow));

        m_fadeLabel = makeLabel(m_globalPanel, "Fade 0.40 s", { 45.f, 16.f }, 0.45f);
        m_fadeSlider = addSlider(m_globalPanel, { 45.f, -2.f }, 0.f, 3.f, 0.4f,
            [this](SliderNode*, float value) {
                if (m_syncing) return;
                Manager::get().setFadeSeconds(value);
                m_dirty = true;
                refreshGlobalLabels();
            });

        m_debugSpr = addButton(globalMenu, "Debug log: OFF", { 45.f, -30.f },
            menu_selector(GlowPopup::onDebug));

        m_pathLabel = makeLabel(m_globalPanel, "", { -105.f, -60.f }, 0.24f, nullptr, true, 290.f);
        if (m_pathLabel) {
            m_pathLabel->setWidth(290.f);
            m_pathLabel->setAlignment(kCCTextAlignmentLeft);
            m_pathLabel->setColor({ 205, 210, 225 });
        }
        makeLabel(m_globalPanel, "Robot/Spider glow uses the icon-kit colour.",
            { 45.f, -90.f }, 0.3f, nullptr, false, 300.f);

        // ---- presets panel -------------------------------------------------------------
        m_presetPanel = CCNode::create();
        m_presetPanel->setPosition({ 0.f, 0.f });
        content->addChild(m_presetPanel, 2);

        auto presetMenu = CCMenu::create();
        presetMenu->setPosition({ 0.f, 0.f });
        m_presetPanel->addChild(presetMenu, 2);

        makeLabel(m_presetPanel, "Presets", { 45.f, 100.f }, 0.65f, "goldFont.fnt");
        m_presetCurrentLabel = makeLabel(m_presetPanel, "Current: Custom colours",
            { 45.f, 72.f }, 0.45f, nullptr, false, 300.f);

        m_presetPrevSpr = addButton(presetMenu, "< Prev", { -28.f, 44.f },
            menu_selector(GlowPopup::onPresetPrev));
        m_presetNextSpr = addButton(presetMenu, "Next >", { 118.f, 44.f },
            menu_selector(GlowPopup::onPresetNext));
        m_presetCustomSpr = addButton(presetMenu, "Use custom colours", { 45.f, 16.f },
            menu_selector(GlowPopup::onPresetCustom));

        m_presetNameInput = TextInput::create(190.f, "New preset name");
        if (m_presetNameInput) {
            m_presetNameInput->setScale(0.7f);
            m_presetNameInput->setPosition({ 45.f, -14.f });
            m_presetNameInput->setMaxCharCount(40);
            m_presetNameInput->setCommonFilter(CommonFilter::Name);
            m_presetPanel->addChild(m_presetNameInput);
        }

        m_presetSaveSpr = addButton(presetMenu, "Save preset", { 118.f, -46.f },
            menu_selector(GlowPopup::onPresetSave));
        m_presetDeleteSpr = addButton(presetMenu, "Delete", { -28.f, -46.f },
            menu_selector(GlowPopup::onPresetDelete));

        m_presetInfo = makeLabel(m_presetPanel, "", { 45.f, -82.f }, 0.3f, nullptr, false, 300.f);

        // ---- footer (shared by every tab) ----------------------------------------------
        auto footer = CCMenu::create();
        footer->setPosition({ 0.f, 0.f });
        content->addChild(footer, 4);

        // Both footer buttons share the panel's centre line (x = 45), and sit high enough
        // that their bottom edge never touches the popup border.
        m_saveSpr = addButton(footer, "Save", { 118.f, -118.f }, menu_selector(GlowPopup::onSave));
        addButton(footer, "Reload", { -28.f, -118.f }, menu_selector(GlowPopup::onReload));

        m_status = makeLabel(content, "", { 45.f, -140.f }, 0.28f);
        if (m_status) m_status->setColor({ 160, 255, 160 });

        m_tab = 0;
        refresh();
        return true;
    }

    // ---------------------------------------------------------------------------------
    // Layout helpers
    // ---------------------------------------------------------------------------------

    static CCLabelBMFont* makeLabel(CCNode* parent, char const* text, CCPoint position,
                                    float scale, char const* font = nullptr,
                                    bool leftAligned = false, float width = 0.f) {
        auto label = CCLabelBMFont::create(text, font ? font : "bigFont.fnt");
        if (!label) return nullptr;

        label->setScale(scale);
        label->setPosition(position);
        if (width > 0.f) {
            label->setWidth(width);
            label->setAlignment(leftAligned ? kCCTextAlignmentLeft : kCCTextAlignmentCenter);
        }
        if (leftAligned) label->setAnchorPoint({ 0.f, 0.5f });
        parent->addChild(label);
        return label;
    }

    ButtonSprite* addButton(CCMenu* menu, char const* text, CCPoint position,
                            SEL_MenuHandler selector) {
        auto spr = ButtonSprite::create(
            text, 126, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.38f);
        if (!spr) return nullptr;

        auto item = CCMenuItemSpriteExtra::create(spr, this, selector);
        item->setPosition(position);
        menu->addChild(item);
        return spr;
    }

    SliderNode* addSlider(CCNode* parent, CCPoint position, float min, float max,
                          float value, SliderNode::SliderCallback callback) {
        auto slider = SliderNode::create(std::move(callback));
        if (!slider) return nullptr;

        slider->setMin(min);
        slider->setMax(max);
        slider->setValue(value);
        slider->setScale(0.75f);
        slider->setPosition(position);
        parent->addChild(slider);
        return slider;
    }

    // ---------------------------------------------------------------------------------
    // Refresh
    // ---------------------------------------------------------------------------------

    void refresh() {
        m_syncing = true;

        for (int i = 0; i < kTabCount; ++i) {
            auto spr = m_tabSprites[i];
            if (!spr || !spr->m_BGSprite) continue;

            if (i == m_tab) {
                spr->m_BGSprite->setColor({ 110, 255, 140 });
            } else if (i < static_cast<int>(kModeCount)) {
                spr->m_BGSprite->setColor(Manager::get().profileView(static_cast<std::size_t>(i)).color);
            } else {
                spr->m_BGSprite->setColor({ 255, 255, 255 });
            }
        }

        m_modePanel->setVisible(m_tab < static_cast<int>(kModeCount));
        m_rgbPanel->setVisible(m_tab == kRgbTab);
        m_globalPanel->setVisible(m_tab == kGlobalTab);
        m_presetPanel->setVisible(m_tab == kPresetTab);

        if (m_tab < static_cast<int>(kModeCount)) refreshMode();
        if (m_tab == kRgbTab) refreshRgb();
        if (m_tab == kGlobalTab) refreshGlobal();
        if (m_tab == kPresetTab) refreshPresets();

        if (m_pathLabel) m_pathLabel->setString(Manager::get().configPathString().c_str());

        m_syncing = false;
    }

    void refreshMode() {
        auto const index = static_cast<std::size_t>(m_tab);
        auto const profile = Manager::get().profileView(index);

        if (m_modeTitle) m_modeTitle->setString(std::string(kModeKeys[index]).c_str());
        if (m_modeEnableSpr) m_modeEnableSpr->setString(profile.enabled ? "Enabled: ON" : "Enabled: OFF");
        if (m_modeColorSpr) m_modeColorSpr->setColor(profile.color);
        if (m_modeBlendSpr) m_modeBlendSpr->setString(profile.additive ? "Blending: Additive" : "Blending: Normal");

        if (m_modeBrightSlider) m_modeBrightSlider->setValue(profile.brightness);
        if (m_modePulseSlider) m_modePulseSlider->setValue(profile.pulse);

        refreshModeLabels();
    }

    void refreshModeLabels() {
        if (m_tab >= static_cast<int>(kModeCount)) return;
        auto const profile = Manager::get().profileView(static_cast<std::size_t>(m_tab));

        char buffer[64];
        if (m_modeBrightLabel) {
            std::snprintf(buffer, sizeof(buffer), "Brightness %.2f", profile.brightness);
            m_modeBrightLabel->setString(buffer);
        }
        if (m_modePulseLabel) {
            if (profile.pulse <= 0.001f) {
                std::snprintf(buffer, sizeof(buffer), "Pulse: off");
            } else {
                std::snprintf(buffer, sizeof(buffer), "Pulse: %.2f s", profile.pulse);
            }
            m_modePulseLabel->setString(buffer);
        }
    }

    void refreshRgb() {
        auto const& rgb = Manager::get().rgb();

        if (m_rgbEnableSpr) m_rgbEnableSpr->setString(Manager::get().rgbOn() ? "Rainbow: ON" : "Rainbow: OFF");
        if (m_rgbBlendSpr) m_rgbBlendSpr->setString(rgb.additive ? "Blending: Additive" : "Blending: Normal");
        if (m_rgbSpeedSlider) m_rgbSpeedSlider->setValue(rgb.speed);
        if (m_rgbSatSlider) m_rgbSatSlider->setValue(rgb.saturation);
        if (m_rgbBrightSlider) m_rgbBrightSlider->setValue(rgb.brightness);

        refreshRgbLabels();
    }

    void refreshRgbLabels() {
        auto const& rgb = Manager::get().rgb();

        char buffer[80];
        if (m_rgbSpeedLabel) {
            auto const seconds = rgb.speed > 0.0001f ? 1.f / rgb.speed : 0.f;
            std::snprintf(buffer, sizeof(buffer), "Speed %.3f (%.0f s per turn)", rgb.speed, seconds);
            m_rgbSpeedLabel->setString(buffer);
        }
        if (m_rgbSatLabel) {
            std::snprintf(buffer, sizeof(buffer), "Saturation %.2f", rgb.saturation);
            m_rgbSatLabel->setString(buffer);
        }
        if (m_rgbBrightLabel) {
            std::snprintf(buffer, sizeof(buffer), "Brightness %.2f", rgb.brightness);
            m_rgbBrightLabel->setString(buffer);
        }
    }

    void refreshGlobal() {
        if (m_modEnabledSpr) {
            m_modEnabledSpr->setString(Mod::get()->getSettingValue<bool>("enabled") ? "Mod: ON" : "Mod: OFF");
        }
        if (m_respectGlowSpr) {
            m_respectGlowSpr->setString(Mod::get()->getSettingValue<bool>("respect-global-toggle")
                ? "Respect glow: ON" : "Respect glow: OFF");
        }
        if (m_debugSpr) {
            m_debugSpr->setString(Manager::get().debug() ? "Debug log: ON" : "Debug log: OFF");
        }

        if (m_fadeSlider) m_fadeSlider->setValue(Manager::get().fadeSeconds());
        refreshGlobalLabels();
    }

    void refreshGlobalLabels() {
        char buffer[48];
        if (m_fadeLabel) {
            std::snprintf(buffer, sizeof(buffer), "Fade %.2f s", Manager::get().fadeSeconds());
            m_fadeLabel->setString(buffer);
        }
    }

    void refreshPresets() {
        auto const count = Manager::get().presetCount();
        auto const active = Manager::get().activePreset();

        char buffer[160];
        if (active >= 0) {
            std::snprintf(buffer, sizeof(buffer), "Current: %s  (%d/%d)",
                Manager::get().presetName(active).c_str(), active + 1, count);
        } else if (count == 0) {
            std::snprintf(buffer, sizeof(buffer), "Current: Custom colours  (no presets yet)");
        } else {
            std::snprintf(buffer, sizeof(buffer), "Current: Custom colours  (%d preset%s available)",
                count, count == 1 ? "" : "s");
        }
        if (m_presetCurrentLabel) m_presetCurrentLabel->setString(buffer);

        if (m_presetInfo) {
            m_presetInfo->setString(active >= 0
                ? "Colour edits update this preset. Save preset writes the JSON."
                : "Type a name, then Save preset to store the current colours.");
        }
    }

    /// Puts the active preset's name into the text field (or a free "Preset N"). Called when
    /// entering the tab and after actions, never while the user is typing.
    void syncPresetInput() {
        if (!m_presetNameInput) return;
        auto const active = Manager::get().activePreset();
        m_presetNameInput->setString(active >= 0
            ? Manager::get().presetName(active)
            : Manager::get().suggestPresetName());
    }

    // ---------------------------------------------------------------------------------
    // Callbacks
    // ---------------------------------------------------------------------------------

    void onTab(CCObject* sender) {
        int tab = sender ? sender->getTag() : 0;
        if (tab < 0 || tab >= kTabCount) return;
        bool const enteringPresets = tab == kPresetTab && m_tab != kPresetTab;
        m_tab = tab;
        if (enteringPresets) syncPresetInput();
        refresh();
    }

    void onModeEnabled(CCObject*) {
        if (m_tab >= static_cast<int>(kModeCount)) return;
        auto const index = static_cast<std::size_t>(m_tab);
        Manager::get().setProfileEnabled(index, !Manager::get().profileView(index).enabled);
        m_dirty = true;
        refresh();
    }

    void onModeColor(CCObject*) {
        if (m_tab >= static_cast<int>(kModeCount)) return;
        auto const index = static_cast<std::size_t>(m_tab);

        auto picker = geode::ColorPickPopup::create(Manager::get().profileView(index).color);
        if (!picker) return;

        picker->setCallback([this, index](ccColor4B const& color) {
            Manager::get().setProfileColor(index, { color.r, color.g, color.b });
            m_dirty = true;
            this->refresh();
        });
        picker->show();
    }

    void onModeBlending(CCObject*) {
        if (m_tab >= static_cast<int>(kModeCount)) return;
        auto const index = static_cast<std::size_t>(m_tab);
        Manager::get().setProfileAdditive(index, !Manager::get().profileView(index).additive);
        m_dirty = true;
        refresh();
    }

    void onRgbEnabled(CCObject*) {
        Manager::get().setRgbOn(!Manager::get().rgbOn());
        m_dirty = true;
        refresh();
    }

    void onRgbBlending(CCObject*) {
        Manager::get().setRgbAdditive(!Manager::get().rgb().additive);
        m_dirty = true;
        refresh();
    }

    void onModEnabled(CCObject*) {
        bool value = !Mod::get()->getSettingValue<bool>("enabled");
        Mod::get()->setSettingValue<bool>("enabled", value);
        setStatus(value ? "Mod enabled" : "Mod disabled");
        refresh();
    }

    void onRespectGlow(CCObject*) {
        bool value = !Mod::get()->getSettingValue<bool>("respect-global-toggle");
        Mod::get()->setSettingValue<bool>("respect-global-toggle", value);
        setStatus(value ? "Following the icon kit's Glow switch" : "Ignoring the icon kit's Glow switch");
        refresh();
    }

    void onDebug(CCObject*) {
        Manager::get().setDebug(!Manager::get().debug());
        m_dirty = true;
        refresh();
    }

    void onSave(CCObject*) {
        if (Manager::get().saveConfig()) {
            m_dirty = false;
            setStatus("Saved to per_gamemode_glow.json");
        } else {
            setStatus("Could not save the config (see log)");
            if (m_status) m_status->setColor({ 255, 130, 130 });
        }
    }

    void onReload(CCObject*) {
        Manager::get().reloadConfig();
        m_dirty = false;
        setStatus("Reloaded from disk");
        refresh();
    }

    void onPresetPrev(CCObject*) {
        Manager::get().stepPreset(-1);
        m_dirty = true;
        syncPresetInput();
        refresh();
    }

    void onPresetNext(CCObject*) {
        Manager::get().stepPreset(+1);
        m_dirty = true;
        syncPresetInput();
        refresh();
    }

    void onPresetCustom(CCObject*) {
        Manager::get().setActivePreset(-1);
        m_dirty = true;
        syncPresetInput();
        refresh();
    }

    void onPresetSave(CCObject*) {
        std::string name;
        if (m_presetNameInput) name = m_presetNameInput->getString().c_str();

        auto const saved = Manager::get().savePreset(name);
        m_dirty = true;
        if (m_presetNameInput) m_presetNameInput->setString(saved);

        if (Manager::get().saveConfig()) {
            m_dirty = false;
            setStatus(("Preset '" + saved + "' saved").c_str());
        } else {
            setStatus("Preset added, but the config could not be written");
        }
        refresh();
    }

    void onPresetDelete(CCObject*) {
        if (!Manager::get().deleteActivePreset()) {
            setStatus("Select a preset first");
            return;
        }

        m_dirty = true;
        if (Manager::get().saveConfig()) {
            m_dirty = false;
            setStatus("Preset deleted");
        } else {
            setStatus("Preset deleted from memory; the config could not be written");
        }
        syncPresetInput();
        refresh();
    }

    void onClose(CCObject* sender) override {
        // Closing the window is the natural "I'm done" action, so unsaved slider/colour
        // changes are written instead of silently dropped.
        if (m_dirty && Manager::get().saveConfig()) m_dirty = false;
        Popup::onClose(sender);
    }

    void setStatus(char const* text) {
        if (!m_status) return;
        m_status->setColor({ 160, 255, 160 });
        m_status->setString(text);
        this->unschedule(schedule_selector(GlowPopup::clearStatus));
        this->scheduleOnce(schedule_selector(GlowPopup::clearStatus), 2.0f);
    }

    void clearStatus(float) {
        if (m_status) m_status->setString("");
    }

    // ---------------------------------------------------------------------------------
    // State
    // ---------------------------------------------------------------------------------

    int m_tab = 0;
    bool m_dirty = false;
    bool m_syncing = false;

    std::array<ButtonSprite*, kTabCount> m_tabSprites {};

    CCNode* m_modePanel = nullptr;
    CCNode* m_rgbPanel = nullptr;
    CCNode* m_globalPanel = nullptr;
    CCNode* m_presetPanel = nullptr;

    CCLabelBMFont* m_modeTitle = nullptr;
    CCLabelBMFont* m_modeColorLabel = nullptr;
    CCLabelBMFont* m_modeBrightLabel = nullptr;
    CCLabelBMFont* m_modePulseLabel = nullptr;

    CCSprite* m_modeColorSpr = nullptr;
    ButtonSprite* m_modeEnableSpr = nullptr;
    ButtonSprite* m_modeBlendSpr = nullptr;
    SliderNode* m_modeBrightSlider = nullptr;
    SliderNode* m_modePulseSlider = nullptr;

    CCLabelBMFont* m_rgbSpeedLabel = nullptr;
    CCLabelBMFont* m_rgbSatLabel = nullptr;
    CCLabelBMFont* m_rgbBrightLabel = nullptr;
    ButtonSprite* m_rgbEnableSpr = nullptr;
    ButtonSprite* m_rgbBlendSpr = nullptr;
    SliderNode* m_rgbSpeedSlider = nullptr;
    SliderNode* m_rgbSatSlider = nullptr;
    SliderNode* m_rgbBrightSlider = nullptr;

    ButtonSprite* m_modEnabledSpr = nullptr;
    ButtonSprite* m_respectGlowSpr = nullptr;
    ButtonSprite* m_debugSpr = nullptr;
    CCLabelBMFont* m_fadeLabel = nullptr;
    CCLabelBMFont* m_pathLabel = nullptr;
    SliderNode* m_fadeSlider = nullptr;

    CCLabelBMFont* m_presetCurrentLabel = nullptr;
    CCLabelBMFont* m_presetInfo = nullptr;
    ButtonSprite* m_presetPrevSpr = nullptr;
    ButtonSprite* m_presetNextSpr = nullptr;
    ButtonSprite* m_presetCustomSpr = nullptr;
    ButtonSprite* m_presetSaveSpr = nullptr;
    ButtonSprite* m_presetDeleteSpr = nullptr;
    TextInput* m_presetNameInput = nullptr;

    ButtonSprite* m_saveSpr = nullptr;
    CCLabelBMFont* m_status = nullptr;
};

void openGui() {
    // F4 is a toggle: never stack two copies of the window.
    if (auto scene = CCDirector::get()->getRunningScene()) {
        if (scene->getChildByIDRecursive("pgg-glow-popup")) return;
    }

    if (auto popup = GlowPopup::create()) {
        popup->show();
    } else {
        log::error("Per-Gamemode Glow: could not create the settings popup");
    }
}

} // namespace pgg

// ---------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------

class $modify(PGGPlayerObject, PlayerObject) {
    struct Fields {
        float elapsed = 0.0f;
        // Did we set this player's custom glow colour? Kept per PlayerObject, so a second
        // player in a dual level (and every other PlayerObject) has its own state, and so a
        // profile turning off only clears the glow this mod itself took over.
        bool customGlowOwned = false;
    };

    void update(float dt) {
        PlayerObject::update(dt);

        m_fields->elapsed += dt;
        pgg::Manager::get().apply(this, m_fields->elapsed, m_fields->customGlowOwned);
    }

    void updatePlayerArt() {
        // Our colour goes in *before* the original: the game's own glow update runs inside it
        // and reads m_glowColor / m_hasCustomGlowColor, and that is the one place the robot
        // and spider limb glow gets its colour from. Re-applied after it too, so the sprites
        // are left with our values whatever the original did to them.
        pgg::Manager::get().apply(this, m_fields->elapsed, m_fields->customGlowOwned);

        PlayerObject::updatePlayerArt();

        // Re-applied immediately so a gamemode change never shows one frame of stale glow.
        pgg::Manager::get().apply(this, m_fields->elapsed, m_fields->customGlowOwned);
    }
};

/// Read-only keyboard hook for the config hotkeys. Key events are only observed; the
/// original implementation is always called, with the original result returned, so input
/// (including text fields) keeps working exactly as before.
class $modify(PGGKeyboardDispatcher, cocos2d::CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        pgg::Manager::get().onKeyEvent(key, isKeyDown, isKeyRepeat);
        return cocos2d::CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};

/// A small "GLOW" button on the pause screen, so the editor is one click away in a level.
class $modify(PGGPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto winSize = CCDirector::get()->getWinSize();

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        this->addChild(menu, 100);

        auto spr = ButtonSprite::create("GLOW", "bigFont.fnt", "GJ_button_01.png", 0.55f);
        auto button = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PGGPauseLayer::onGlow));
        button->setPosition({ winSize.width - 46.f, winSize.height * 0.5f });
        menu->addChild(button);
    }

    void onGlow(CCObject*) {
        pgg::openGui();
    }
};

$on_mod(Loaded) {
    pgg::Manager::get().loadConfig();
}
