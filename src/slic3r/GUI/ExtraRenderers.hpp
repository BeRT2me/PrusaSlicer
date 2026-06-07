///|/ Copyright (c) Prusa Research 2020 - 2021 Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GUI_ExtraRenderers_hpp_
#define slic3r_GUI_ExtraRenderers_hpp_

#include <functional>
#include <map>

#include <wx/dataview.h>

#if wxUSE_MARKUP && wxCHECK_VERSION(3, 1, 1)
    #define SUPPORTS_MARKUP
#endif

// ----------------------------------------------------------------------------
// DataViewBitmapText: helper class used by BitmapTextRenderer
// ----------------------------------------------------------------------------

class DataViewBitmapText : public wxObject
{
public:
    DataViewBitmapText( const wxString &text = wxEmptyString,
                        const wxBitmap& bmp = wxNullBitmap) :
        m_text(text),
        m_bmp(bmp)
    { }

    DataViewBitmapText(const DataViewBitmapText &other)
        : wxObject(),
        m_text(other.m_text),
        m_bmp(other.m_bmp)
    { }

    void SetText(const wxString &text)      { m_text = text; }
    wxString GetText() const                { return m_text; }
    void SetBitmap(const wxBitmap &bmp)     { m_bmp = bmp; }
    const wxBitmap &GetBitmap() const       { return m_bmp; }

    bool IsSameAs(const DataViewBitmapText& other) const {
        return m_text == other.m_text && m_bmp.IsSameAs(other.m_bmp);
    }

    bool operator==(const DataViewBitmapText& other) const {
        return IsSameAs(other);
    }

    bool operator!=(const DataViewBitmapText& other) const {
        return !IsSameAs(other);
    }

private:
    wxString    m_text;
    wxBitmap    m_bmp;

    wxDECLARE_DYNAMIC_CLASS(DataViewBitmapText);
};
DECLARE_VARIANT_OBJECT(DataViewBitmapText)

// ----------------------------------------------------------------------------
// BitmapTextRenderer
// ----------------------------------------------------------------------------
#if ENABLE_NONCUSTOM_DATA_VIEW_RENDERING
class BitmapTextRenderer : public wxDataViewRenderer
#else
class BitmapTextRenderer : public wxDataViewCustomRenderer
#endif //ENABLE_NONCUSTOM_DATA_VIEW_RENDERING
{
public:
    BitmapTextRenderer(bool use_markup = false,
        wxDataViewCellMode mode =
#ifdef __WXOSX__
        wxDATAVIEW_CELL_INERT
#else
        wxDATAVIEW_CELL_EDITABLE
#endif

        , int align = wxDVR_DEFAULT_ALIGNMENT
#if ENABLE_NONCUSTOM_DATA_VIEW_RENDERING
    );
#else
    ) :
    wxDataViewCustomRenderer(wxT("DataViewBitmapText"), mode, align)
    {
        EnableMarkup(use_markup);
    }
#endif //ENABLE_NONCUSTOM_DATA_VIEW_RENDERING

    ~BitmapTextRenderer();

    void EnableMarkup(bool enable = true);

    bool SetValue(const wxVariant& value) override;
    bool GetValue(wxVariant& value) const override;
#if ENABLE_NONCUSTOM_DATA_VIEW_RENDERING && wxUSE_ACCESSIBILITY
    virtual wxString GetAccessibleDescription() const override;
#endif // wxUSE_ACCESSIBILITY && ENABLE_NONCUSTOM_DATA_VIEW_RENDERING

    virtual bool Render(wxRect cell, wxDC* dc, int state) override;
    virtual wxSize GetSize() const override;

    bool        HasEditorCtrl() const override
    {
#ifdef __WXOSX__
        return false;
#else
        return true;
#endif
    }
    wxWindow*   CreateEditorCtrl(wxWindow* parent, wxRect labelRect, const wxVariant& value) override;
    bool        GetValueFromEditorCtrl(wxWindow* ctrl, wxVariant& value) override;
    bool        WasCanceled() const { return m_was_unusable_symbol; }

    void        set_can_create_editor_ctrl_function(std::function<bool()> can_create_fn) { can_create_editor_ctrl = can_create_fn; }

private:
    DataViewBitmapText  m_value;
    bool                m_was_unusable_symbol{ false };

    std::function<bool()>    can_create_editor_ctrl { nullptr };

