# Per-Gamemode Glow

Independent glow settings for every player gamemode in Geometry Dash: give the cube, ship,
ball, UFO, wave, robot, spider and swing their own colour, brightness, blend mode and pulse
instead of one global glow for all of them.

A built-in editor (**F4**, or the **GLOW** button on the pause screen) has a tab for every
gamemode plus Rainbow, Global and Presets tabs: pick a colour with the game's colour picker, drag
brightness, pulse and fade sliders, switch normal/additive blending, and everything applies live
in the level. The Presets tab can cycle, create, overwrite and delete named colour sets. Changes
are saved back to the JSON when you press Save or close the window.

The same settings can still be edited by hand in
`config/zam.per-gamemode-glow/per_gamemode_glow.json`; the file is re-read on the fly. Hotkeys
remain available for quick tweaks while playing (reload, presets, hue and brightness of the
current gamemode, and an RGB rainbow mode with its own speed control).

An RGB mode overlays a smooth rainbow on every gamemode at once — one key toggles it, two more
tune the speed while playing, and the log prints the exact value to keep.

Fully client-side: nothing is sent to the server and level progress is not affected.

Known limitation: Geometry Dash draws the robot's and the spider's own glow itself, from the icon
kit's glow colour, and ignores a per-player glow colour for those two modes -- so `robot` and
`spider` cannot take effect on this build. The mod says so in its log and in the editor (a yellow
note on those two tabs), and documents it in the README.

Built for **GD 2.2081** with Geode 5.11.2.

Made by zam, with the help of an AI assistant (Geode SDK).

See `README.md` for the config format, hotkeys, limitations and compatibility notes.
