# quickDoc v5.2 — Left-aligned toolbar groups

Changes from v5.1:
- Toolbar groups now pack tightly against the top-left corner instead of spreading evenly across available width.
- Keeps fixed 14 px gaps between groups and 4 px gaps within groups.
- Extra window width remains empty on the right; narrow windows still wrap whole groups to additional rows.
- Preserves the v5.1 toolbar repaint isolation, text clearing, and resizing fixes.
- No viewer behavior, PDF content, or settings persistence changes.

## Install

Extract over the existing quickDoc repository. Keep `.deps` and `.build` if present. Run `setup.bat` to compile, then `launch.bat` for subsequent launches. `build.bat` still creates the single-file portable release.

**Windows compilation and visual behavior have not been verified in this environment.**

---

# quickDoc v5.1 — Toolbar layout and text rendering fix

Changes from v5.0:
- Toolbar now lives in a dedicated, opaque native Win32 child window, isolated from Direct2D PDF drawing.
- Responsive group packing recomputes from the actual client width and moves controls in a deferred batch.
- The book-page controls and search controls remain together within each group; groups wrap on smaller windows.
- The PDF-page reference, page total, offset, and search status labels paint their full backgrounds before text, eliminating overprinted glyphs.
- A minimum window width ensures no unbreakable group can overflow its row during normal resizing.
- All previous features and the setup.bat / launch.bat / build.bat workflow are unchanged.
- The PDF remains read-only; no viewer state is saved.

## Install

Extract the entire ZIP over the v5 repository. Preserve `.deps` to avoid redownloading PDFium; preserve `.build` if desired. Run `setup.bat` once, then `launch.bat` to open the existing development build. `build.bat` creates the single-file portable release.

**Windows MSVC compilation and visual behavior must still be tested on your PC.**

---

# quickDoc v5

Responsive grouped toolbar; Book # checkbox; dark mode toggle (on by default).

# quickDoc v4.3 - Windows x64 native PDF viewer

Small C++20 / Win32 / Direct2D PDF reader using PDFium. The repository keeps **one application source file** and **three separate entry points** for setup, launch, and release packaging.

## Quick start

Prerequisites on the **development PC only**: Windows 10/11 x64, Visual Studio 2022 Desktop development with C++ (MSVC + Windows SDK), CMake (in PATH), PowerShell, `tar` (built into modern Windows), and internet access for the initial PDFium download.

1. Run `setup.bat` once (or again after replacing source files). Downloads PDFium to `.deps/`, configures CMake, compiles `.build/Release/quickDoc.exe`, and copies `pdfium.dll` into that development build directory.
2. Run `launch.bat` whenever you want to start the app. **It never recompiles**. You can pass a PDF path to it, or open a PDF using Ctrl+O / drag-and-drop.
3. Run `build.bat` when you want a **single-file distributable** at `dist/quickDoc.exe`. This recompiles an optimized version with PDFium embedded as a Windows resource.

The first run of the standalone EXE writes its embedded `pdfium.dll` to a version-named subfolder of `%LOCALAPPDATA%\quickDoc` and uses that cached copy on subsequent runs. **This is a single-file distribution, not a fully statically linked or no-extraction executable.** The development build instead uses a neighboring DLL. The release executable can be copied to another Windows x64 machine, where its cache is initialized automatically. Standard Windows system DLLs remain required.

## v4.1 correction

Page navigation (Go, Enter, page arrows, chapter and thumbnail navigation) now queues bitmap rendering immediately after jumping. This fixes blank destination pages that previously needed a scroll or zoom to populate. Rendering stays asynchronous and only the PDF viewport is invalidated; the v2.3 toolbar/sidebar flicker fix remains in place.

## Controls

| Action | Control |
| --- | --- |
| Open PDF | Open button, Ctrl+O, drag-and-drop, CLI path |
| Scroll pages | Mouse wheel or vertical scrollbar |
| Go to page | Enter number in toolbar, Go, or Enter |
| Focus page field | Ctrl+G |
| Previous / next page | Toolbar arrows or Page Up / Page Down |
| Previous / next bookmarked chapter | Ctrl+Page Up / Ctrl+Page Down |
| Switch sidebar between page previews and chapters | Chapters / Pages button or F12 |
| Navigate by page preview | Click a thumbnail in the left sidebar |
| Scroll through page previews | Mouse wheel over the sidebar |
| Zoom | +/- toolbar or Ctrl+mouse wheel |
| Fit width | Fit width button or Ctrl+1 |

PDF chapter navigation relies on **embedded PDF bookmarks**. No bookmarks means an empty Chapters view (page previews still work); there is no background OCR or automatic chapter detection. PDF text selection, search, print, forms interaction, and password prompts are not in this version.

## Replacing repository ZIPs

Extract the latest ZIP over your existing `quickDoc/` folder and allow source files to overwrite. **Keep `.deps/`** to avoid downloading PDFium again. `.build/`, `.release/`, and `dist/` can also be preserved for faster incremental builds. Run `setup.bat` once after updating the source; `launch.bat` alone deliberately keeps using the previously compiled executable.