#ifdef SUPPORTS_MARKUP
    #ifdef wxHAS_GENERIC_DATAVIEWCTRL
    class wxItemMarkupText* m_markupText { nullptr };;
    #else
    bool is_markupText {false};
    #endif
#endif // SUPPORTS_MARKUP
};


// ----------------------------------------------------------------------------
// BitmapChoiceRenderer
// ----------------------------------------------------------------------------

class BitmapChoiceRenderer : public wxDataViewCustomRenderer
{
public:
    BitmapChoiceRenderer(wxDataViewCellMode mode =
#ifdef __WXOSX__
        wxDATAVIEW_CELL_INERT
#else
        wxDATAVIEW_CELL_EDITABLE
#endif
        , int align = wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL
    ) : wxDataViewCustomRenderer(wxT("DataViewBitmapText"), mode, align) {}

    bool SetValue(const wxVariant& value) override;
    bool GetValue(wxVariant& value) const override;

    virtual bool Render(wxRect cell, wxDC* dc, int state) override;
    virtual wxSize GetSize() const override;

    bool        HasEditorCtrl() const override { return true; }
    wxWindow*   CreateEditorCtrl(wxWindow* parent, wxRect labelRect, const wxVariant& value) override;
    bool        GetValueFromEditorCtrl(wxWindow* ctrl, wxVariant& value) override;

    void        set_can_create_editor_ctrl_function(std::function<bool()> can_create_fn) { can_create_editor_ctrl = can_create_fn; }
    void        set_default_extruder_idx(std::function<int()> default_extruder_idx_fn)   { get_default_extruder_idx = default_extruder_idx_fn; }

private:
    DataViewBitmapText      m_value;
    std::function<bool()>   can_create_editor_ctrl  { nullptr };
    std::function<int()>    get_default_extruder_idx{ nullptr };
};



// ----------------------------------------------------------------------------
// TextRenderer
// ----------------------------------------------------------------------------

class TextRenderer : public wxDataViewCustomRenderer
{
public:
    TextRenderer(wxDataViewCellMode mode = wxDATAVIEW_CELL_INERT
        , int align = wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL
    ) : wxDataViewCustomRenderer(wxT("string"), mode, align) {}

    bool SetValue(const wxVariant& value) override;
    bool GetValue(wxVariant& value) const override;

    virtual bool Render(wxRect cell, wxDC* dc, int state) override;
    virtual wxSize GetSize() const override;

    bool        HasEditorCtrl() const override { return false; }

private:
    wxString    m_value;
};



// ----------------------------------------------------------------------------
// BitmapIconRenderer
// ----------------------------------------------------------------------------

// Icon renderer for the Print/Editing columns. Drawing a 32-bit alpha wxBitmap
// onto the object list's buffered paint DC costs ~330us/cell on wxMSW (live
// AlphaBlend), so scrolling a long list with these icon columns crawls
// (GH #15177, #11557). This renderer pays that cost once per distinct icon by
// pre-compositing it onto the row background -- an opaque bitmap that then blits in
// ~10us. Selected rows are composited onto the theme's selection colour (read from
// the painted DC, per row so the active row matches too). Composites are cached by a
// content fingerprint, which stays stable even when GetBitmapFor() hands back fresh
// bitmaps at a second monitor's DPI (a pointer key would churn and grow the cache
// without bound -> OOM).
class BitmapIconRenderer : public wxDataViewCustomRenderer
{
    wxBitmap m_src;    // the original alpha icon (fallback + cache-miss blend source)
    wxBitmap m_normal; // m_src pre-composited onto the current normal row background
    wxUint64 m_fp = 0; // content fingerprint of m_src (cache key)

    static std::map<std::pair<wxUint64, wxUint32>, wxBitmap> s_cache;

    static wxUint64       fingerprint(const wxBitmap& src);
    static const wxBitmap* find_composited(wxUint64 fp, wxUint32 rgb);
    static wxBitmap        make_composited(wxUint64 fp, const wxBitmap& src, const wxColour& bg);

public:
    BitmapIconRenderer(wxDataViewCellMode mode, int align)
        : wxDataViewCustomRenderer(wxS("wxBitmap"), mode, align) {}

    bool SetValue(const wxVariant& value) override;
    bool GetValue(wxVariant& WXUNUSED(value)) const override { return true; }
    wxSize GetSize() const override;
    bool Render(wxRect cell, wxDC* dc, int state) override;
};


#endif // slic3r_GUI_ExtraRenderers_hpp_
