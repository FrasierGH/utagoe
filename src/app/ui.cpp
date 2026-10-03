#include "ui.hpp"

#include <uxtheme.h>
#include <vssym32.h>

#include <cstdarg>
#include <cwchar>

namespace ui {

HINSTANCE hinst;

namespace {

const wchar_t* FORM_CLASS = L"UtagoeForm";
const wchar_t* PAGE_CLASS = L"UtagoePage";
const wchar_t* MEDIA_CLASS = L"UtagoeMediaPlayer";

// Tab sheet background: the themed tab body (TTabSheet paints it under XP themes), kept as a
// pattern brush so labels, buttons and trackbars on the sheet can fill with the same texture.
HBRUSH page_brush(HWND page) {
    HBRUSH br = (HBRUSH)GetWindowLongPtrW(page, GWLP_USERDATA);
    if (br) return br;
    RECT rc;
    GetClientRect(page, &rc);
    HTHEME th = OpenThemeData(page, L"TAB");
    if (!th || rc.right <= 0 || rc.bottom <= 0) {
        if (th) CloseThemeData(th);
        return GetSysColorBrush(COLOR_BTNFACE);
    }
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    FillRect(mem, &rc, GetSysColorBrush(COLOR_BTNFACE));
    DrawThemeBackground(th, mem, TABP_BODY, 0, &rc, nullptr);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    CloseThemeData(th);
    br = CreatePatternBrush(bmp);
    DeleteObject(bmp);
    SetWindowLongPtrW(page, GWLP_USERDATA, (LONG_PTR)br);
    return br;
}

void drop_page_brush(HWND page) {
    if (HBRUSH br = (HBRUSH)GetWindowLongPtrW(page, GWLP_USERDATA)) DeleteObject(br);
    SetWindowLongPtrW(page, GWLP_USERDATA, 0);
}

// Container used for tab sheets: forwards notifications to the form.
LRESULT CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: case WM_NOTIFY: case WM_HSCROLL: case WM_VSCROLL: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        return SendMessageW(GetAncestor(h, GA_ROOT), msg, wp, lp);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        RECT r;
        GetWindowRect((HWND)lp, &r);
        MapWindowPoints(nullptr, h, (POINT*)&r, 2);
        SetBrushOrgEx(dc, -r.left, -r.top, nullptr);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)page_brush(h);
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        SetBrushOrgEx((HDC)wp, 0, 0, nullptr);
        FillRect((HDC)wp, &rc, page_brush(h));
        return 1;
    }
    case WM_SIZE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        drop_page_brush(h);
        InvalidateRect(h, nullptr, TRUE);
        break;
    case WM_NCDESTROY:
        drop_page_brush(h);
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

COLORREF pixel_key(HBITMAP bmp, int x, int y) {
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, bmp);
    COLORREF c = GetPixel(dc, x, y);
    SelectObject(dc, old);
    DeleteDC(dc);
    return c;
}

HBITMAP crop(HBITMAP src, int x, int w, int h) {
    HDC screen = GetDC(nullptr);
    HDC a = CreateCompatibleDC(screen), b = CreateCompatibleDC(screen);
    HBITMAP out = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ oa = SelectObject(a, src), ob = SelectObject(b, out);
    BitBlt(b, 0, 0, w, h, a, x, 0, SRCCOPY);
    SelectObject(a, oa);
    SelectObject(b, ob);
    DeleteDC(a);
    DeleteDC(b);
    ReleaseDC(nullptr, screen);
    return out;
}

Form* active_form = nullptr;

// ---------------------------------------------------------------- TMediaPlayer
const int MP_BUTTON_W = 29;  // MinBtnSize: the DFM width (-3) is below the minimum, so this is the size

struct MediaPlayer {
    const Glyph* glyphs;
    int count, first_id;
    int focused = 0;     // FFocusedButton
    int pressed = -1;    // button under a mouse or space-bar press
    bool lowered = false;
};

