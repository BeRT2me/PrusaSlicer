# Object‑list scroll fix — `BitmapIconRenderer`

A line‑by‑line explanation of the change that fixes slow object‑list scrolling
(GitHub [#15177](https://github.com/prusa3d/PrusaSlicer/issues/15177),
[#11557](https://github.com/prusa3d/PrusaSlicer/issues/11557)).

**Three files changed:**

| file | change |
|------|--------|
| `src/slic3r/GUI/ExtraRenderers.hpp` | declares the new `BitmapIconRenderer` class |
| `src/slic3r/GUI/ExtraRenderers.cpp` | defines its methods + the static cache |
| `src/slic3r/GUI/GUI_ObjectList.cpp` | wires the renderer onto the Print/Editing columns |

The renderer lives in `ExtraRenderers.*` alongside PrusaSlicer's other custom
renderers (`BitmapTextRenderer`, `BitmapChoiceRenderer`, `TextRenderer`), which
follow the same pattern. `GUI_ObjectList.cpp` just uses it.

---

## 1. The problem, precisely

The object list is a `wxDataViewCtrl`. Its columns are:

| col | content | renderer (before) |
|-----|---------|-------------------|
| Name | icon + text | `BitmapTextRenderer` (PrusaSlicer custom) |
| Print | eye icon | **stock `wxDataViewBitmapRenderer`** (via `AppendBitmapColumn`) |
| Extruder | swatch + number | `BitmapChoiceRenderer` (PrusaSlicer custom) |
| Editing | gear icon | **stock `wxDataViewBitmapRenderer`** (via `AppendBitmapColumn`) |

The Print and Editing columns are pure‑icon columns that used wx's **stock**
bitmap renderer. That renderer draws each cell with `wxDC::DrawBitmap(icon, …)`.
On Windows, the object list paints into a `wxBufferedPaintDC`, and the icons are
**32‑bit bitmaps with an alpha channel** (they come from SVGs rasterized through
NanoSVG into a `wxBitmapBundle`). Drawing a 32‑bit *alpha* bitmap through wxMSW's
`DrawBitmap` takes the slow per‑pixel `AlphaBlend` path: **~330 µs per cell**.

Two things multiply that into visible lag:

1. **Every visible cell is redrawn on every scroll step** (the generic
   `wxDataViewCtrl` repaints the visible area rather than blitting and painting
   only the newly‑exposed strip).
2. **More visible rows = worse.** A tall window (e.g. full‑screen on 4K) shows
   more rows, so each repaint does more 330 µs blits.

Measured: scrolling a long list spent **seconds** in icon drawing — the list was
"almost impossible to scroll." Profiling confirmed it: the stock renderer's
`Render` was ~330 µs/call and dominated the scroll time, while the Name column
(mostly text, icons only on a few parent rows) and everything else were
negligible.

### Why we fix it in the renderer, not by upgrading wx

The expensive primitive (alpha `DrawBitmap`) and the repaint‑everything behaviour
both live in wxWidgets and predate our pinned version (3.2.6). There is **no
newer wx version that fixes this** — the only related upstream change is a 3.3.0
regression (`WS_EX_COMPOSITED` enabled by default) and its revert in 3.3.2, which
nets back to 3.2.6 behaviour. So the fix is at the Slic3r layer, using
`wxDataViewCtrl`'s supported extension point: a **custom cell renderer**.

### The idea (a memoization / `@lru_cache` pattern)

`AlphaBlend` is only needed because the icon is *translucent*. But the background
behind a given cell is a known solid colour. So: **alpha‑blend the icon onto that
background colour once** to get an *opaque* bitmap, then every subsequent paint is
a plain opaque blit (~10 µs, no `AlphaBlend`). Cache those composites, keyed by
`(icon, background colour)`, so the expensive blend happens once per unique
combination — not once per cell per scroll.

That's the whole fix. The rest is making the caching correct across (a) the
selection highlight, (b) light/dark themes, and (c) a second monitor at a
different DPI.

---

## 2. New include lines

**`ExtraRenderers.cpp`:**
```cpp
#include <boost/functional/hash.hpp>   // boost::hash_combine / boost::hash_range
#include <wx/dcmemory.h>               // wxMemoryDC (off-screen composite canvas)
```

**`ExtraRenderers.hpp`:**
```cpp
#include <map>                         // the std::map cache type in the class decl
```

**`GUI_ObjectList.cpp`:**
```cpp
#include "ExtraRenderers.hpp"         // to see the new BitmapIconRenderer
#include <map>                        // std::map (already used in this file; now explicit)
```

> Note: `GUI_ObjectList.cpp` already uses `std::map` elsewhere (e.g. a `sel_map`
> and a `cut_objects`) but previously got it only via a transitive include. The
> explicit `#include <map>` makes that dependency direct — keep it.

---

## 3. `BitmapIconRenderer`

### 3.0 Declaration (`ExtraRenderers.hpp`)

```cpp
class BitmapIconRenderer : public wxDataViewCustomRenderer
{
    wxBitmap m_src;    // the original alpha icon (fallback + cache-miss blend source)
    wxBitmap m_normal; // m_src pre-composited onto the current normal row background
    wxUint64 m_fp = 0; // content fingerprint of m_src (cache key)

    static std::map<std::pair<wxUint64, wxUint32>, wxBitmap> s_cache;

    static wxUint64        fingerprint(const wxBitmap& src);
    static const wxBitmap* find_composited(wxUint64 fp, wxUint32 rgb);
    static wxBitmap        make_composited(wxUint64 fp, const wxBitmap& src, const wxColour& bg);

public:
    BitmapIconRenderer(wxDataViewCellMode mode, int align)
        : wxDataViewCustomRenderer(wxS("wxBitmap"), mode, align) {}

    bool   SetValue(const wxVariant& value) override;
    bool   GetValue(wxVariant& WXUNUSED(value)) const override { return true; }
    wxSize GetSize() const override;
    bool   Render(wxRect cell, wxDC* dc, int state) override;
};
```

It derives from **`wxDataViewCustomRenderer`** — the wx base class meant for
user‑written renderers (you override `SetValue`/`GetValue`/`GetSize`/`Render` and
the framework calls them per cell). It's a *sibling* of the stock
`wxDataViewBitmapRenderer`, not a subclass of it; both ultimately descend from
`wxDataViewRenderer`. A **single instance is reused for every cell of its
column.**

### 3.1 Per‑instance members

```cpp
wxBitmap m_src;    // original alpha icon (fallback + cache-miss blend source)
wxBitmap m_normal; // m_src composited onto the normal row background
wxUint64 m_fp = 0; // content fingerprint of m_src (cache key)
```

Because the renderer is reused per cell (the control calls `SetValue` then
`Render` for each row), these hold "the icon for whichever cell is currently being
processed":

- `m_src` — the raw icon as handed to us (still has alpha). Needed in `Render` for
  the selected‑row composite, whose background colour isn't known until paint
  time, and as a fallback.
- `m_normal` — the icon already composited onto the *normal* row background
  (computed in `SetValue`, the common case). Opaque → fast blit.
- `m_fp` — a content hash of the icon, used as the cache key.

`wxBitmap` is reference‑counted: copying one is a cheap pointer/refcount bump, not
a pixel copy. So holding these copies is inexpensive.

### 3.2 The static cache

```cpp
// in the class:
static std::map<std::pair<wxUint64, wxUint32>, wxBitmap> s_cache;

// defined once in ExtraRenderers.cpp:
std::map<std::pair<wxUint64, wxUint32>, wxBitmap> BitmapIconRenderer::s_cache;
```

A **named class‑level static** (not a function‑local static) — so the shared
state is explicit in the class definition. It's shared across all
`BitmapIconRenderer` instances, i.e. the Print and Editing columns use one cache.
That's desirable: same icon + same colour = same composite.

The key is `std::pair<icon fingerprint, packed‑rgb colour>`. In Python terms:
```python
s_cache: dict[tuple[int, int], Bitmap] = {}
```
`std::map` needs keys that support `operator<` (it's an ordered tree).
`std::pair<wxUint64, wxUint32>` does; `wxColour` does not, which is why the colour
is reduced to a packed `wxUint32` via `bg.GetRGB()` (see §3.5).

### 3.3 `fingerprint()` — a stable, content‑based cache key

```cpp
wxUint64 BitmapIconRenderer::fingerprint(const wxBitmap& src)
{
    const wxImage img = src.ConvertToImage();
    const int w = img.GetWidth(), ht = img.GetHeight();
    size_t h = 0;
    boost::hash_combine(h, w);
    boost::hash_combine(h, ht);
    const size_t npixels = size_t(w) * ht;
    if (const unsigned char* d = img.GetData())
        boost::hash_range(h, d, d + npixels * 3);
    if (img.HasAlpha()) {
        const unsigned char* a = img.GetAlpha();
        boost::hash_range(h, a, a + npixels);
    }
    return static_cast<wxUint64>(h);
}
```

Hashes the icon's **content** (dimensions + pixels) into a 64‑bit value used as
the cache key.

- Takes a `wxBitmap`, calls `ConvertToImage()` to get a `wxImage` whose raw bytes
  it can read. (So callers never hold a `wxImage` themselves.)
- `boost::hash_combine(h, x)` folds a value into the running hash `h`;
  `boost::hash_range(h, begin, end)` folds the **bytes between two pointers**
  (their *contents*, not the addresses).
- `wxImage` is **planar**: RGB lives in one flat block (`GetData()`, `w*ht*3`
  bytes) and alpha in a separate block (`GetAlpha()`, `w*ht` bytes) — hence two
  separate `hash_range` calls.
- `d + npixels * 3` is one‑past‑the‑end pointer arithmetic over the RGB block.
- `npixels = size_t(w) * ht` is cast to `size_t` first so the multiply can't
  overflow `int`.
- Width and height are hashed separately because the flat pixel array doesn't
  encode its own dimensions (a 2×4 and a 4×2 image with the same bytes would
  otherwise collide).

**Why a content hash instead of the obvious key (the bitmap pointer)?** This is
the subtle bug that caused a 4K‑only crash. `wxBitmapBundle::GetBitmapFor()` (how
the model produces the icon) can return a **freshly allocated `wxBitmap` per
call** — notably at a second monitor's DPI, where the bundle re‑rasterizes. A
`wxBitmap`'s internal identity pointer would then differ every call even for
identical pixels, so a pointer‑keyed cache would miss every time, grow without
bound, and eventually throw `std::bad_alloc` mid‑scroll (observed crash:
`0xc0000005`/`0xc000041d` in `VCRUNTIME140.dll`). A content hash is identical for
identical pixels regardless of which object carries them, so the cache stays
bounded to the handful of distinct icons (eye‑open, eye‑closed, gear, …).

> Cost: `fingerprint` runs once per cell per paint (from `SetValue`), hashing a
> ~16×16–32×32 px icon (≈1–4 KB) → a few µs. Negligible next to the 330 µs blit
> it lets us skip.

### 3.4 `find_composited()` — cache lookup (hot path)

```cpp
const wxBitmap* BitmapIconRenderer::find_composited(wxUint64 fp, wxUint32 rgb)
{
    auto it = s_cache.find({fp, rgb});
    return it != s_cache.end() ? &it->second : nullptr;
}
```

Pure lookup: takes the two integers that form the key, returns a pointer to the
cached bitmap or `nullptr` on a miss. No image data, no allocation — so the hot
path (`Render`) can check the cache for almost nothing and only pay on a miss.
(`it->second` is the value; `it->first` would be the key. `s_cache.end()` is the
"not found" sentinel.)

Splitting lookup from the blend (next) is the key shape: a cache **hit** touches
only this cheap function.

### 3.5 `make_composited()` — the one‑time blend + insert

```cpp
wxBitmap BitmapIconRenderer::make_composited(wxUint64 fp, const wxBitmap& src, const wxColour& bg)
{
    if (s_cache.size() > 1024) s_cache.clear();
    wxBitmap out(src.GetWidth(), src.GetHeight(), 24);
    {
        wxMemoryDC mdc(out);
        mdc.SetBackground(wxBrush(bg));
        mdc.Clear();
        mdc.DrawBitmap(src, 0, 0, true);
    }
    return s_cache.emplace(std::make_pair(fp, bg.GetRGB()), std::move(out)).first->second;
}
```

Called **only on a miss**. Blends `src` (alpha) onto solid `bg`, stores the
opaque result, returns it.

- **`if (s_cache.size() > 1024) s_cache.clear();`** — insurance. In practice the
  cache holds ~(a few icons × a few colours) ≈ tens of entries. A pathological
  theme that paints selection as a gradient/anti‑aliased fill could feed many
  distinct sampled colours (see `Render`); the cap guarantees we never grow
  unbounded → never OOM. Clearing just forces recompute; correctness is
  unaffected.
- `wxBitmap out(w, h, 24)` — a new **24‑bit** (no alpha) bitmap, same size. A
  24‑bit bitmap always draws via a fast opaque blit later; the slow `AlphaBlend`
  path only triggers for 32‑bit‑with‑alpha bitmaps.
- `wxMemoryDC mdc(out)` — an off‑screen DC that draws *into* `out`. Wrapped in its
  own `{ }` scope so the DC's destructor runs (flushing to `out` and releasing it)
  **before** `out` is moved into the cache.
- `SetBackground(wxBrush(bg)); Clear();` — fill `out` with the background colour.
- `mdc.DrawBitmap(src, 0, 0, true)` — **the one expensive `AlphaBlend`**, done
  once. **`useMask = true` is *not* the 1‑bit‑mask (blocky) path** — that
  blockiness only came from `ConvertAlphaToMask` (an earlier, rejected approach),
  which we do **not** call. `src` keeps its full 8‑bit alpha channel, and on wxMSW
  `DrawBitmap` picks its path from what the bitmap *has*: because `src` has a real
  alpha channel, it uses the smooth per‑pixel `AlphaBlend`, so the result is
  anti‑aliased. `useMask = true` is just a harmless "use the source's transparency
  if it has any" hint (it would only select a 1‑bit mask if the bitmap had a mask
  and *no* alpha — not our case); it's there for robustness across bitmap formats.
  Smoothness comes from the preserved alpha, not the flag. Cost is irrelevant —
  this runs once per cache entry, never on scroll.
- `s_cache.emplace(std::make_pair(fp, bg.GetRGB()), std::move(out))` — inserts by
  **moving** `out` into the map (no pixel copy). `bg.GetRGB()` packs the colour
  into the `wxUint32` key. `.first->second` returns a reference to the stored
  bitmap (`std::map` keeps element references valid across later inserts).

### 3.6 Constructor — declaring the variant type

```cpp
BitmapIconRenderer(wxDataViewCellMode mode, int align)
    : wxDataViewCustomRenderer(wxS("wxBitmap"), mode, align) {}
```

A `wxDataViewCustomRenderer` is constructed with the **variant type string** it
handles. The model (`ObjectDataViewModel::GetValue`) pushes the icon as a
`wxBitmap` variant, so we declare `"wxBitmap"`. **If this string doesn't match the
value's type, the control silently skips the cell** — `SetValue`/`Render` are
never called and the icon just doesn't appear. (We hit exactly that when an
earlier version declared `"wxBitmapBundle"`.) `mode`/`align` are forwarded to the
base; see §4 for the values passed.

### 3.7 `SetValue()` — called per cell, before `Render`

```cpp
bool BitmapIconRenderer::SetValue(const wxVariant& value)
{
    wxBitmap src;
    if (value.GetType() == wxS("wxBitmap"))
        src << value;
    if (!src.IsOk()) { m_src = m_normal = wxNullBitmap; m_fp = 0; return true; }
    m_fp  = fingerprint(src);
    m_src = src;
    const wxColour bg = GetView()->GetBackgroundColour();
    if (const wxBitmap* b = find_composited(m_fp, bg.GetRGB()))
        m_normal = *b;
    else
        m_normal = make_composited(m_fp, src, bg);
    return true;
}
```

The control hands us this cell's value; we extract the icon and precompute what
we'll need.

- `src << value` — wx's variant‑extraction operator for `wxBitmap`. Only the
  `"wxBitmap"` case is handled because that's all the model ever pushes for these
  columns (via `GetBitmapFor()`); the generic `wxBitmapBundle` branch a broader
  renderer might add isn't needed.
- **Empty icon:** if there's no valid bitmap, reset the members and return;
  `Render` will no‑op.
- **Precompute the normal composite:** fingerprint the icon, keep `m_src`, then
  get the opaque composite for the **normal** row background
  (`GetView()->GetBackgroundColour()` — theme‑aware) via cache lookup, computing
  it only on a miss. So by paint time `m_normal` is a ready, cheap, opaque bitmap.

`return true` is the wx convention for "value accepted." `GetView()` returns the
owning `wxDataViewCtrl`.

### 3.8 `GetValue()` — required override, unused

```cpp
bool GetValue(wxVariant& WXUNUSED(value)) const override { return true; }
```

`wxDataViewCustomRenderer` requires `GetValue` (it's used to read a value back out
of an in‑place editor). These cells are inert (no editing), so there's nothing to
return. `WXUNUSED(value)` marks the parameter intentionally unused. (Defined
inline in the header.)

### 3.9 `GetSize()` — cell content size

```cpp
wxSize BitmapIconRenderer::GetSize() const
{
    if (m_normal.IsOk())
        return m_normal.GetSize();
    const int s = Slic3r::GUI::wxGetApp().em_unit();
    return wxSize(s, s);
}
```

The control uses this (with the renderer's alignment, §4) to position the rect
passed to `Render`. We report the icon's pixel size, or a DPI‑scaled `em` square
fallback if there's no icon. (`wxGetApp()` is fully qualified here because the
file's namespace context differs from `GUI_ObjectList.cpp`.)

### 3.10 `Render()` — the per‑paint hot path

```cpp
bool BitmapIconRenderer::Render(wxRect cell, wxDC* dc, int state)
{
    if (!m_normal.IsOk())
        return true;

    wxBitmap bmp = m_normal;
    if ((state & wxDATAVIEW_CELL_SELECTED) && m_src.IsOk()) {
        wxColour bg;
        if (dc->GetPixel(cell.x + 1, cell.y + cell.height / 2, &bg)) {
            if (const wxBitmap* b = find_composited(m_fp, bg.GetRGB()))
                bmp = *b;
            else
                bmp = make_composited(m_fp, m_src, bg);
        } else {
            bmp = m_src; // fallback: original alpha icon (correct, just slower)
        }
    }

    const wxSize sz = bmp.GetSize();
    dc->DrawBitmap(bmp, cell.x + (cell.width - sz.x) / 2,
                        cell.y + (cell.height - sz.y) / 2, false);
    return true;
}
```

Called for every visible cell on every paint — must be cheap.

- **No icon →** `return true`.
- **Non‑selected (common case):** `bmp = m_normal` — the opaque composite already
  built in `SetValue`. A cheap refcounted copy; near‑zero cost.
- **Selected rows** need a different background, and two wrinkles make it
  non‑trivial:
  1. The selection colour is **theme‑drawn** (dark‑mode grey, etc.) — not
     portably computable.
  2. The **active/focused** row is often a *different* shade than other selected
     rows, and that distinction is **not** in the `state` flags.

  So we **read the colour the control already painted into this cell**. The
  control paints the row background *before* calling the cell renderer, so at
  `Render` time the pixels under the cell are the real selection colour:
  - `dc->GetPixel(cell.x + 1, cell.y + cell.height / 2, &bg)` samples one pixel
    near the cell's left edge, vertically centred — background (the icon is centred
    and not yet drawn this frame), away from row borders.
  - then the same find/make cache pattern composites the icon onto that exact
    colour. Only a couple of distinct selection shades arise (plain selected,
    active), so the cache stays tiny.
  - Fallback `bmp = m_src` if `GetPixel` fails (e.g. some non‑MSW backends) —
    draws the original alpha icon directly: correct, just the slow blit, for that
    one cell.
- **Draw, centred:** `DrawBitmap(bmp, …, false)` — `useMask = false` because
  `bmp` is opaque (the composite already baked in the alpha); this is the fast
  path. `(cell.width - sz.x)/2` / `(cell.height - sz.y)/2` centre the icon (same
  idea as CSS `margin: auto`).

> **Why per‑cell `GetPixel` is safe.** Earlier crashes were initially blamed on
> `GetPixel`, but they were the *unbounded cache* OOM (§3.3). With the fingerprint
> key + size cap the cache is bounded, and `GetPixel` is a cheap (~µs) pixel read
> that only runs for *selected* cells (usually few).

---

## 4. Wiring the renderer into the columns (`GUI_ObjectList.cpp`)

The two `AppendBitmapColumn(...)` calls became explicit `AppendColumn(new
wxDataViewColumn(..., new BitmapIconRenderer(...), ...))` calls:

```cpp
// Print (eye) column
AppendColumn(new wxDataViewColumn(" ",
    new BitmapIconRenderer(wxDATAVIEW_CELL_INERT,
                           wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL),
    colPrint, 3*em, wxALIGN_CENTER_HORIZONTAL, wxDATAVIEW_COL_RESIZABLE));

// Editing column
AppendColumn(new wxDataViewColumn(_L("Editing"),
    new BitmapIconRenderer(wxDATAVIEW_CELL_INERT,
                           wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL),
    colEditing, 3*em, wxALIGN_CENTER_HORIZONTAL, wxDATAVIEW_COL_RESIZABLE));
```

`AppendBitmapColumn(label, model_col, mode, width, align, flags)` is just sugar
for "make a `wxDataViewColumn` with a stock `wxDataViewBitmapRenderer` and append
it." We spell it out to pass **our** renderer instead. Everything the sugar set is
still set (title, model column, `3*em` width, column alignment,
`wxDATAVIEW_COL_RESIZABLE`). Two deliberate differences:

1. **Renderer:** `new BitmapIconRenderer(...)` — the whole fix.
2. **Renderer alignment:** `wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL`.
   The control uses the renderer's alignment to position the rect handed to
   `Render`. The stock path centred vertically; our custom renderer must ask
   explicitly, otherwise the icon top‑aligns and rides the row divider (a bug we
   saw and fixed with the `_VERTICAL` flag).

`wxDATAVIEW_CELL_INERT` = not click‑to‑edit, matching the original behaviour
(clicks on these icons are handled by the list's mouse handlers, not cell
editing). The **Name** (`BitmapTextRenderer`) and **Extruder**
(`BitmapChoiceRenderer`) columns are untouched.

---

## 5. Design decisions & rejected alternatives

- **Content hash over pointer identity** — `GetBitmapFor()` may return a fresh
  `wxBitmap` per call (esp. at a second monitor's DPI); a pointer key would churn,
  grow the cache unbounded, and OOM.
- **Split `find_composited` / `make_composited`** — keeps the hot path (a cache
  hit in `Render`/`SetValue`) down to two integers + a map lookup; the expensive
  blend is isolated to the miss path.
- **Class‑level `static s_cache`** — explicit shared state, visible in the class,
  shared by both column renderers.
- **`wxMemoryDC` + `DrawBitmap(..., true)` for the off‑screen blend** — lets the
  platform composite alpha correctly regardless of the source bitmap's internal
  format (a raw image paste can silently fall back to a plain copy for DDBs
  without an explicit alpha plane). The `true` (`useMask`) cost is irrelevant
  here — it's the once‑per‑entry path, not the scroll path.
- **Per‑cell `GetPixel` for selected rows** — the selection colour isn't in the
  `state` flags and varies per row (plain vs active/focused); sampling the
  already‑painted DC is the only portable way to get the exact colour.

Earlier dead ends (for context): caching `GetSize()` text measurement (not the
bottleneck); disabling ellipsization (no effect); a 1‑bit mask for transparency
(fast but hard‑edges the icons); off‑screen `wxRendererNative::DrawItemSelectionRect`
to learn the selection colour (wrong colour — no window theme context); sampling
the selection colour once globally (misses the active‑row shade).

---

## 6. Known limitations

- **Per‑cell `GetPixel` on selected rows.** Cheap and bounded, but per selected
  cell per paint. A theme painting the selection as a gradient (many colours)
  could make the cache churn; the `> 1024` cap keeps it safe (recompute, never
  crash) but slower in that exotic case.
- **Extruder column not optimized.** `BitmapChoiceRenderer` still does a naive
  `DrawBitmap`. It wasn't a bottleneck (hidden on single‑extruder setups). If it
  ever lags on a multi‑extruder printer, port the same find/make composite trick
  into its `Render`.
- **Doesn't fix the underlying wx inefficiencies.** The slow alpha `DrawBitmap`
  and the repaint‑all‑visible‑cells behaviour still exist in wx; we just make each
  draw cheap enough that they no longer matter. Many more icon columns could
  resurface the repaint amplifier.

---

## 7. How to verify

1. Build (`cmake --build . --config RelWithDebInfo --parallel 24` from `build/`).
2. Load a project with many parts; scroll the object list — smooth on 2K and 4K,
   with no/several/many rows selected.
3. Selected rows (including the active/focused one) — icon backgrounds match the
   row highlight, no mismatched box, in dark and light themes.
4. Icons crisp (anti‑aliased), centred, correct per cell when dragging the window
   between monitors of different DPI.
5. No crash on full‑screen 4K scrolling.
```
