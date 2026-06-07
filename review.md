# Object-list scroll fix — code review walkthrough

## The problem in one sentence

Every time you scroll the object list, wxWidgets redraws every visible cell. The "Print" (eye) and "Editing" (gear) icon columns used a built-in renderer that called Windows' `AlphaBlend` to draw each icon — a slow per-pixel operation that took ~330µs per cell. Scroll a tall list and you're calling that hundreds of times per frame.

---

## Change 1 — New imports

**`ExtraRenderers.cpp`** gains two new includes:

```cpp
#include <boost/functional/hash.hpp>
#include <wx/dcmemory.h>
```

- `<boost/functional/hash.hpp>` — for `boost::hash_combine` and `boost::hash_range`, used to build a content hash over the icon's pixels.
- `<wx/dcmemory.h>` — for `wxMemoryDC`, an off-screen drawing canvas used to composite icons onto a background bitmap.

**`ExtraRenderers.hpp`** gains `#include <map>` for the `std::map` cache declared in the class.

**`GUI_ObjectList.cpp`** gains `#include "ExtraRenderers.hpp"` to pick up the new class.

---

## Change 2 — The new class

```cpp
class BitmapIconRenderer : public wxDataViewCustomRenderer
```

Declared in `ExtraRenderers.hpp`, defined in `ExtraRenderers.cpp` — alongside the other custom renderers (`BitmapTextRenderer`, `BitmapChoiceRenderer`, `TextRenderer`) that follow the same pattern.

In Python terms: `class BitmapIconRenderer(wxDataViewCustomRenderer)` — a subclass. `wxDataViewCustomRenderer` is wx's base class for custom cell drawing. The framework calls your methods (`SetValue`, `Render`, etc.) for each cell; you override them. One instance is **reused for every row** in its column.

### Instance members

```cpp
wxBitmap m_src;    // the original alpha icon (fallback + cache-miss blend source)
wxBitmap m_normal; // m_src pre-composited onto the current normal row background
wxUint64 m_fp = 0; // content fingerprint of m_src (cache key)
```

`m_` is a naming convention for member variables (like `self._x` in Python). These three always hold "the icon for whichever cell is currently being processed."

`wxBitmap` is reference-counted — copying one is a cheap pointer bump, not a pixel copy. Both `m_src` and `m_normal` are just refcounted handles; the actual pixel data lives on the heap and is shared.

### Class-level static member

```cpp
static std::map<std::pair<wxUint64, wxUint32>, wxBitmap> s_cache;
```

The composite cache — shared across all `BitmapIconRenderer` instances (both the Print and Editing columns share one cache). Declared in the class, defined once after the class body:

```cpp
std::map<std::pair<wxUint64, wxUint32>, wxBitmap> BitmapIconRenderer::s_cache;
```

The cache key is a pair of `(icon fingerprint, packed background colour)`. In Python terms:

```python
s_cache: dict[tuple[int, int], Bitmap] = {}
```

`std::map` requires keys to support `operator<` (less-than) for sorting — it's a sorted tree internally. `wxColour` doesn't implement `operator<`, so colours can't be used directly as keys. `bg.GetRGB()` packs R, G, B into a single `uint32` (`0x00BBGGRR` in wx convention), which has `<` defined automatically.

---

### `fingerprint()` — a content hash

```cpp
static wxUint64 fingerprint(const wxBitmap& src) {
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

`static` means it belongs to the class, not any instance — like `@staticmethod` in Python.

Takes a `wxBitmap` and converts it to a `wxImage` internally to read raw pixel bytes. This hashes the icon's dimensions and pixel contents. In Python pseudocode:

```python
def fingerprint(src):
    img = src.to_image()
    h = 0
    h = hash_combine(h, img.width)
    h = hash_combine(h, img.height)
    h = hash_range(h, img.rgb_bytes)   # w * h * 3 bytes
    h = hash_range(h, img.alpha_bytes) # w * h bytes
    return h