## Files

```text
quickDoc/
  main.cpp          Win32 viewer, PDFium worker, Direct2D, bookmarks, cache
  CMakeLists.txt    Development + embedded-release build targets
  pdfium.rc.in      Resource definition template for portable release
  setup.bat         Download dependencies / compile development version
  launch.bat        Start already-compiled development program
  build.bat         Compile distributable single EXE
  README.md         Instructions and limitations
  plan.md           Scope, architecture, checklist, remaining work
  .gitignore        Ignore downloaded and built files
```

## Implementation notes

- Virtualized continuous scrolling: all page *sizes* are read, but only nearby pages are rendered. Pages are laid out in a 64-bit-capable `double` document coordinate system and mapped to the Windows scrollbar range.
- One dedicated PDFium worker serializes PDF operations; completed raster pages are handed to the Direct2D UI thread. Bitmaps are cached with approximate 128 MiB CPU-pixel budget, with additional GPU memory used by Direct2D.
- PDF files are loaded into memory to support Unicode Windows paths using `FPDF_LoadMemDocument64`. Files larger than ~2 GiB are currently rejected.
- User preferences are not stored. No telemetry, services, update checks, or network operations occur in the application.
- Prebuilt PDFium comes from the third-party https://github.com/bblanchon/pdfium-binaries (latest x64 no-V8 release); it is not pinned yet. Review original PDFium/third-party licenses before redistribution.
- This code is **not Windows-compiled or run-tested** from the current Linux environment. Run setup and report any build log / runtime error; Windows SDK and PDFium-version changes may require adjustments.

## v2.1 build fixes

- Cast Win32 `LONG` rectangle dimensions to `int` before passing them to `std::max`.
- Fix missing repaint argument in the sidebar `MoveWindow` call.
- Use pointer-width-safe Win32 control identifiers and remove a shadowed variable warning.
- Keep both `setup.bat` and `build.bat` open when errors occur.
- The Direct2D call already passes the required three arguments; the invalid `size` expression caused the misleading overload error in the reported build.
- Windows compilation and execution still need to be confirmed locally.

## Troubleshooting

- If CMake or Visual Studio cannot be found: install Visual Studio 2022 Desktop development with C++, with MSVC and a Windows SDK, plus CMake, then reopen the console.
- If setup cannot download PDFium: check connectivity to GitHub Releases, remove partial `.deps/pdfium/`, and rerun setup.
- If `launch.bat` says the executable is missing: run `setup.bat`.
- If `build.bat` fails: keep the full compiler error output. Release uses resource compilation and delay loading; this is not the same as PDFium static linking.
- If the single EXE cannot open at all: check write access to `%LOCALAPPDATA%\quickDoc`; this folder is used for the embedded DLL cache.

## v2.2 scrolling improvements

- Prioritize on-screen pages before prefetch renders so visible content is not delayed behind off-screen work.
- Reuse cached page bitmaps as temporary scaled images during zoom and fit-width changes.
- Resize the Direct2D render target only when the window actually changes size.
- Avoid Win32 background erasure and replace stark white loading rectangles with softer neutral placeholders.
- Extremely fast jumps to never-rendered pages may still show a placeholder momentarily; this is not a synchronous whole-document renderer.
- Compilation and visual testing on Windows are still required for this revision.

## v2.3 - toolbar and sidebar flicker fix

Scrolling now invalidates only the PDF viewport, not the full main window.
The Win32 parent uses `WS_CLIPCHILDREN` so Direct2D does not repaint over
buttons, the page input, or the bookmark tree. The Direct2D clear operation
is clipped to the PDF viewport. No scrolling or PDF engine functionality
was intentionally changed.

This fix has not been compiled or visually validated on Windows in the
authoring environment. Keep `.deps` and `.build`, overwrite the repository
files, and run `setup.bat` once before resuming `launch.bat`.

## v3 — page thumbnails and chapter toggle

- The left sidebar starts in **Pages** mode and displays nearby page previews.
- Click **Chapters** (or press F12) to switch to the embedded-bookmark tree; the button then reads **Pages** to switch back.
- Clicking a thumbnail jumps to the corresponding page. The current page has a blue border.
- The thumbnail list follows the current document page while scrolling the main document. Scrolling the thumbnail list itself temporarily disables automatic following until the document is scrolled again.
- Thumbnails are generated on demand at a maximum of 150 x 143 pixels by the existing PDFium worker. Visible full-size document pages always have rendering priority.
- Thumbnail memory is capped at about 24 MiB, separate from the main page bitmap cache. A double-buffered native thumbnail window and the v2.3 child-window clipping are intended to prevent toolbar/sidebar flashing.
- All application code remains in `main.cpp`; no new runtime libraries or external files are needed.
- Windows compilation and runtime behavior of v3 **have not been verified** in this environment.

## v3.1 — page selector usability