// DrawButtonFace(Canvas, R, 1, bsNew, False, IsDown, False)
void draw_button_face(HDC dc, RECT r, bool down) {
    HPEN frame = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_WINDOWFRAME));
    HPEN shadow = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
    HPEN light = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNHIGHLIGHT));
    HGDIOBJ op = SelectObject(dc, frame), ob = SelectObject(dc, GetSysColorBrush(COLOR_BTNFACE));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    InflateRect(&r, -1, -1);
    if (!down) {
        SelectObject(dc, shadow);
        POINT s[3] = {{r.left + 1, r.bottom - 1}, {r.right - 1, r.bottom - 1}, {r.right - 1, r.top}};
        Polyline(dc, s, 3);
        SelectObject(dc, light);
    } else {
        SelectObject(dc, shadow);
    }
    POINT t[3] = {{r.left, r.bottom - 1}, {r.left, r.top}, {r.right, r.top}};
    Polyline(dc, t, 3);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(frame);
    DeleteObject(shadow);
    DeleteObject(light);
}

int media_hit(MediaPlayer* m, HWND h, int x, int y) {
    RECT rc;
    GetClientRect(h, &rc);
    if (x < 0 || x >= rc.right || y < 0 || y >= rc.bottom) return -1;
    int i = x / (MP_BUTTON_W - 1);
    return i < m->count ? i : m->count - 1;
}

void media_click(MediaPlayer* m, HWND h, int i) {
    SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(m->first_id + i, BN_CLICKED), (LPARAM)h);
}

