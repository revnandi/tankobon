# PSP artwork templates

These SVG files use Tankobon's interface palette and can be edited in Figma, Inkscape, Illustrator, Affinity Designer, or a text editor.

| Template | Export filename | Export size | Purpose |
| :--- | :--- | :--- | :--- |
| `ICON0-template.svg` | `ICON0.PNG` | 144×80 | App icon shown in the XMB |
| `PIC0-template.svg` | `PIC0.PNG` | 310×180 | Optional foreground information panel |
| `PIC1-template.svg` | `PIC1.PNG` | 480×272 | Optional full-screen XMB background |

Export PNG files at the exact dimensions above with transparency enabled. Keep filenames uppercase and put them in this directory — the Makefile picks up each one on the next `make`, and leaves out whatever is missing.

## Editing notes

**One mark, three files.** All three share the same open-book group, marked `id="mark"` (or `id="volume-stack"` in PIC1). Change it in one file and paste it into the others so the icon and the XMB art stay the same drawing.

**Text needs Arial.** Copy is live `<text>` so it stays editable, which means the exporting tool has to have Arial or Helvetica. If yours does not, convert text to outlines before exporting or the metrics will shift.

**ICON0 is full bleed.** The XMB draws it as a plain rectangle, so nothing should depend on rounded corners or transparent margins.

**PIC1 keeps its left side empty.** The XMB draws its wave, clock, and menu column over the left and centre of the background. Artwork belongs in the right third.

**Mind the floor.** Nothing thinner than 2 px and no text under 10 px. These are displayed at native size on a 480×272 screen — a 1 px rule that looks fine zoomed in will disappear.

## Palette

Same eight colours as the interface:

| | | |
| :--- | :--- | :--- |
| `#1C1C1C` | `#2C2C2C` | `#4A4A4A` |
| `#8A8A8A` | `#D6D7D7` | `#F3F4F4` |
| `#612D53` | `#853953` | |
