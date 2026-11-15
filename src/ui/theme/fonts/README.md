# The system UI's fonts

IBM Plex Sans (Regular, Bold) and IBM Plex Mono (Bold), the fonts of the
owner's design, as TrueType files from IBM's releases
(github.com/IBM/plex):

- `IBMPlexSans-Regular.ttf`, `IBMPlexSans-Bold.ttf`: `@ibm/plex-sans@1.1.0`,
  `fonts/complete/ttf`;
- `IBMPlexMono-Bold.ttf`: `@ibm/plex-mono@2.5.0`, `fonts/complete/ttf`.

They are under the SIL Open Font License 1.1 (`OFL.txt`). The build turns
them into 1-bit LVGL fonts in `build/ui` (`tools/mkfont.py`, the sizes in
the top-level `Makefile`).