LRESULT CALLBACK media_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    MediaPlayer* m = (MediaPlayer*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        break;
    case WM_NCDESTROY:
        delete m;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
        bool enabled = IsWindowEnabled(h) != FALSE;
        for (int i = 0; i < m->count; i++) {
            int x = i * (MP_BUTTON_W - 1);
            RECT r = {x, 0, x + MP_BUTTON_W, rc.bottom};
            bool down = m->lowered && m->pressed == i;
            draw_button_face(dc, r, down);
            const Glyph& g = m->glyphs[i];
            HBITMAP bmp = enabled ? g.normal : g.disabled;
            if (bmp) {
                HDC mem = CreateCompatibleDC(dc);
                HGDIOBJ old = SelectObject(mem, bmp);
                int gx = x + (MP_BUTTON_W - g.w) / 2 + (down ? 1 : 0), gy = (rc.bottom - g.h) / 2 + (down ? 1 : 0);
                TransparentBlt(dc, gx, gy, g.w, g.h, mem, 0, 0, g.w, g.h, enabled ? g.key_normal : g.key_disabled);
                SelectObject(mem, old);
                DeleteDC(mem);
            }
            if (i == m->focused && GetFocus() == h) {
                InflateRect(&r, -3, -3);
                SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                DrawFocusRect(dc, &r);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN: {
        int i = media_hit(m, h, (short)LOWORD(lp), (short)HIWORD(lp));
        if (i < 0) break;
        SetFocus(h);
        SetCapture(h);
        m->focused = m->pressed = i;
        m->lowered = true;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (m->pressed >= 0 && GetCapture() == h) {
            bool over = media_hit(m, h, (short)LOWORD(lp), (short)HIWORD(lp)) == m->pressed;
            if (over != m->lowered) {
                m->lowered = over;
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (m->pressed >= 0 && GetCapture() == h) {
            int i = m->pressed;
            bool fire = m->lowered;
            m->pressed = -1;
            m->lowered = false;
            ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
            if (fire) media_click(m, h, i);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (m->pressed >= 0 && (HWND)lp != h) {
            m->pressed = -1;
            m->lowered = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_LEFT || wp == VK_UP) {
            if (m->focused > 0) m->focused--;
        } else if (wp == VK_RIGHT || wp == VK_DOWN) {
            if (m->focused < m->count - 1) m->focused++;
        } else if (wp == VK_SPACE) {
            m->pressed = m->focused;
            m->lowered = true;
        } else {
            break;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_KEYUP:
        if (wp == VK_SPACE && m->pressed >= 0) {
            int i = m->pressed;
            m->pressed = -1;
            m->lowered = false;
            InvalidateRect(h, nullptr, FALSE);
            media_click(m, h, i);
            return 0;
        }
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// TCustomGroupBox.Paint (themed): caption at (8, 0), frame from half the caption height down
LRESULT CALLBACK group_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg != WM_PAINT && msg != WM_PRINTCLIENT) return DefSubclassProc(h, msg, wp, lp);
    HTHEME th = OpenThemeData(h, L"BUTTON");
    if (!th) return DefSubclassProc(h, msg, wp, lp);
    PAINTSTRUCT ps;
    HDC dc = msg == WM_PAINT ? BeginPaint(h, &ps) : (HDC)wp;
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH bg = (HBRUSH)SendMessageW(GetParent(h), WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)h);
    FillRect(dc, &rc, bg ? bg : GetSysColorBrush(COLOR_BTNFACE));
    HGDIOBJ oldf = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
    std::wstring text = window_text(h);
    RECT cap = {0, 0, 0, 0};
    if (!text.empty()) {
        SIZE sz;
        GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
        cap = {8, 0, 8 + sz.cx, sz.cy};
    }
    RECT outer = rc;
    outer.top = (cap.bottom - cap.top) / 2;
    int state = IsWindowEnabled(h) ? GBS_NORMAL : GBS_DISABLED;
    int save = SaveDC(dc);
    ExcludeClipRect(dc, cap.left, cap.top, cap.right, cap.bottom);
    DrawThemeBackground(th, dc, BP_GROUPBOX, state, &outer, nullptr);
    RestoreDC(dc, save);
    if (!text.empty()) DrawThemeText(th, dc, BP_GROUPBOX, state, text.c_str(), (int)text.size(), DT_LEFT, 0, &cap);
    SelectObject(dc, oldf);
    CloseThemeData(th);
    if (msg == WM_PAINT) EndPaint(h, &ps);
    return 0;
}

}  // namespace

void init() {
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES |
                                                ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = Form::proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIconW(hinst, L"MAINICON");
    wc.lpszClassName = FORM_CLASS;
    RegisterClassExW(&wc);
    WNDCLASSEXW pc = {sizeof pc};
    pc.lpfnWndProc = page_proc;
    pc.hInstance = hinst;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    pc.lpszClassName = PAGE_CLASS;
    RegisterClassExW(&pc);
    WNDCLASSEXW mc = {sizeof mc};
    mc.lpfnWndProc = media_proc;
    mc.hInstance = hinst;
    mc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mc.lpszClassName = MEDIA_CLASS;
    RegisterClassExW(&mc);
}

std::wstring format(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 2048, fmt, ap);
    va_end(ap);
    return buf;
}

std::wstring window_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}

int text_width(HFONT f, const std::wstring& s) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    RECT r = {0, 0, 0, 0};
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return r.right;
}

int font_height(HFONT f) {
    TEXTMETRICW tm;
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return tm.tmHeight;
}

int scaled_font_height(int height, int m, int d) {
    int size = -MulDiv(height, 72, 96);  // TFont.Size at PixelsPerInch = 96
    return -MulDiv(MulDiv(size, m, d), 96, 72);
}

void paint_bevel(HDC dc, const Bevel& b) {
    HPEN shadow = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
    HPEN light = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNHIGHLIGHT));
    HGDIOBJ old = SelectObject(dc, shadow);
    auto line = [&](HPEN p, int x1, int y1, int x2, int y2) {
        SelectObject(dc, p);
        MoveToEx(dc, x1, y1, nullptr);
        LineTo(dc, x2, y2);
    };
    int x = b.x, y = b.y, r = b.x + b.w - 1, btm = b.y + b.h - 1;
    switch (b.shape) {
    case BEVEL_BOX:
        line(shadow, x, btm, x, y);
        line(shadow, x, y, r, y);
        line(light, r, y, r, btm);
        line(light, r, btm, x - 1, btm);
        break;
    case BEVEL_TOP:
        line(shadow, x, y, r + 1, y);
        line(light, x, y + 1, r + 1, y + 1);
        break;
    case BEVEL_BOTTOM:
        line(shadow, x, btm - 1, r + 1, btm - 1);
        line(light, x, btm, r + 1, btm);
        break;
    case BEVEL_LEFT:
        line(shadow, x, y, x, btm + 1);
        line(light, x + 1, y, x + 1, btm + 1);
        break;
    }
    SelectObject(dc, old);
    DeleteObject(shadow);
    DeleteObject(light);
}