```

`wxImage` uses a **planar** layout — RGB and alpha are stored in two separate flat memory blocks, not interleaved as `RGBA`. That's why `GetData()` and `GetAlpha()` return two separate pointers and are hashed in two separate calls.

`d` is a pointer to the first byte of the RGB block. `d + npixels * 3` is a pointer to one byte past the end — C++ pointer arithmetic, where adding `n` moves the pointer forward `n` elements. `boost::hash_range(h, begin, end)` walks every byte between those two addresses and folds them into `h` — it reads the *contents* at those addresses, not the addresses themselves.

`npixels = size_t(w) * ht` is pre-calculated and reused for both planes. The `size_t` cast ensures the multiplication doesn't overflow (two `int`s multiplied together can overflow; `size_t` is 64-bit on a 64-bit system).

Width and height are included separately because the flat pixel array doesn't encode its own dimensions — a 2×4 and 4×2 image with the same bytes would otherwise hash identically.

**Why a content hash instead of a pointer?** `GetBitmapFor()` can return a brand-new `wxBitmap` object on each call (especially on a 4K monitor at a different DPI). A `wxBitmap`'s `m_refData` pointer — its internal identity — would be different every call even for identical pixels, so a pointer-based cache key would miss every time, grow unboundedly, and eventually OOM. A content hash is stable: same pixels → same hash, regardless of which object carries them.

---

### `find_composited()` — cache lookup

```cpp
static const wxBitmap* find_composited(wxUint64 fp, wxUint32 rgb) {
    auto it = s_cache.find({fp, rgb});
    return it != s_cache.end() ? &it->second : nullptr;
}
```

Looks up `(fp, rgb)` in the cache and returns a pointer to the cached bitmap, or `nullptr` on a miss. No image data needed — just the two integers that form the key.

`s_cache.find()` returns an iterator — a cursor pointing at the found entry, or the sentinel `s_cache.end()` if not found. `it->second` is the value (the bitmap); `it->first` would be the key. In Python:

```python
def find_composited(fp, rgb):
    return s_cache.get((fp, rgb))  # None on miss
