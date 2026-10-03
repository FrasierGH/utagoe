// Thin Win32 layer standing in for the VCL controls of the original forms.
// Every control is created at the Left/Top/Width/Height of the original DFM, scaled the way
// TCustomForm.ReadState scales a form whose font is taller than the DFM's TextHeight.
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <map>
#include <string>
#include <vector>

namespace ui {

extern HINSTANCE hinst;

void init();                                       // common controls, window classes
std::wstring format(const wchar_t* fmt, ...);
std::wstring window_text(HWND h);

// TBevel (Style = bsLowered)
enum BevelShape { BEVEL_BOX, BEVEL_TOP, BEVEL_BOTTOM, BEVEL_LEFT };
struct Bevel { int x, y, w, h; BevelShape shape; };
void paint_bevel(HDC dc, const Bevel& b);

// Glyph strip (VCL NumGlyphs = 2: normal + disabled) for a TBitBtn
struct Glyph {
    HBITMAP normal = nullptr, disabled = nullptr;
    COLORREF key_normal = 0, key_disabled = 0;
    int w = 0, h = 0;
};
Glyph load_glyph(const wchar_t* strip, const wchar_t* disabled_res = nullptr);

// TBitBtn: native themed button with the glyph and caption drawn at VCL's layout
struct BitBtnInfo {
    std::wstring caption;
    Glyph glyph;
    int margin = -1, spacing = 4;
    HFONT font = nullptr;
};

class Form {
public:
    virtual ~Form() = default;
    HWND hwnd = nullptr;
    HFONT font = nullptr;

    // VCL form scaling: the DFMs say TextHeight = 12, so a form whose font measures differently
    // (Tahoma in the en_US build) has every control scaled by font_height(font) / 12.
    int scale_m = 12, scale_d = 12;
    bool scaled() const { return scale_m != scale_d; }
    int sx(int v) const { return MulDiv(v, scale_m, scale_d); }
    void scale_rect(int& x, int& y, int& w, int& h) const;  // TControl.ChangeScale
    // Controls created between enter() and leave() are children of the TGroupBox at (x, y, w, h):
    // VCL scales their coordinates relative to the group and clips them at its edges.
    // Coordinates stay in the parent's DFM units (group position + DFM Left/Top).
    void enter(int x, int y, int w, int h) { box_ = {x, y, w, h, true}; }
    void leave() { box_.on = false; }
    int px(int x) const { return box_.on ? sx(box_.x) + sx(x - box_.x) : sx(x); }  // scaled position
    int py(int y) const { return box_.on ? sx(box_.y) + sx(y - box_.y) : sx(y); }
    void paint_bevels(HDC dc) const;

    // Creates the top-level window with the given client size (DFM units), centred on the screen.
    bool create(const wchar_t* title, int client_w, int client_h, DWORD style, DWORD ex_style, HWND owner);
    // ShowModal: disables the owner and runs a nested message loop until closed.
    void show_modal(HWND owner);
    void show_focus_cues();
    void close();
    bool alive() const { return hwnd != nullptr; }

    // control factories; `parent` may be the form or a page window
    HWND label(HWND parent, int x, int y, const std::wstring& text, HFONT f = nullptr, int w = 0, int h = 0,
               DWORD align = SS_LEFT);
    // TStaticText: like a TLabel, but AutoSize adds a 2-pixel border on each side
    HWND static_text(HWND parent, int x, int y, const std::wstring& text, int w = 0, int h = 0, DWORD align = SS_LEFT);
    void set_static_text(HWND st, const std::wstring& text);  // SetText + AdjustBounds (keeps Left)
    HWND edit(HWND parent, int id, int x, int y, int w, int h, int max_len);
    // TUpDown with Associate = edit, AlignButton = udRight: flush against the edit's right edge
    HWND updown(HWND parent, int id, HWND buddy, int x, int w, int max);
    HWND bitbtn(HWND parent, int id, int x, int y, int w, int h, const std::wstring& caption, const Glyph& g,
                int margin = -1, int spacing = 4, DWORD style = 0);
    HWND button(HWND parent, int id, int x, int y, int w, int h, const std::wstring& caption);
    HWND check(HWND parent, int id, int x, int y, int w, int h, const std::wstring& caption);
    HWND radio(HWND parent, int id, int x, int y, int w, int h, const std::wstring& caption, bool first);
    HWND group(HWND parent, int x, int y, int w, int h, const std::wstring& caption);
    // TRadioGroup: group box + radio buttons spread like VCL's ArrangeButtons
    // clip_right: x where a control stacked above the group hides the buttons (VCL z-order)
    std::vector<HWND> radio_group(HWND parent, int first_id, int x, int y, int w, int h, const wchar_t* const* caption_items,
                                  int count, int clip_right = 0);
    HWND trackbar(HWND parent, int id, int x, int y, int w, int h, int max, int thumb, bool both);
    HWND combo(HWND parent, int id, int x, int y, int w, const wchar_t* const* items, int count, int max_len);
    HWND page(HWND parent, int x, int y, int w, int h);  // pixel coordinates (from TCM_ADJUSTRECT)
    // TMediaPlayer: a strip of `count` 29-pixel buttons sharing their frames; a click sends
    // WM_COMMAND(first_id + i) to the form
    HWND media_player(HWND parent, int first_id, int x, int y, int h, const Glyph* glyphs, int count);
    void set_caption(HWND bitbtn, const std::wstring& caption);
    void set_bitbtn_font(HWND bitbtn, HFONT f);

    std::vector<Bevel> bevels;

protected:
    virtual LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT default_handle(UINT msg, WPARAM wp, LPARAM lp);
    // a control at DFM coordinates (scaled)
    HWND child(const wchar_t* cls, const std::wstring& text, DWORD style, DWORD ex, HWND parent, int id, int x, int y,
               int w, int h, HFONT f = nullptr);
    // a control at final pixel coordinates
    HWND create_child(const wchar_t* cls, const std::wstring& text, DWORD style, DWORD ex, HWND parent, int id, int x,
                      int y, int w, int h, HFONT f = nullptr);
    std::map<HWND, BitBtnInfo> bitbtns_;

public:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

private:
    struct Box { int x, y, w, h; bool on; } box_ = {};
    bool custom_draw(NMCUSTOMDRAW* cd, LRESULT* result);
    bool modal_ = false;
    HWND owner_ = nullptr;
};

void message_loop_step(MSG& m);  // IsDialogMessage + dispatch for the active form
int text_width(HFONT f, const std::wstring& s);
int font_height(HFONT f);  // TEXTMETRIC.tmHeight (VCL's Canvas.TextHeight)
// Font.Size := MulDiv(Font.Size, m, d) for a control with ParentFont = False; returns the new Font.Height
int scaled_font_height(int height, int m, int d);

}  // namespace ui
