<div align="center">

# Tankobon

A CBZ manga reader for the Sony PlayStation Portable.

![Platform](https://img.shields.io/badge/platform-PSP-003791?style=for-the-badge&logo=playstation&logoColor=white)
![Language](https://img.shields.io/badge/language-C-00599C?style=for-the-badge&logo=c&logoColor=white)
![Version](https://img.shields.io/badge/version-1.1.0-6f42c1?style=for-the-badge)

</div>

Tankobon reads `.cbz` files directly on a PSP. It scans your manga library, caches cover thumbnails on the Memory Stick, and saves your place in each volume.

The renderer writes a 480x272 framebuffer in software. It does not use the PSP GU or shaders.

## Features

- Reads stored and deflated CBZ entries without unpacking them to disk
- Fit-width, fit-screen, and rotated reading modes
- 1.25x or 1.75x magnification with analog panning
- Optional scan-margin cropping
- Manga right-to-left and western left-to-right page order
- Optional page slide transitions
- Three library layouts: cover grid, cover list, and text list
- Cover thumbnail caching with progress rails, percentages, and saved page details
- Analog momentum scrolling
- Automatic, black, or white page letterboxing
- 21 color themes adapted from open source palettes

## Install

1. Download `EBOOT.PBP` from [Releases](https://github.com/revnandi/tankobon/releases).
2. Create `ms0:/PSP/GAME/Tankobon/` on the Memory Stick.
3. Copy `EBOOT.PBP` into that directory.
4. Add a `mangas` directory beside it, with one subdirectory per series.
5. Launch Tankobon from the XMB. It scans the library on startup.

Your files should look like this:

```text
ms0:/PSP/GAME/Tankobon/
|-- EBOOT.PBP
|-- mangas/
|   |-- Berserk/
|   |   |-- cover.jpg
|   |   |-- Vol01.cbz
|   |   `-- Vol02.cbz
|   `-- Vinland Saga/
|       |-- cover.png
|       `-- Vol01.cbz
|-- bookmarks.dat        created after you start reading
|-- settings.dat         created after you change an option
`-- .thumbs/             created during the first scan
```

Series, volumes, and pages use natural sorting, so `Vol2` comes before `Vol10` without zero-padding.

### Covers

A 1:1.42 cover ratio works best. A 256x364 JPEG or PNG is enough for the interface; 512x728 leaves more room for thumbnail generation. Tankobon crops other ratios instead of stretching them.

### Page images

For a reasonable balance of image quality, load time, and memory use:

- Use JPEG at 80 to 85 percent quality.
- Keep the original aspect ratio.
- Aim for 960 pixels wide and 1360 to 1500 pixels tall.
- Aim for 150 to 350 KB per page.

Pages 720 to 800 pixels wide load faster. For larger JPEG pages, Tankobon uses the decoder's native downscaling and keeps up to 1200 pixels of horizontal detail. This avoids allocating the full source resolution while preserving enough detail for the magnifier. PNG pages still decode at their original resolution.

## Controls

### Library

| Button                | Action                     |
| :-------------------- | :------------------------- |
| D-pad or analog stick | Move the selection         |
| L / R                 | Jump one page in list view |
| Cross                 | Open a series              |
| Square                | Change the library layout  |
| Circle                | Resume the last volume     |
| Select                | Rescan the library         |
| Start                 | Open options               |

### Volume list

| Button   | Action                   |
| :------- | :----------------------- |
| D-pad    | Move the selection       |
| L / R    | Jump one page            |
| Cross    | Read the selected volume |
| Triangle | Return to the library    |
| Start    | Open options             |

### Reader

| Button             | Action                                           |
| :----------------- | :----------------------------------------------- |
| Analog stick       | Scroll, or pan while magnified                   |
| D-pad up / down    | Scroll quickly                                   |
| L / R              | Move to the previous or next page                |
| D-pad left / right | Turn the page according to the reading direction |
| Square             | Toggle the magnifier                             |
| Circle             | Change the view mode                             |
| Triangle           | Close the volume                                 |
| Start              | Open options                                     |
| Select             | Show the controls                                |

## Options

Press Start from any screen to open the options menu. Tankobon applies changes immediately and writes them to `settings.dat` beside the EBOOT.

| Option            | Values                                     | What it changes                                      |
| :---------------- | :----------------------------------------- | :--------------------------------------------------- |
| Theme             | 11 dark themes, 10 light themes             | Interface colors                                     |
| View mode         | Fit width, fit screen, rotate 90 degrees   | How each page is sized and oriented                   |
| Page background   | Auto, black, white                         | The unused area beside or above the page              |
| Reading direction | Manga right-to-left, western left-to-right | Which side advances to the next page                  |
| Auto-crop         | On, off                                    | Whether plain scan margins are trimmed before fitting |
| Page transitions  | On, off                                    | Whether page changes slide or happen immediately      |

The selected option shows its explanation beside a circled information icon. The automatic page background samples the image edges and chooses a matching light or dark color for the unused screen area.

### Theme browser

Highlight Theme in the options menu and press Cross.

| Button             | Action                                          |
| :----------------- | :---------------------------------------------- |
| D-pad up / down    | Move through matching themes                    |
| D-pad left / right | Filter all, dark, or light themes               |
| Select             | Open or close the on-screen keyboard            |
| Cross              | Add a search letter or apply the selected theme |
| Square             | Delete a search letter or clear the search      |
| Circle             | Leave search entry or return to options         |

Search is case-insensitive and matches any part of a name. `CAT` finds the Catppuccin themes, while `PINE` finds Rose Pine and Rose Pine Dawn. You can combine the name search with the light or dark filter.

<details>
<summary>Included themes</summary>

| Dark                 | Light                |
| :------------------- | :------------------- |
| Dracula              | Atom One Light       |
| Night Owl            | Ayu Light            |
| TokyoNight Night     | Catppuccin Latte     |
| Gruvbox Dark         | Everforest Light Med |
| Nord                 | GitHub Light Default |
| Catppuccin Macchiato | Gruvbox Light        |
| Atom One Dark        | Night Owlish Light   |
| Catppuccin Mocha     | Rose Pine Dawn       |
| Rose Pine            | TokyoNight Day       |
| Catppuccin Frappe    | Alucard              |
| Lovelace             |                      |

Tankobon maps each palette's background, foreground, selection, blue, and red colors to its interface. It derives panel, border, and secondary text colors from those values. The original theme projects retain their names and copyrights; see [Theme sources and licenses](THEME_SOURCES.md).

</details>

### Library scan

Press Square in the library to switch between the cover grid, cover list, and text list. Press Select to scan the `mangas` directory again after adding or removing files. The scan reuses existing thumbnails and creates any that are missing.

### Reading progress

The cover grid uses a thin progress rail, while the cover list and text list show a percentage. The library status bar shows the exact saved volume, page, and percentage. The volume list also marks the saved page.

Existing bookmark files immediately provide the saved volume and page. Tankobon adds the volume's total page count after you open or save it, which lets the library calculate a percentage. Overall series progress assumes that every volume before the current one is complete.

## Build

Building requires [PSPSDK](https://pspdev.github.io/).

```sh
git clone https://github.com/revnandi/tankobon.git
cd tankobon
make clean
make
```

The build writes `EBOOT.PBP` to the project root.

| Command          | Purpose                                                                         |
| :--------------- | :------------------------------------------------------------------------------ |
| `make`           | Build the application                                                           |
| `make DEBUG=1`   | Write a boot trace to `ms0:/tankobon_boot.log` and show on-screen stage markers |
| `make PROBE=1`   | Add a four-color display probe to the boot trace build                          |
| `make test-host` | Compile and run the host-side tests for `core.c`                                |

Editable XMB artwork is in [`assets/`](assets/README.md). Export the SVG templates to the documented PNG names and dimensions. The Makefile includes any exported artwork it finds when it builds the EBOOT.

## Code layout

```text
assets/             XMB artwork and export notes
THEME_SOURCES.md    Theme attribution and license links
tests/test_core.c   Host-side tests for sorting and bookmark parsing
core.c              Natural sorting and bookmark parsing
core.h              Declarations for the host-testable code
main.c              Rendering, input, CBZ handling, and UI
Makefile            PSP and host test targets
VERSION             Current release version
stb_image.h         Vendored PNG and fallback image decoder
```

`core.c` has no PSP headers, so `make test-host` can test it on a regular development machine.

### Rendering notes

Tankobon resamples each page once into a screen-sized surface, including rotation and box filtering. Scrolling, panning, and page slides then copy framebuffer rows instead of rescaling the source every frame. A source image that would need more than 6 MB at the current zoom falls back to direct sampling.

The main loop runs once per vertical blank, even when the display does not need a redraw. Input repeat and animations therefore use real 60 Hz frames.

Page loading is synchronous. Tankobon keeps the previous rendered page visible while the activity indicator advances during each 128 KB archive read and decompression step. Deflated entries are expanded as a stream, so Tankobon does not hold complete compressed and decompressed copies at the same time. Large JPEG pages use libjpeg's native downscaling before scanlines enter the page buffer.

## Upgrading from Tsundoku

Tankobon was previously named Tsundoku. Rename `ms0:/PSP/GAME/Tsundoku/` to `Tankobon/`. The `bookmarks.dat`, `settings.dat`, and `.thumbs/` files remain in that directory, so the rename keeps your progress, options, and cached covers.

## Contributing

Issues and pull requests are welcome. Run `make test-host` when changing sorting or bookmark parsing. Changes to rendering and input still need testing on PSP hardware or an emulator.

## Credits

Tankobon is based on [Tsundoku PSP](https://github.com/hash-asterisk/tsundoku-psp) by hash-asterisk. This fork was created to continue development independently while preserving the original MIT license and attribution.

## License

MIT.