```

Keeping the lookup separate from the blend means callers on the hot path (`Render`) can check the cache with just two integers, paying nothing if they hit.

---

### `make_composited()` — blend and cache insert

```cpp
static wxBitmap make_composited(wxUint64 fp, const wxBitmap& src, const wxColour& bg) {
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

Called only after a `find_composited` miss. Blends `src` (alpha icon) onto solid colour `bg`, stores the result in the cache, and returns it.

`if (s_cache.size() > 1024) s_cache.clear()` — a safety cap. In practice the cache holds ~(a few icons × a few colours) = tens of entries. A pathological theme painting the selection as a gradient could produce many distinct sampled colours; the cap guarantees the cache never grows unbounded. Clearing forces a recompute but doesn't affect correctness.

**The blend:**
- `wxBitmap out(w, h, 24)` — a new 24-bit (no alpha) output bitmap.
- `wxMemoryDC mdc(out)` — an off-screen DC that draws directly into `out`. Like opening a canvas in memory.
- `mdc.SetBackground(wxBrush(bg)); mdc.Clear()` — fills `out` with the background colour.
- `mdc.DrawBitmap(src, 0, 0, true)` — alpha-composites `src` onto the background. The `true` argument means "use alpha/mask". On Windows this calls `AlphaBlend` once — the expensive operation — but it's paid only on a cache miss, never on scroll.
- The `mdc` destructor runs at the closing `}`, flushing to `out` and releasing the DC.
- `wxBitmap(out, 24)` is not needed here — `out` is already 24-bit. A 24-bit bitmap is always drawn with a fast opaque blit; the `AlphaBlend` slow path only triggers for 32-bit bitmaps with an alpha channel.

`s_cache.emplace(key, std::move(out)).first->second` — inserts by moving `out` into the map (no copy), then returns a reference to the stored bitmap. (`std::map` keeps element references stable across later inserts.)

In Python terms:

```python
def make_composited(fp, src, bg):
    if len(s_cache) > 1024:
        s_cache.clear()
    canvas = Image.new("RGB", src.size, bg)       # solid background, no alpha
    canvas.paste(src, mask=src.split()[3])         # alpha-blend src on top
    s_cache[(fp, bg.get_rgb())] = canvas
    return canvas
```

---

### Constructor

```cpp
BitmapIconRenderer(wxDataViewCellMode mode, int align)
    : wxDataViewCustomRenderer(wxS("wxBitmap"), mode, align) {}
```

`: wxDataViewCustomRenderer(...)` calls the parent constructor — like `super().__init__(...)` in Python. The `"wxBitmap"` string tells the framework which variant type this renderer handles. If it doesn't match what the model pushes, the framework silently skips the cell.

---

### `SetValue()` — called once per cell, before painting

```cpp
bool SetValue(const wxVariant& value) override {
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

`wxVariant` is wx's dynamically-typed value (like Python's `Any`). `src << value` extracts the bitmap. The type check against `"wxBitmap"` is there because `SetValue` is a virtual override whose signature is fixed by the base class — the parameter must be `const wxVariant&` regardless of what the model actually pushes. In practice both icon columns always push a `wxBitmap` (via `GetBitmapFor()`), so the `wxBitmapBundle` branch that a generic renderer might include is not needed here.

`fingerprint(src)` does the `ConvertToImage()` internally — `SetValue` never needs to hold a `wxImage` directly. The fingerprint and the normal composite are both computed from `src` (the `wxBitmap`) before `Render` is ever called, so by paint time `m_normal` is already a cheap opaque bitmap ready to blit.

`return true` = "value accepted" — the wx API contract for this method.

---

### `GetValue()` — required but unused

```cpp
bool GetValue(wxVariant& WXUNUSED(value)) const override { return true; }
```

Like an abstract method you're forced to implement but don't need. Used for in-place cell editing, which these icon columns don't support. `WXUNUSED(value)` suppresses the compiler warning about the unused parameter.

---

### `GetSize()` — cell content dimensions

```cpp
wxSize GetSize() const override {
    if (m_normal.IsOk())
        return m_normal.GetSize();
    const int s = wxGetApp().em_unit();
    return wxSize(s, s);
}
```

Returns the icon's pixel size so the framework can lay out the cell. Falls back to `em_unit()` — PrusaSlicer's DPI-scaled text unit — if there's no icon.

---

### `Render()` — the hot path, called on every scroll repaint

```cpp
bool Render(wxRect cell, wxDC* dc, int state) override {
    if (!m_normal.IsOk())
        return true;

    wxBitmap bmp = m_normal; // non-selected: composite onto the normal background
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

`state & wxDATAVIEW_CELL_SELECTED` — bitwise AND, same as Python/JS. Checks if the "selected" bit is set in the state flags integer.

**Non-selected rows:** `bmp = m_normal` — the opaque composite already built in `SetValue`. Just a cheap reference-counted copy. This is the common case and costs almost nothing.

**Selected rows** need a different background colour. Two problems make it non-trivial:
1. The selection colour is theme-drawn and can't be hard-coded portably.
2. The active/focused row is often a different shade from other selected rows, and that distinction isn't conveyed by the `state` flags.

The solution: read the colour the control already painted into this cell. The control paints row backgrounds *before* calling the cell renderer, so the DC already has the real selection colour at this cell's coordinates.

```python
bg_color = dc.get_pixel(cell.x + 1, cell.y + cell.height // 2)
```

Then check the cache — on a hit (the common case after the first paint) nothing else is needed. On a miss, `make_composited` is called with `m_src` directly. Cache misses for selected rows are rare — typically one per distinct selection-highlight shade, ever.

`GetPixel` can fail on some platforms (GTK if no Cairo context is available). The fallback `bmp = m_src` draws the original alpha icon directly — correct result, just the slow `AlphaBlend` path, for that one cell.

**Draw, centred:**
```cpp
dc->DrawBitmap(bmp, cell.x + (cell.width - sz.x) / 2,
                    cell.y + (cell.height - sz.y) / 2, false);
```
`(cell.width - sz.x) / 2` is the horizontal padding — the same math as CSS `margin: 0 auto`. `false` = no mask needed, the bitmap is opaque.

---

## Change 3 — Wiring it in

**Before:**
```cpp
AppendBitmapColumn(" ", colPrint, wxDATAVIEW_CELL_INERT, 3*em,
    wxALIGN_CENTER_HORIZONTAL, wxDATAVIEW_COL_RESIZABLE);
```

**After:**
```cpp
AppendColumn(new wxDataViewColumn(" ",
    new BitmapIconRenderer(wxDATAVIEW_CELL_INERT,
                           wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL),
    colPrint, 3*em, wxALIGN_CENTER_HORIZONTAL, wxDATAVIEW_COL_RESIZABLE));
```

`AppendBitmapColumn` was a convenience wrapper that internally created a stock `wxDataViewBitmapRenderer` — the slow one. Now we spell it out manually to pass `BitmapIconRenderer` instead. Everything else (column title, width, alignment, flags) stays the same.

The extra `wxALIGN_CENTER_VERTICAL` in the renderer's alignment is required — the stock renderer centred vertically automatically; the custom renderer must ask for it explicitly, otherwise icons pin to the top of the row.

The same change is applied to the Editing column.

---

## Summary

The fix is a **memoization pattern** — the same idea as Python's `@functools.lru_cache`. The expensive operation (`AlphaBlend` to draw a translucent icon) gets cached by `(icon_content_hash, background_color)`, so it runs once per unique combination rather than once per cell per scroll event.

Key design decisions:
- **Content hash over pointer identity** — `GetBitmapFor()` may return a fresh `wxBitmap` allocation per call (especially at a second monitor's DPI); a pointer key would churn and OOM.
- **Split lookup from blend** — `find_composited` checks the cache with just two integers; `make_composited` is only called on a miss.
- **`wxMemoryDC` for compositing** — `wxImage::Paste(wxIMAGE_ALPHA_BLEND_COMPOSE)` silently falls back to a raw pixel memcpy when the source bitmap doesn't carry an explicit alpha plane (which can happen with GDI DDBs on Windows). `wxMemoryDC` + `DrawBitmap(..., true)` lets the platform handle alpha correctly regardless of the internal bitmap format.
- **Class-level static cache** — explicit shared state, visible in the class definition, shared across both column renderer instances.
- **Per-cell `GetPixel` for selected rows** — the selection highlight colour isn't available from state flags and varies per row (plain selected vs active/focused); sampling the already-painted DC pixel is the only portable way to get the exact colour.