- The page-number edit box now contains **only the current page number**.
- A separate non-editable label immediately beside it displays **/ total pages**.
- Clicking the edit box selects the full current number, so typing immediately replaces it. Ctrl+G also selects the full number.
- Live page updates do not overwrite a number being typed while the field has focus.
- No changes to rendering, scrolling, caching, or release packaging.

## v4.1 - printed book-page offset

- Enable **Book #** on the toolbar. The controls to its right remain disabled until enabled and a PDF is open.
- Navigate to a page with a known printed page number. The label **PDF 190** shows the actual PDF page at the current location.
- Enter the printed book page in the adjacent field (for example **185**) and click **Set** or press Enter. The application calculates `offset = 185 - 190 = -5`.
- The normal page entry field now shows book page numbers and Go jumps using the offset (book page 185 -> PDF page 190). The total label explicitly says PDF because there may be no meaningful printed-book total.
- The **PDF n** label follows the document as you navigate. Press Set again with a newly entered printed page to recalibrate using the *current* PDF page.
- Uncheck **Book #** to return to standard PDF numbering; check it again to restore the mapping. Opening another file resets the mapping. It is never saved to disk.
- This is a single linear offset. Separate Roman-numeral prefaces, missing pages, and numbering resets cannot be mapped correctly with one offset.
- When an offset maps the first PDF pages to zero or negative numbers, the displayed value can be zero/negative; these are mathematical labels, not evidence of actual printed numbering.
- UI-only arithmetic: no extra document rendering or dependencies.

**Build status:** The C++ changes have been syntax-inspected but not compiled on Windows in this environment.


## v4.2 changes
- App and Windows executable renamed quickDoc (`quickDoc.exe`).
- `Fit page` added next to `Fit width`. On opening each PDF, Fit page is the default.
- Fit page scales to the viewport width and height so a whole PDF page fits (including padding), using the largest page dimensions for mixed-sized PDFs.
- `Ctrl+1`: Fit width; `Ctrl+2`: Fit page. `+`, `-`, and Ctrl+wheel switch to manual zoom.
- Icon artwork is not bundled yet; a user-selected icon will be installed in a future version.
- Development exe: `.build\Release\quickDoc.exe`; release exe: `dist\quickDoc.exe`.

## v4.3: Windows executable icon

- Selected concept **3**: brown leather grimoire with gold star and clasp.
- `quickDoc.ico` includes 16/24/32/48/64/128/256 pixel resolutions and is embedded in both development and release builds through `quickDoc.rc`.
- `quickDoc-icon.png` is an optional source preview.
- No runtime icon file is needed; the portable `quickDoc.exe` contains the icon.
- If Windows Explorer continues to show an old icon, refresh Explorer/icon cache; the app executable may have been cached by Windows.

## v4.4: Reading layout, dark mode, and text search

- Default dark window chrome, toolbar, sidebar, thumbnail panel, and controls. PDFs retain their original colors.
- Rotate the *current page* 90 degrees left/right using the toolbar or Ctrl+R (right). Rotation remains in memory and resets on reopening the document. The PDF file is never edited.
- Two pages: toggle side-by-side consecutive pages. Fit Page (default when opening) scales a row vertically and horizontally to fit the viewport.
- Up and Down arrows scroll. F11 toggles fullscreen, while Ctrl+1 / Ctrl+2 choose Fit Width / Fit Page.
- Drag the vertical divider beside the sidebar to resize it for this session.
- Ctrl+F opens the search field. Type a phrase, press Enter to begin asynchronous case-insensitive PDF text-layer search, then use the adjacent Previous/Next buttons to jump between occurrences. Escape hides the search field. The result counter updates while scanning. The search does not do OCR and does not currently highlight exact matched glyphs; it jumps to the page that contains the match. Scanned PDFs without a selectable text layer will not produce results.
- All viewer changes are session-only. No PDF content is changed, and settings, rotation, search results, and layout state are not saved to disk. The existing single-file portable release **does** still extract its embedded PDFium DLL to the local dependency cache on first launch; this is not a document write.

**Verification:** Windows/MSVC compilation and runtime remain to be tested on the user's machine. In particular, test sidebar repainting, mixed-aspect PDF spreads, rotated thumbnails, and search while scrolling.

## v4.4.2 UI correction

- Added a **Light mode / Dark mode** toggle to the top toolbar. Dark mode remains the default; changing themes is session-only and never writes a preference or alters the PDF.
- **Up/Down arrow keys** jump to the previous/next PDF page (or page pair in two-page mode). When typing into text/page inputs, the keys retain their native editing behavior. The mouse wheel and scroll bar still scroll continuously.
- Fixed **splitter trails during sidebar resizing** by invalidating the full swept region of the parent/document canvas and repainting the sidebar children when dragging ends.
- Clipped owner-drawn toolbar labels to the control rectangle to prevent stale/overlapping drawing at the toolbar edge.
- PDF files continue to be opened read-only. No rotations or view settings are saved.

**Windows compilation and runtime repainting remain to be verified on the user's machine.**