Glyph load_glyph(const wchar_t* strip, const wchar_t* disabled_res) {
    Glyph g;
    HBITMAP src = (HBITMAP)LoadImageW(hinst, strip, IMAGE_BITMAP, 0, 0, LR_DEFAULTCOLOR);
    if (!src) return g;
    BITMAP bm;
    GetObjectW(src, sizeof bm, &bm);
    if (disabled_res) {  // TMediaPlayer: separate bitmaps
        g.w = bm.bmWidth, g.h = bm.bmHeight;
        g.normal = src;
        g.key_normal = pixel_key(src, 0, g.h - 1);
        g.disabled = (HBITMAP)LoadImageW(hinst, disabled_res, IMAGE_BITMAP, 0, 0, LR_DEFAULTCOLOR);
        g.key_disabled = g.disabled ? pixel_key(g.disabled, 0, g.h - 1) : g.key_normal;
        return g;
    }
    g.w = bm.bmWidth / 2, g.h = bm.bmHeight;  // VCL glyph strip: normal + disabled
    g.normal = crop(src, 0, g.w, g.h);
    g.disabled = crop(src, g.w, g.w, g.h);
    g.key_normal = g.key_disabled = pixel_key(src, 0, g.h - 1);  // bottom-left pixel is transparent
    DeleteObject(src);
    return g;
}

// ---------------------------------------------------------------- Form

LRESULT CALLBACK Form::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Form* f;
    if (msg == WM_NCCREATE) {
        f = (Form*)((CREATESTRUCTW*)lp)->lpCreateParams;
        f->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)f);
    } else {
        f = (Form*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    if (!f) return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_ACTIVATE) {
        if (LOWORD(wp) != WA_INACTIVE) active_form = f;
    }
    LRESULT r = f->handle(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        if (active_form == f) active_form = nullptr;
        f->hwnd = nullptr;
    }
    return r;
}

void Form::scale_rect(int& x, int& y, int& w, int& h) const {
    if (!box_.on) {
        int nx = sx(x), ny = sx(y);
        w = sx(x + w) - nx;
        h = sx(y + h) - ny;
        x = nx, y = ny;
        return;
    }
    int rx = x - box_.x, ry = y - box_.y;
    w = sx(rx + w) - sx(rx);
    h = sx(ry + h) - sx(ry);
    x = sx(box_.x) + sx(rx);
    y = sx(box_.y) + sx(ry);
    // a child window is clipped by the group box
    int right = sx(box_.x + box_.w), bottom = sx(box_.y + box_.h);
    if (x + w > right) w = right - x;
    if (y + h > bottom) h = bottom - y;
}

void Form::paint_bevels(HDC dc) const {
    for (Bevel b : bevels) {
        scale_rect(b.x, b.y, b.w, b.h);
        paint_bevel(dc, b);
    }
}

bool Form::create(const wchar_t* title, int cw, int ch, DWORD style, DWORD ex, HWND owner) {
    // TCustomForm.ReadState: if TextHeight <> GetTextHeight then ScaleControls(GetTextHeight, TextHeight)
    scale_m = font ? font_height(font) : 12;
    scale_d = 12;
    RECT r = {0, 0, sx(cw), sx(ch)};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    owner_ = owner;
    return CreateWindowExW(ex, FORM_CLASS, title, style, x, y, w, h, owner, nullptr, hinst, this) != nullptr;
}

