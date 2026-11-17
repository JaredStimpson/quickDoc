# v5.2 layout update

Toolbar groups now use fixed left-aligned packing without any spare-width stretch. Wrapping, child-window repaints, and the existing read-only functionality remain unchanged. Confirm visual behavior on Windows after building.

---

# quickDoc - Roadmap and implementation status (v2)

## Goal and architecture

Native Windows x64 PDF viewer optimized for fast interactive startup, continuous vertical scrolling, page and chapter navigation, and minimal deployment overhead.

- **Language/UI:** C++20 + Win32, without Electron/Python/.NET runtime.
- **Graphics:** Direct2D drawing on main thread.
- **PDF engine:** PDFium, on a single dedicated serialized worker thread.
- **Development deployment:** EXE + DLL, downloaded once to ignored `.deps/`.
- **Portable distribution:** single EXE containing the PDFium DLL as an embedded resource; caches it to `%LOCALAPPDATA%` on first launch. This is an interim trade-off. **Original fully statically linked/no-extraction requirement remains open.**
- **Repository:** one main source file, CMake and resource template, three BAT scripts, two docs, gitignore. Complete ZIP replacements supported.

## Implemented in source

- [x] Native resizable Win32 application, initial window before file loading.
- [x] Open PDF using file dialog, command-line path or drag-and-drop.
- [x] Continuous document with all page dimensions, page gaps, and vertical scrolling.
- [x] Virtualized drawing: viewport pages only, with neighboring-page prefetch requests.
- [x] Worker PDFium rendering, no rendering inside the UI paint callback.
- [x] Bitmap cache with approximate 128 MiB CPU buffer limit; stale generation checks on document switches.
- [x] Direct2D bitmap compositing and recreation after device-target loss.
- [x] Page field, jump-to-page without rendering intermediate pages; next/previous page controls.
- [x] Embedded PDF bookmark hierarchy and chapter-page jumping.
- [x] Next/previous bookmarked chapter shortcut.
- [x] Fit to width and manual zoom, Ctrl+mouse-wheel zoom.
- [x] 64-bit-capable logical scroll positions mapped to Win32 scrollbar range.
- [x] Per-monitor DPI awareness request.
- [x] `setup.bat` one-time download/build, `launch.bat` no build, `build.bat` single-file resource-embedded release.
- [x] Minimal single-source repository and replacement-ZIP workflow.

## Remaining / not verified

- [ ] **Windows build and execution not tested.** Source, CMake, batch scripts and resource-embedding may need adjustment based on actual MSVC build errors.
- [ ] **True static PDFium EXE without cache extraction:** not implemented. Current one-file release embeds a DLL and extracts it once per version into `%LOCALAPPDATA%`. A fully static PDFium build requires GN/Ninja, static dependency closure, and extensive MSVC/Clang toolchain integration.
- [ ] **Speed benchmarking:** no measured cold/warm launch time, first-page paint time, scrolling frame time, RAM/VRAM usage, or SumatraPDF comparison yet.
- [ ] **Password prompt:** encrypted/password-protected documents currently show a generic error.
- [ ] **Fit-to-page:** only fit-to-width and manual zoom are currently available.
- [ ] **Precise bookmark destination coordinates:** chapter jumps go to destination pages, not their exact in-page positions.
- [ ] **Accurate PDF page rotations / page labels:** basic PDF page sizes and consecutive 1-based page numbers are used.
- [ ] **Scroll input polish:** no touch/precision-trackpad gesture-specific handling or touch inertia tuning; mouse wheel and standard scrollbar are implemented.
- [ ] **PDFium dependency version pinned and integrity verified:** current setup fetches the latest third-party build, which can change unexpectedly.
- [ ] **Memory cache refinements:** 128 MiB CPU budget, GPU copies not counted. No progressive tiled rendering of gigantic pages. Heavy or highly pathological PDFs may respond poorly.
- [ ] **Open-file cancellation:** worker renders serialized work; loading a huge document can delay switching to a new document.
- [ ] **Comprehensive error UX:** no advanced diagnostics or specialized messages for missing font, unsupported stream, corrupt PDF, encrypted PDF.
- [ ] **Packaged third-party license notices and automated dependency audit:** redistribution requires reviewing PDFium and its third-party notices.
- [ ] **Large-file streaming:** current implementation reads entire PDF into memory, with an approximately 2 GiB cutoff.
- [ ] **Automated Windows build/test pipeline and binary import audit:** not available in current environment.

## Verification / acceptance checklist for Windows PC

1. From clean ZIP, run `setup.bat` with VS2022 C++ build tools, CMake and network access.
2. Run `launch.bat` twice. Verify second use does not compile and the window opens quickly.
3. Test one-page, 100-page, and 1000-page PDFs; test non-ASCII filename, embedded images and mixed page sizes.
4. Use mouse wheel, scrollbar, page entry, Ctrl+G, Page Up/Down, chapters, F12, Ctrl+Page Up/Down, Ctrl+wheel, resize, and file drag/drop.
5. Run `build.bat`. Copy **only** `dist/quickDoc.exe` to a clean Win10/11 x64 account; open a PDF and confirm `%LOCALAPPDATA%\quickDoc\engine-*\pdfium.dll` gets created only on the initial launch.
6. Verify startup with Windows Defender enabled, cold and warm, and compare against SumatraPDF.
7. Use `dumpbin /dependents dist\quickDoc.exe` to confirm import behavior; a delay-loaded PDFium import is expected, with the DLL supplied from the cache.
8. Run malformed and password-protected PDF tests; confirm no crashes/hangs, record remaining issues.

