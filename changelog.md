# 1.1.0
- Built for **GD 2.2081** with Geode 5.11.2 (native loader support, no forward-compat shims).
- **New in-game editor** (F4, or the GLOW button on the pause screen): one tab per gamemode plus Rainbow, Global and Presets tabs, the game's colour picker, brightness / pulse / fade sliders, normal/additive blending, and a config path + save status footer. Changes apply live and are saved to the JSON on Save or when the window is closed.
- **Presets in the editor**: Prev/Next, Use custom colours, name field, Save preset (creates or overwrites) and Delete. The active preset is written to the config on save.
- Brightness above 1.0 now does something: 0–1 changes opacity, 1–2 brightens the tint and slightly grows the glow instead of being clamped at full opacity.
- All editor buttons use fixed sizes: tabs stay fully inside the sidebar, and action/footer buttons are aligned and uniform.
- Footer buttons and the colour row are centred in the panel; the close button sits inside the popup.
- Global tab: master on/off, respect the icon kit's Glow switch, colour fade time and debug logging.
- Mod logo for the Geode mod card.

# 1.0.0
- Independent glow for every player gamemode (cube, ship, ball, UFO, wave, robot, spider, swing): colour, brightness, blend mode and optional pulse.
- RGB mode: a smooth rainbow across all gamemodes at once, toggled in game (default F12), with its own speed, saturation, brightness and blending. Speed and colour transitions are eased and tuned with hotkeys.
- JSON config with live reload — edits apply within ~half a second, no restart needed.
- Hotkeys: reload config, next/previous preset, hue / brightness of the current gamemode, RGB toggle and RGB speed, and easy rebinding of all of them in the config.
- Named colour presets for switching palettes.
- Optional debug logging for troubleshooting per-mode glow (including a full sprite dump for robot/spider).
- Known limitation: `robot` and `spider` cannot take effect -- the game draws those two modes' glow itself from the icon kit's glow colour (documented in the README, and the mod warns in the log if those profiles are enabled). The spider's eye does take the mod's colour, by request.