void Form::show_modal(HWND owner) {
    modal_ = true;
    if (owner) EnableWindow(owner, FALSE);
    show_focus_cues();
    ShowWindow(hwnd, SW_SHOW);
    MSG m;
    bool quit = false;
    while (alive()) {
        if (GetMessageW(&m, nullptr, 0, 0) <= 0) {
            quit = true;
            break;
        }
        message_loop_step(m);
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    if (quit) PostQuitMessage((int)m.wParam);
}

void Form::show_focus_cues() {
    // VCL forms draw focus rectangles from the start, not only after keyboard use
    SendMessageW(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
}

void Form::close() {
    if (hwnd) {
        if (modal_ && owner_) EnableWindow(owner_, TRUE);  // re-enable first so focus returns to the owner
        DestroyWindow(hwnd);
    }
}

void message_loop_step(MSG& m) {
    if (active_form && active_form->hwnd && IsDialogMessageW(active_form->hwnd, &m)) return;
    TranslateMessage(&m);
    DispatchMessageW(&m);
}

HWND Form::child(const wchar_t* cls, const std::wstring& text, DWORD style, DWORD ex, HWND parent, int id, int x, int y,
                 int w, int h, HFONT f) {
    scale_rect(x, y, w, h);
    return create_child(cls, text, style, ex, parent, id, x, y, w, h, f);
}

HWND Form::create_child(const wchar_t* cls, const std::wstring& text, DWORD style, DWORD ex, HWND parent, int id, int x,
                        int y, int w, int h, HFONT f) {
    // every control except a radio button starts a new group for arrow-key navigation
    if (!(wcscmp(cls, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_AUTORADIOBUTTON)) style |= WS_GROUP;
    HWND c = CreateWindowExW(ex, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, hinst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : font), FALSE);
    if (wcscmp(cls, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_GROUPBOX) SetWindowSubclass(c, group_proc, 1, 0);
    if (wcscmp(cls, L"STATIC") == 0 && (style & SS_TYPEMASK) == SS_ETCHEDHORZ)  // it trims itself on creation
        SetWindowPos(c, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
    return c;
}

HWND Form::label(HWND parent, int x, int y, const std::wstring& text, HFONT f, int w, int h, DWORD align) {
    // AutoSize labels take their size from the text; the position is scaled
    HFONT ff = f ? f : font;
    int sw = w, sh = h;
    scale_rect(x, y, sw, sh);
    if (!w) sw = text_width(ff, text);
    if (!h) sh = font_height(ff);
    return create_child(L"STATIC", text, align | SS_NOPREFIX, 0, parent, -1, x, y, sw, sh, ff);
}

HWND Form::static_text(HWND parent, int x, int y, const std::wstring& text, int w, int h, DWORD align) {
    int sw = w, sh = h;
    scale_rect(x, y, sw, sh);
    if (!w) sw = text_width(font, text) + GetSystemMetrics(SM_CXBORDER) * 4;  // TCustomStaticText.AdjustBounds
    if (!h) sh = (text.empty() ? 0 : font_height(font)) + GetSystemMetrics(SM_CYBORDER) * 4;
    return create_child(L"STATIC", text, align | SS_NOPREFIX, 0, parent, -1, x, y, sw, sh);
}

void Form::set_static_text(HWND st, const std::wstring& text) {
    SetWindowTextW(st, text.c_str());
    RECT r;
    GetWindowRect(st, &r);
    MapWindowPoints(nullptr, GetParent(st), (POINT*)&r, 2);
    SetWindowPos(st, nullptr, 0, 0, text_width(font, text) + GetSystemMetrics(SM_CXBORDER) * 4, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

HWND Form::updown(HWND parent, int id, HWND buddy, int x, int w, int max) {
    RECT r;
    GetWindowRect(buddy, &r);
    MapWindowPoints(nullptr, parent, (POINT*)&r, 2);
    int y = box_.y, h = 0;
    scale_rect(x, y, w, h);
    HWND u = create_child(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, parent, id, r.right,
                          r.top, w, r.bottom - r.top);
    SendMessageW(u, UDM_SETBUDDY, (WPARAM)buddy, 0);
    SendMessageW(u, UDM_SETRANGE32, 0, max);
    SetWindowPos(u, nullptr, 0, 0, w, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);  // the control snaps to its own width
    return u;
}

HWND Form::edit(HWND parent, int id, int x, int y, int w, int h, int max_len) {
    scale_rect(x, y, w, h);
    h = font_height(font) + 8;  // TCustomEdit.AdjustHeight (AutoSize)
    HWND e = create_child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, parent, id, x, y, w, h);
    SendMessageW(e, EM_LIMITTEXT, max_len, 0);
    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);  // as the VCL edits end up
    return e;
}

HWND Form::bitbtn(HWND parent, int id, int x, int y, int w, int h, const std::wstring& caption, const Glyph& g,
                  int margin, int spacing, DWORD style) {
    HWND b = child(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON | style, 0, parent, id, x, y, w, h);
    BitBtnInfo info;
    info.caption = caption;
    info.glyph = g;
    info.margin = margin;
    info.spacing = spacing;
    info.font = font;
    bitbtns_[b] = info;
    return b;
}

void Form::set_bitbtn_font(HWND b, HFONT f) {
    auto it = bitbtns_.find(b);
    if (it != bitbtns_.end()) it->second.font = f;
    InvalidateRect(b, nullptr, TRUE);
}

void Form::set_caption(HWND b, const std::wstring& caption) {
    auto it = bitbtns_.find(b);
    if (it != bitbtns_.end()) it->second.caption = caption;
    InvalidateRect(b, nullptr, TRUE);
}

HWND Form::button(HWND parent, int id, int x, int y, int w, int h, const std::wstring& caption) {
    return child(L"BUTTON", caption, WS_TABSTOP | BS_PUSHBUTTON, 0, parent, id, x, y, w, h);
}

HWND Form::check(HWND parent, int id, int x, int y, int w, int h, const std::wstring& caption) {
    return child(L"BUTTON", caption, WS_TABSTOP | BS_AUTOCHECKBOX, 0, parent, id, x, y, w, h);
}

HWND Form::radio(HWND parent, int id, int x, int y, int w, int h, const std::wstring& caption, bool first) {
    return child(L"BUTTON", caption, BS_AUTORADIOBUTTON | (first ? WS_GROUP | WS_TABSTOP : 0), 0, parent, id, x, y, w, h);
}

HWND Form::group(HWND parent, int x, int y, int w, int h, const std::wstring& caption) {
    return child(L"BUTTON", caption, BS_GROUPBOX, 0, parent, -1, x, y, w, h);
}

std::vector<HWND> Form::radio_group(HWND parent, int first_id, int x, int y, int w, int h,
                                    const wchar_t* const* caption_items, int count, int clip_right) {
    scale_rect(x, y, w, h);  // the group is scaled; ArrangeButtons then lays out the buttons in pixels
    if (clip_right) clip_right = sx(clip_right);
    create_child(L"BUTTON", caption_items[0], BS_GROUPBOX, 0, parent, -1, x, y, w, h);
    int th = font_height(font);
    int avail = h - th - 5;  // TCustomRadioGroup.ArrangeButtons
    int bh = avail / count;
    int top = th + 1 + (avail % count) / 2;
    std::vector<HWND> out;
    for (int i = 0; i < count; i++)
        out.push_back(create_child(L"BUTTON", caption_items[1 + i], BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP | WS_TABSTOP : 0),
                                   0, parent, first_id + i, x + 8, y + top + i * bh,
                                   clip_right ? clip_right - (x + 8) : w - 10, bh));
    return out;
}

HWND Form::trackbar(HWND parent, int id, int x, int y, int w, int h, int max, int thumb, bool both) {
    // TTrackBar.CreateParams always adds TBS_FIXEDLENGTH and TBS_ENABLESELRANGE (the wide channel)
    DWORD style = WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS | TBS_ENABLESELRANGE | TBS_FIXEDLENGTH |
                  (both ? TBS_BOTH : TBS_BOTTOM);
    if (!thumb) thumb = 20;  // ThumbLength default
    HWND t = child(TRACKBAR_CLASSW, L"", style, 0, parent, id, x, y, w, h);
    SendMessageW(t, TBM_SETRANGE, FALSE, MAKELPARAM(0, max));
    SendMessageW(t, TBM_SETPAGESIZE, 0, 1);
    SendMessageW(t, TBM_SETTICFREQ, 1, 0);
    SendMessageW(t, TBM_SETTHUMBLENGTH, thumb, 0);
    return t;
}

HWND Form::combo(HWND parent, int id, int x, int y, int w, const wchar_t* const* items, int count, int max_len) {
    int h = 200;
    scale_rect(x, y, w, h);
    HWND c = create_child(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, parent, id, x, y,
                          w, 200);
    for (int i = 0; i < count; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    COMBOBOXINFO ci = {sizeof ci};
    if (GetComboBoxInfo(c, &ci) && ci.hwndItem) SendMessageW(ci.hwndItem, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SendMessageW(c, CB_LIMITTEXT, max_len, 0);
    return c;
}

HWND Form::page(HWND parent, int x, int y, int w, int h) {
    HWND p = CreateWindowExW(WS_EX_CONTROLPARENT, PAGE_CLASS, L"", WS_CHILD | WS_CLIPSIBLINGS, x, y, w, h, parent,
                             nullptr, hinst, nullptr);
    return p;
}

HWND Form::media_player(HWND parent, int first_id, int x, int y, int h, const Glyph* glyphs, int count) {
    int w = 0;
    scale_rect(x, y, w, h);
    w = count * (MP_BUTTON_W - 1) + 1;  // TMediaPlayer.AdjustSize
    MediaPlayer* m = new MediaPlayer{glyphs, count, first_id};
    HWND c = CreateWindowExW(0, MEDIA_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP, x, y, w, h, parent,
                             (HMENU)(INT_PTR)first_id, hinst, m);
    if (!c) delete m;
    return c;
}

bool Form::custom_draw(NMCUSTOMDRAW* cd, LRESULT* result) {
    auto it = bitbtns_.find(cd->hdr.hwndFrom);
    if (it == bitbtns_.end()) return false;
    if (cd->dwDrawStage == CDDS_PREPAINT) {
        *result = CDRF_NOTIFYPOSTPAINT;
        return true;
    }
    if (cd->dwDrawStage != CDDS_POSTPAINT) return false;
    const BitBtnInfo& b = it->second;
    HDC dc = cd->hdc;
    RECT rc = cd->rc;
    bool disabled = (cd->uItemState & CDIS_DISABLED) != 0;
    bool down = (cd->uItemState & CDIS_SELECTED) != 0;
    // themed TBitBtn lays out inside ThemeServices.ContentRect and does not shift when pressed
    if (HTHEME th = OpenThemeData(cd->hdr.hwndFrom, L"BUTTON")) {
        RECT content;
        if (SUCCEEDED(GetThemeBackgroundContentRect(th, dc, BP_PUSHBUTTON, PBS_NORMAL, &rc, &content))) rc = content;
        CloseThemeData(th);
        down = false;
    }
    // TButtonGlyph.CalcButtonLayout (Layout = blGlyphLeft)
    int cw = rc.right - rc.left, chh = rc.bottom - rc.top;
    HGDIOBJ oldf = SelectObject(dc, b.font);
    RECT tr = {0, 0, 0, 0};
    if (!b.caption.empty()) DrawTextW(dc, b.caption.c_str(), -1, &tr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int tw = tr.right, th = tr.bottom;
    int gw = b.glyph.normal ? b.glyph.w : 0, gh = b.glyph.normal ? b.glyph.h : 0;
    int spacing = (b.caption.empty() || !gw) ? 0 : b.spacing;
    int margin = b.margin;
    if (margin == -1) margin = (cw - (gw + spacing + tw) + 1) / 2;
    int gx = margin, gy = (chh - gh + 1) / 2;
    int tx = gx + gw + spacing, ty = (chh - th + 1) / 2;
    if (down) gx++, gy++, tx++, ty++;
    if (gw) {
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = disabled ? b.glyph.disabled : b.glyph.normal;
        COLORREF key = disabled ? b.glyph.key_disabled : b.glyph.key_normal;
        HGDIOBJ old = SelectObject(mem, bmp);
        TransparentBlt(dc, rc.left + gx, rc.top + gy, gw, gh, mem, 0, 0, gw, gh, key);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    if (!b.caption.empty()) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
        RECT t = {rc.left + tx, rc.top + ty, rc.left + tx + tw, rc.top + ty + th};
        DrawTextW(dc, b.caption.c_str(), -1, &t, DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, oldf);
    *result = CDRF_DODEFAULT;
    return true;
}

LRESULT Form::handle(UINT msg, WPARAM wp, LPARAM lp) { return default_handle(msg, wp, lp); }

LRESULT Form::default_handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_bevels(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_NOTIFY: {
        NMHDR* h = (NMHDR*)lp;
        LRESULT r;
        if (h->code == NM_CUSTOMDRAW && custom_draw((NMCUSTOMDRAW*)lp, &r)) return r;
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace ui