## Possible next milestone

First: compile/test on the actual Windows development PC, fix concrete MSVC and runtime issues, add fit-to-page and exact bookmark coordinates, and pin/checksum the PDFium revision. Second: if the cache-on-first-launch tradeoff is unacceptable, replace the release packaging mechanism with a genuine static PDFium link.

## v2.2 scroll flash mitigation

The renderer now schedules visible pages ahead of nearby prefetch pages, uses cached page images while a new zoom-resolution bitmap is pending, and avoids unnecessary render-target resizing. Verify wheel and scrollbar scrolling on Windows with long/image-heavy PDFs. Note that never-rendered pages may still show placeholders for a short period; complete elimination would require stronger scrolling cache or loading behavior.

## v2.3 flicker correction

- Restrict scrolling redraw to the document rectangle.
- Use `WS_CLIPCHILDREN` to protect native button, edit, and tree controls.
- Restrict the Direct2D clear region to the document viewport.
- Pending: verify on Windows that toolbar and bookmark tree remain stable while scrolling.


## v3 implementation update

- [x] Sidebar defaults to lazily rendered page snapshots.
- [x] Chapters button toggles sidebar between bookmarks and page thumbnails (F12).
- [x] Click a thumbnail to jump to that page; current page outlined.
- [x] Scroll preview list independently; follow main page position on document scroll.
- [x] Render thumbnail previews on the same serialized worker, behind visible full-size page work.
- [x] Separate limited thumbnail bitmap cache and double-buffered thumbnail pane.
- [ ] Compile and test v3 with MSVC on Windows.
- [ ] Measure startup, memory, and scrolling performance on representative PDFs.
- [ ] Fully static/no-extraction PDFium distribution, as previously discussed.

## v3.1 Page-selector adjustment

Page entry is editable current page only; total page count is a fixed adjacent label. Clicking or focusing the page field selects all text for immediate replacement. Updates while typing are suppressed.


## v4.1 implementation: transient book page offset
- Checkbox enabled mapping; dependent anchor, edit, set, offset controls disabled when checkbox off.
- Show current real PDF page as anchor; entering a printed page and setting computes signed offset.
- Jump-to-page and current-page display translate via constant offset; PDF renderer and thumbnail indices stay unchanged.
- Preserve mapping when checkbox turned off/on, reset on OpenPDF.
- No file storage, PDF modifications, OCR, or performance-heavy processing.
- Windows compilation and interactive verification remain necessary.


## v4.2
Renamed to quickDoc. Added Fit page beside Fit width, and made Fit page default for each opened document. Icon selection pending.

## v4.3

Embedded icon option 3 (brown leather grimoire) in both build configurations using a Win32 ICON resource.

## v4.4 implemented in source (Windows verification pending)

- Default dark-mode UI and dark titlebar.
- Instance-only page rotation (current page); arrow scrolling and F11 fullscreen.
- Drag-adjustable sidebar width; two-page layout; default fit-to-page.
- Ctrl+F asynchronous text-layer search and match navigation, no OCR.
- No persistent viewer preferences or PDF modifications.

Open questions for later versions: exact text hit highlighting/scroll-to-character, stronger accessibility/theme support for native controls, Windows compile and runtime performance profiling.

## v4.4.2 scope and testing
- [x] Toolbar toggle for dark/light UI (dark default; session only).
- [x] Arrow-key previous/next full pages (page pair in two-page mode); do not hijack edit fields.
- [x] Explicit invalidation of swept sidebar divider region during dragging.
- [x] Clip owner-drawn button text.
- [ ] Compile with MSVC on Windows and inspect visual behavior.
- [ ] Test repeated sidebar drags at multiple widths and both themes.


## v5 toolbar update
- Responsive groups: Open | Chapters | page navigation | zoom/fits | rotation | single/two page | theme | book-page mapping.
- Controls stay in coherent groups, with group spacing distributed across the window; whole groups wrap to new rows as needed.
- Book # remains a checkbox, and the PDF page reference and offset controls become disabled when unchecked.
- Dark mode starts enabled and can be toggled without saving state.
- All viewing changes remain session-only.


## v5.1 layout hardening
- Host all native toolbar controls in a separate opaque child HWND to avoid Direct2D child-window redraw artifacts.
- Use group-aware line wrapping and atomic DeferWindowPos moves on resize.
- Owner-draw dynamic text labels with explicit background clears (page count, PDF reference, offset, search status).
- Enforce a minimum track width that accommodates the largest group.
- Do not persist configuration or modify opened PDF files.
