# dwg2cnc

Converts DWG/DXF drawings into G-code (`.TXT`) for the **FLSK F2100B** controller
(portable plasma/flame cutting machines such as MiniCut). It builds the cut path, lead-ins/outs,
kerf compensation (G41) and hole handling, and previews the result.

> ⚠️ **Language warning:** all source code comments, UI text, `.ini` settings comments, batch files and
> the user guide (`PANDUAN.txt`) are written in **Indonesian (Bahasa Indonesia)**. This README is the only English document.

## Repository layout

| Folder | What it is |
|---|---|
| `cpp/` | C++ (Windows, MSVC) implementation: console `dwg2cnc.exe` + GUI `dwg2cnc-gui.exe` with 2D/3D cutting animation. No Python needed. |
| `web/` | Offline browser tool (HTML/JS, three.js, libredwg-web WASM): upload DWG, preview, 3D simulation, save `.TXT`. |
| `python/` | Reference implementation (`dwg2cnc.py`, needs `ezdxf` + `matplotlib`). The C++ output is tested to be byte-identical to it. |
| `libredwg/` | Prebuilt LibreDWG (GPLv3) `dwg2dxf.exe` + DLL, used by `cpp/` and `python/` to read DWG. |
| `PANDUAN.txt` | Operator guide (Indonesian), including how to load files on the machine. |

## How it works

1. **Read** — a DWG is first converted to DXF with LibreDWG (`dwg2dxf`); DXF is read directly. The web tool uses the libredwg-web WASM build instead.
2. **Filter** — entities (LINE, ARC, CIRCLE, polylines, nested blocks) are flattened into segments. Layers matching `ignore_layers` (text, dimensions, marks...) are dropped; if `cut_layers` is set, only those are kept.
3. **Chain** — loose segments are joined into closed loops when their ends are within `join_tolerance`. Duplicate loops are removed and full circles are detected.
4. **Classify** — loops are nested by point-in-polygon: an outer loop is a *part*, loops inside it are *holes*. Holes are skipped or cut per `holes` / `min_hole_diameter`.
5. **Plan each cut** — direction is set so kerf compensation lands on the scrap side (G41 uses the machine's Kerf parameter). Round holes (and loops of arcs that form a full circle) use a **hook lead-in**: pierce at the centre (up to `center_pierce_max_radius`, larger holes pierce near the edge), dwell (`pierce_dwell`), a short straight stem, a small arc of radius `hook_radius` that meets the circle tangentially (no notch in the hole wall), the full circle CCW, then a mirrored hook back to the pierce point. The path stays on the drawing line; the machine applies the kerf through G41 like every other contour. Outer contours get a lead-in and lead-out 45° either side of the start-point normal, crossing in an X (in line with the part edges on a 90° corner), so the cut always runs through the start point and the part drops free. Other holes lead in and out along the corner bisector, since an X at a hole corner would cut into the part. `lead_out` sets the lead-out length (0 = none). The start point is a corner or the middle of the longest edge (`start_point`).
6. **Order** — nearest part first; inside a part, holes by nearest pierce point, then the outer contour last so the part stays supported.
7. **Write G-code** — `G92 G21 G90/G91`, then rapid moves (`G00`), `M07` torch on, cuts (`G01/G02/G03`), `M08` torch off, return home, `M02`. Coordinates are shifted by `offset_x/y` and may be incremental (G91, the FastCAM style) or absolute (G90).
8. **Verify and preview** — the generated G-code is re-parsed and simulated to check the path and estimate cut time (`cut_speed`), then drawn as `_preview.png` (CLI) or animated in 2D/3D (GUI and web).

The C++ and Python versions follow the same pipeline; `cpp/test/test.bat` checks that the C++ output matches the Python output exactly.

## Usage

**C++ (recommended)** — build with Visual Studio 2022 (set `VCVARS` if installed elsewhere):

```bat
cpp\build.bat          :: produces dist\dwg2cnc.exe and dist\dwg2cnc-gui.exe
cpp\test\test.bat      :: regression test, prints LULUS (pass) / GAGAL (fail)
dist\dwg2cnc.exe part.dwg other.dxf -o out_folder
```

Drag files or a folder onto the exe (or the GUI window). Settings come from `dwg2cnc.ini` next to the exe.

**Web** — run `web\BUKA-WEB.bat` (needs Python only to serve `http://localhost:8765`).

**Python** — `python\KONVERSI.bat` (drag DWG/DXF onto it) or `python python/dwg2cnc.py part.dwg --holes skip --mode G90`.

## Settings (`dwg2cnc.ini`)

`mode` (G91/G90), `kerf` (G41/NONE), `holes` (cut/skip), `min_hole_diameter`, `lead_in_outer`, `lead_in_hole`,
`center_pierce_max_radius`, `hook_radius`, `pierce_dwell`, `lead_out`, `start_point`, `offset_x/y`,
`ignore_layers`, `cut_layers`, `cut_speed`, `preview`, etc. Each key is documented (in Indonesian) in the file itself.

A round hole, D=18 with `hook_radius = 2.5` (G91, relative to the hole centre):

```
G00X..Y..                          rapid to the hole centre
G41                                kerf compensation from the machine's Kerf parameter
M07
G04P3.0                            pierce dwell (pierce_dwell)
G01X-5.538Y2.308                   hook stem
G03X-3.462Y-2.308I-0.962J-2.308    hook arc, tangent to the circle
G03X18.0Y0.0I9.0J0.0               circle, first half (drawing radius 9)
G03X-18.0Y0.0I-9.0J0.0             circle, second half
G03X3.462Y-2.308I2.5J0.0           lead-out arc
G01X5.538Y2.308                    back to the centre
M08
G40
```

The hook radius shrinks automatically on small holes (at most 0.4 × the stem distance). With G41 the machine
shrinks the hook arc by half its Kerf, so keep `hook_radius` larger than that.

Only G-codes from the F2100B manual are emitted: `G92 G90 G91 G21 G41 G40 G00 G01 G02 G03 M07 M08 M02`, plus the
round-hole pierce dwell (`pierce_dwell`, default `G04P3.0`; leave it empty if your controller rejects it).
**Always dry-run (`Space > F3 Frame`, no flame) before cutting.**

## License

LibreDWG is GPLv3 (see `libredwg/`). Prebuilt `.exe` files are not tracked in git; build them with `cpp/build.bat`.
