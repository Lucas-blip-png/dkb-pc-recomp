# Accessibility

DKB Recomp runs the game's own code, so the game's text, menus, subtitles and difficulty modes (Normal and Assist)
are the game's. What the port adds sits around it: input, the picture, sound and the settings menu. This page lists
what you can adjust today, what is known not to work, and how to tell us about a barrier.

## What you can adjust

Most of these are in the settings menu (F1 or Esc; on a controller, the Guide button or Minus and Plus together).
Every one can also be set in `dkb.ini` next to `dkb.exe`; `dkb.ini.example` describes each key.

**Input** ([docs/controls.md](docs/controls.md))

- Play with a gamepad, with keyboard and mouse, or with both at once (their inputs are merged).
- Remap every button and stick direction to any pad button or key, in the menu's Controls tab or with `bind_A`,
  `bind_ZL`, `bind_LSTICK_UP`, ...
- Button layout: Nintendo or Xbox positions, and Xbox-style button prompts in the game (`pad_layout`).
- Stick dead zone for drifting sticks (`stick_deadzone`), mouse camera sensitivity or no mouse camera (`mouse_sens`).
- Motion controls on or off (`motion`); rumble on or off, its strength and curve (`rumble`, `rumble_strength`,
  `rumble_curve`).
- The game keeps running when its window loses focus, so switching to another tool does not pause it
  (`assume_focus`).

**Picture**

- Brightness, contrast, saturation and gamma, or the vivid and clarity profiles (`color_preset`, `color_*`).
- Window, borderless or fullscreen, and the output resolution (`window_mode`, `output_res`).
- Frame rate limit (`fps_limit`) and anti-aliasing of the final picture, HUD included (`smaa`).

**Sound**

- Volume on top of the game's own (`volume`), and stereo or 5.1 (`audio_channels`).

**Language**

- Game language (`lang`) and settings menu language, English, Russian or Chinese (`menu_lang`; Chinese follows the
  system language for Simplified and Traditional alike).

## Known limitations

- The settings menu has no screen reader support and no text size setting.
- There are no colour-blind filters; the colour profile only changes brightness, contrast, saturation and gamma.
- The game's flashing effects, camera motion and text size come from the game and cannot be changed by the port.
- Frame generation (`frame_gen`) is experimental and can add visual artefacts; it is off by default.

## Reporting a barrier

Open an issue and describe what stops you, what you use (controller, screen reader, other assistive tools) and your
`dkb.ini` if you changed it. Requests that the port can address go on the roadmap (#10).
