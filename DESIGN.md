# Design system — Minimum Reborn panel

Declared before any UI code was written (R-37: no DESIGN.md existed). Based on the
product's own identity: the Minimum Reborn mark (violet/indigo) and the GD-native
panel language the mod already uses (dark rounded panel, thick light border,
game-native close button). Everything else was rebuilt.

Dials: ENERGY 2, RHYTHM 2, MOTION 1 (hover/press transitions only; no ambient motion
in front of gameplay; nothing loops).

## Color (2-3 cores + 1 accent, R-29)

| Token | Value | Use |
|---|---|---|
| `--base` | `#12141c` | window background |
| `--panel` | `#171a26` | panel body |
| `--surface` | `#1f2333` | raised controls, secondary buttons, unselected segments |
| `--hairline` | `#2b3046` | 1px separators |
| `--text` | `#eef1f8` | primary text |
| `--muted` | `#a6aec7` | secondary text (≥ 4.5:1 on `--panel`: 7.6:1) |
| `--accent` | `#8b5cf6` | active tab, selected profile, primary button, checkbox fill, focus ring |
| `--accent-contrast` | `#12141c` | text/icons on accent fills |

Success green (`#34d17b`) is reserved for the single "mod enabled" status; it appears
nowhere else. No other colors. No gradients.

## Type

One family. GD-style bold display face for the title only; clean sans (game `chatFont`
family) for everything else, two weights (regular 400 / semibold 600).

- title 26px / body 15px / secondary 12.5px — hard floor 12px everywhere.
- No script fonts, no serif italics, no wide-tracked caps labels. Sentence case.

## Space / radius

Spacing scale 4 / 8 / 12 / 16 / 24 / 32. Nothing off-scale. Panel width 420-540 logical
units (~480); identical layout at 1024x768 and 1920x1080 except outer margins. The panel
must never fill more than ~56% of the screen width at 1024x768; effective rendered
size (checked by screenshot) beats code constants every time.

Radii per role: panel 14, buttons 10, inputs 8, checkbox 4. One radius per role.

## Roles

Buttons: primary (accent fill, dark text — "Benchmark" in the pause panel), secondary
(`--surface`), ghost (transparent, muted — e.g. Reset). Every role: hover, pressed,
disabled (visible + non-interactive), focus-visible (accent 2px ring, 2px offset).
Hit target >= 40px (44 preferred).

Tabs: six equal measured columns inside panel padding; labels never clip
(`Overview Render Culling Input Editor Diag`); active = accent underline 2px + full
opacity; inactive = muted; arrow-key navigation.

Profile picker: one segmented control (Custom / Default / Balanced / Performance /
Extreme). Selected = accent fill + dark text + a check. Unselected = `--surface`.

Checkbox: 20px box, accent fill with dark check on dark surface; label 15px regular.

Header: logo left, "Minimum Reborn" + one sentence-case subtitle, single status pill
with the real profile (or nothing when not measurable), game-native close button.

Telemetry rows: label left, value right on a shared baseline, 1px hairline between
rows, every value real or the row hides. No micro italic columns.

## Copy

Sentence case everywhere. No ALL-CAPS labels. No buzzwords. Numbers only if real.
