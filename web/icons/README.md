# Optional web UI icon overrides

Drop PNG or SVG files here using any of these names, then rebuild the ELF:

- `logo`: header logo
- `store`: store tiles
- `library`: Installed games tile
- `download`: Download all and individual download buttons
- `downloads`, `settings`, `quit`, `add`: navigation/actions
- `tick`: installed-game marker
- `trash`: installed-file deletion

For example, use `store.png` or `store.svg`. PNG takes precedence if both exist.
Transparent square images work best. Keep each file below 2 MB.
Missing overrides use the existing inline SVG. All icons are embedded in the
ELF, so no separate files need to be copied to the console.
