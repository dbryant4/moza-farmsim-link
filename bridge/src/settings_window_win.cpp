#include "settings_window.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <vector>

namespace bridge {

namespace {

constexpr wchar_t kClassName[] = L"MozaFsLinkSettings";
constexpr UINT    kTimerId     = 1;
constexpr UINT    kRefreshMs   = 250;

// Sizes are in pixels at 96 dpi and scaled to the screen's setting when the window is built.
constexpr int kClientWidth  = 880;
constexpr int kClientHeight = 610;
constexpr int kMargin       = 12;
constexpr int kTopHeight    = 46;
constexpr int kBottomHeight = 52;
constexpr int kNavWidth     = 176;
constexpr int kRowHeight    = 32;
constexpr int kHintHeight   = 58;

enum ControlId : int
{
    kIdNav = 100,
    kIdState,
    kIdForces,
    kIdRelease,
    kIdTitle,
    kIdHint,
    kIdStatusList,
    kIdDirty,
    kIdExport,
    kIdImport,
    kIdReset,
    kIdRevert,
    kIdSave,
    kIdRowBase = 1000,
};

/** The controls of one setting are numbered from kIdRowBase + row * kRowIds. */
constexpr int kRowIds = 8;
enum RowPart : int
{
    kPartLabel = 0,
    kPartSlider,
    kPartEdit,
    kPartUnit,
    kPartDefault,
    kPartCheck,
};

std::wstring widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int    length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string narrow(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int   length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring textOf(HWND control)
{
    const int    length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

/** Sets a control's text only when it differs, so that nothing flickers on the refresh timer. */
void setText(HWND control, const std::wstring& text)
{
    if (control != nullptr && textOf(control) != text)
    {
        SetWindowTextW(control, text.c_str());
    }
}

struct Row
{
    SettingInfo info;
    int         page = 0;
    /** Number of slider positions above zero. */
    int  steps   = 1;
    HWND label   = nullptr;
    HWND slider  = nullptr;
    HWND edit    = nullptr;
    HWND unit    = nullptr;
    HWND check   = nullptr;
    HWND restore = nullptr;
    /** What the controls currently show; NaN before the first refresh. */
    double shown = std::nan("");

    bool toggle() const { return info.kind == SettingInfo::Kind::Toggle; }
};

} // namespace

struct SettingsWindow::Impl
{
    // What is shown. Held only for the length of one handler, never across a dialog, so that
    // detach() from another thread is prompt.
    mutable std::mutex    mutex;
    std::optional<Source> source;

    std::atomic<HWND> window{nullptr};

    // Everything below belongs to the window's thread.
    int   dpi       = 96;
    HFONT font      = nullptr;
    HFONT titleFont = nullptr;
    HWND  nav       = nullptr;
    HWND  state     = nullptr;
    HWND  forces    = nullptr;
    HWND  release   = nullptr;
    HWND  title     = nullptr;
    HWND  hint      = nullptr;
    HWND  list      = nullptr;
    HWND  dirty     = nullptr;
    HWND  tooltip   = nullptr;
    HWND  buttonSave = nullptr, buttonRevert = nullptr, buttonReset = nullptr, buttonImport = nullptr, buttonExport = nullptr;

    std::vector<std::wstring> pages; // page 0 is the live status; the rest are the settings' groups
    std::vector<Row>          rows;
    std::vector<std::wstring> statusShown;
    int                       page = 0;

    int px(int value) const { return MulDiv(value, dpi, 96); }

    static Impl* self(HWND window) { return reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA)); }

    HWND child(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD exStyle = 0, HFONT useFont = nullptr)
    {
        const HWND parent  = window.load();
        const HWND control = CreateWindowExW(exStyle, className, text, WS_CHILD | style, px(x), px(y), px(w), px(h), parent,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (control != nullptr)
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(useFont != nullptr ? useFont : font), FALSE);
        }
        return control;
    }

    void addTip(HWND control, const std::wstring& text)
    {
        if (tooltip == nullptr || control == nullptr || text.empty())
        {
            return;
        }
        TOOLINFOW tool{};
        tool.cbSize   = sizeof(tool);
        tool.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd     = window.load();
        tool.uId      = reinterpret_cast<UINT_PTR>(control);
        tool.lpszText = const_cast<wchar_t*>(text.c_str());
        SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }

    /** Creates every control. Called once, from WM_CREATE, with the source's settings as they are registered. */
    void build(const std::vector<SettingState>& settings)
    {
        const HWND parent = window.load();
        {
            const HDC screen = GetDC(nullptr);
            dpi              = GetDeviceCaps(screen, LOGPIXELSX);
            ReleaseDC(nullptr, screen);
        }
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        {
            font                 = CreateFontIndirectW(&metrics.lfMessageFont);
            LOGFONTW larger      = metrics.lfMessageFont;
            larger.lfHeight      = larger.lfHeight * 4 / 3;
            larger.lfWeight      = FW_SEMIBOLD;
            titleFont            = CreateFontIndirectW(&larger);
        }

        // Fit the client area, whatever the frame around it measures.
        RECT frame{0, 0, px(kClientWidth), px(kClientHeight)};
        AdjustWindowRectEx(&frame, static_cast<DWORD>(GetWindowLongPtrW(parent, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongPtrW(parent, GWL_EXSTYLE)));
        SetWindowPos(parent, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                                  CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (tooltip != nullptr)
        {
            SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, px(340));
            SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
        }

        // Along the top: what the game is doing, and the two controls that matter in a hurry.
        state   = child(L"STATIC", L"", WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, kMargin, 8, 420, 30, kIdState, 0, titleFont);
        forces  = child(L"BUTTON", L"Forces on", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, kClientWidth - kMargin - 250, 11, 100, 24, kIdForces);
        release = child(L"BUTTON", L"Release forces", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kClientWidth - kMargin - 140, 9, 140, 28, kIdRelease);
        addTip(forces, L"The master switch. Off, the link applies no force at all until it is switched on again.");
        addTip(release, L"Drops every force at once and switches forces off. Switch Forces on to resume.");
        child(L"STATIC", L"", WS_VISIBLE | SS_ETCHEDHORZ, 0, kTopHeight, kClientWidth, 2, -1);

        // Down the left: the pages.
        const int bodyTop    = kTopHeight + kMargin;
        const int bodyHeight = kClientHeight - kTopHeight - kBottomHeight - 2 * kMargin;
        // Drawn by this window (see drawNavItem), for room around the names and no sunken frame.
        nav = child(L"LISTBOX", L"", WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, kMargin, bodyTop,
                    kNavWidth, bodyHeight, kIdNav);

        pages.clear();
        pages.push_back(L"Live status");
        for (const SettingState& setting : settings)
        {
            const std::wstring group = widen(setting.info.group);
            if (std::find(pages.begin() + 1, pages.end(), group) == pages.end())
            {
                pages.push_back(group);
            }
        }
        for (const std::wstring& name : pages)
        {
            SendMessageW(nav, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }

        // To the right: the selected page.
        const int paneLeft  = kMargin + kNavWidth + kMargin + 4;
        const int paneWidth = kClientWidth - paneLeft - kMargin;
        title = child(L"STATIC", L"", WS_VISIBLE | SS_LEFT | SS_NOPREFIX, paneLeft, bodyTop, paneWidth, 26, kIdTitle, 0, titleFont);
        hint  = child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, paneLeft, bodyTop + bodyHeight - kHintHeight, paneWidth, kHintHeight, kIdHint);

        list = child(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS, paneLeft, bodyTop + 34, paneWidth,
                     bodyHeight - 34, kIdStatusList, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column{};
        column.mask    = LVCF_TEXT | LVCF_WIDTH;
        column.cx      = px(paneWidth * 55 / 100);
        // The wide messages by name: this project is built without UNICODE defined, and the
        // ListView_ macros would then send the narrow ones.
        column.pszText = const_cast<wchar_t*>(L"What");
        SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
        column.cx      = px(paneWidth * 40 / 100);
        column.pszText = const_cast<wchar_t*>(L"Now");
        SendMessageW(list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));

        rows.clear();
        std::vector<int> rowsOnPage(pages.size(), 0);
        const int        rowsTop = bodyTop + 38;
        for (const SettingState& setting : settings)
        {
            Row row;
            row.info = setting.info;
            row.page = static_cast<int>(std::find(pages.begin() + 1, pages.end(), widen(setting.info.group)) - pages.begin());
            const int index = static_cast<int>(rows.size());
            const int base  = kIdRowBase + index * kRowIds;
            const int y     = rowsTop + rowsOnPage[static_cast<std::size_t>(row.page)]++ * kRowHeight;
            const std::wstring label = widen(setting.info.label);
            const std::wstring help  = widen(setting.info.description);

            const int restoreWidth = 64;
            const int restoreLeft  = paneLeft + paneWidth - restoreWidth;
            if (row.toggle())
            {
                row.check = child(L"BUTTON", label.c_str(), WS_TABSTOP | BS_AUTOCHECKBOX | BS_NOTIFY, paneLeft, y + 3, restoreLeft - paneLeft - 8, 22,
                                  base + kPartCheck);
                addTip(row.check, help);
            }
            else
            {
                const int labelWidth = 262;
                const int unitWidth  = 48;
                const int editWidth  = 64;
                const int unitLeft   = restoreLeft - 8 - unitWidth;
                const int editLeft   = unitLeft - 6 - editWidth;
                const int sliderLeft = paneLeft + labelWidth + 4;
                row.label  = child(L"STATIC", label.c_str(), SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY | SS_NOPREFIX | SS_ENDELLIPSIS, paneLeft, y, labelWidth, 26,
                                   base + kPartLabel);
                row.slider = child(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH, sliderLeft, y, editLeft - sliderLeft - 6, 26,
                                   base + kPartSlider);
                row.edit   = child(L"EDIT", L"", WS_TABSTOP | ES_RIGHT | ES_AUTOHSCROLL, editLeft, y + 2, editWidth, 22, base + kPartEdit, WS_EX_CLIENTEDGE);
                row.unit   = child(L"STATIC", widen(setting.info.unit).c_str(), SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, unitLeft, y, unitWidth, 26,
                                   base + kPartUnit);
                const double span = (setting.info.maximum - setting.info.minimum) / setting.info.step;
                row.steps         = static_cast<int>(std::clamp(std::lround(span), 1L, 30000L));
                SendMessageW(row.slider, TBM_SETRANGEMIN, FALSE, 0);
                SendMessageW(row.slider, TBM_SETRANGEMAX, FALSE, row.steps);
                SendMessageW(row.slider, TBM_SETPAGESIZE, 0, std::max(1, row.steps / 10));
                SendMessageW(row.edit, EM_SETLIMITTEXT, 24, 0);
                addTip(row.label, help);
                addTip(row.slider, help);
            }
            row.restore = child(L"BUTTON", L"Default", WS_TABSTOP | BS_PUSHBUTTON | BS_NOTIFY, restoreLeft, y + 1, restoreWidth, 24, base + kPartDefault);
            addTip(row.restore, L"Puts this one setting back to its default.");
            rows.push_back(row);
        }

        // Along the bottom: what is unsaved, and what to do about it.
        const int bottomTop = kClientHeight - kBottomHeight;
        child(L"STATIC", L"", WS_VISIBLE | SS_ETCHEDHORZ, 0, bottomTop, kClientWidth, 2, -1);
        const int buttonTop = bottomTop + 12;
        const int width     = 84;
        int       x         = kClientWidth - kMargin - width;
        buttonSave   = child(L"BUTTON", L"Save", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, buttonTop, width, 28, kIdSave);
        buttonRevert = child(L"BUTTON", L"Revert", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x -= width + 6, buttonTop, width, 28, kIdRevert);
        buttonReset  = child(L"BUTTON", L"Defaults...", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x -= width + 6, buttonTop, width, 28, kIdReset);
        buttonImport = child(L"BUTTON", L"Import...", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x -= width + 18, buttonTop, width, 28, kIdImport);
        buttonExport = child(L"BUTTON", L"Export...", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x -= width + 6, buttonTop, width, 28, kIdExport);
        dirty        = child(L"STATIC", L"", WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, kMargin, buttonTop, x - 2 * kMargin, 28, kIdDirty);
        addTip(buttonSave, L"Keeps the settings as they are now for next time.");
        addTip(buttonRevert, L"Goes back to what was last saved.");
        addTip(buttonReset, L"Puts every setting back to its default. Not saved until you press Save.");
        addTip(buttonImport, L"Loads settings from a file made with Export. They take effect at once and count as unsaved.");
        addTip(buttonExport, L"Writes the settings as they are now, saved or not, to a file.");

        selectPage(1 < static_cast<int>(pages.size()) ? 1 : 0);
    }

    void drawNavItem(const DRAWITEMSTRUCT& item)
    {
        if (item.itemID == static_cast<UINT>(-1))
        {
            return;
        }
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        FillRect(item.hDC, &item.rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        wchar_t   name[128] = L"";
        const int length    = static_cast<int>(SendMessageW(item.hwndItem, LB_GETTEXTLEN, item.itemID, 0));
        if (length > 0 && length < static_cast<int>(ARRAYSIZE(name)))
        {
            SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(name));
        }
        RECT text = item.rcItem;
        text.left += px(12);
        text.right -= px(6);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        DrawTextW(item.hDC, name, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        if ((item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0)
        {
            DrawFocusRect(item.hDC, &item.rcItem);
        }
    }

    void show(HWND control, bool visible)
    {
        if (control != nullptr)
        {
            ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
        }
    }

    void selectPage(int index)
    {
        page = std::clamp(index, 0, static_cast<int>(pages.size()) - 1);
        SendMessageW(nav, LB_SETCURSEL, static_cast<WPARAM>(page), 0);
        setText(title, pages[static_cast<std::size_t>(page)]);
        show(list, page == 0);
        show(hint, page != 0);
        setHint(-1);
        for (const Row& row : rows)
        {
            const bool visible = row.page == page;
            show(row.label, visible);
            show(row.slider, visible);
            show(row.edit, visible);
            show(row.unit, visible);
            show(row.check, visible);
            show(row.restore, visible);
        }
        InvalidateRect(window.load(), nullptr, TRUE);
    }

    void setHint(int rowIndex)
    {
        if (rowIndex < 0 || rowIndex >= static_cast<int>(rows.size()))
        {
            setText(hint, L"Point at a setting, or pick it, to read what it does. Changes take effect at once; Save keeps them.");
            return;
        }
        const SettingInfo& info = rows[static_cast<std::size_t>(rowIndex)].info;
        std::wstring       text = widen(info.label) + L": " + widen(info.description);
        if (info.kind != SettingInfo::Kind::Toggle)
        {
            text += L" (" + widen(formatNumber(info.minimum)) + L" to " + widen(formatNumber(info.maximum));
            if (!info.unit.empty())
            {
                text += L" " + widen(info.unit);
            }
            text += L", default " + widen(formatNumber(info.defaultValue)) + L")";
        }
        setText(hint, text);
    }

    int rowOf(HWND control) const
    {
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const Row& row = rows[i];
            if (control != nullptr && (control == row.label || control == row.slider || control == row.edit || control == row.check || control == row.restore))
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    /** Stores a new value for one setting and shows the result. */
    void change(int rowIndex, double value)
    {
        {
            std::scoped_lock lock(mutex);
            if (!source || rowIndex < 0 || rowIndex >= static_cast<int>(rows.size()))
            {
                return;
            }
            source->settings->set(rows[static_cast<std::size_t>(rowIndex)].info.key, value);
        }
        refresh(true);
    }

    void commitEdit(int rowIndex)
    {
        if (rowIndex < 0 || rowIndex >= static_cast<int>(rows.size()))
        {
            return;
        }
        Row&        row  = rows[static_cast<std::size_t>(rowIndex)];
        std::string text = narrow(textOf(row.edit));
        std::replace(text.begin(), text.end(), ',', '.');
        const auto value = parseNumber(text);
        // Whatever was typed, what is shown afterwards is what is in force.
        row.shown = std::nan("");
        if (value)
        {
            change(rowIndex, *value);
        }
        else
        {
            refresh(true);
        }
    }

    /** Brings every control in line with the settings and the status. @p everything also rewrites a field that has the keyboard. */
    void refresh(bool everything = false)
    {
        std::vector<SettingState> settings;
        StatusSnapshot            status;
        bool                      forcesOn = false;
        {
            std::scoped_lock lock(mutex);
            if (!source)
            {
                return;
            }
            settings = source->settings->snapshot();
            status   = source->status->snapshot();
            forcesOn = source->status->forcesEnabled();
        }

        const HWND focus   = GetFocus();
        int        unsaved = 0;
        bool       offDefault = false;
        for (std::size_t i = 0; i < rows.size() && i < settings.size(); ++i)
        {
            Row&                row     = rows[i];
            const SettingState& setting = settings[i];
            unsaved += setting.value != setting.saved ? 1 : 0;
            offDefault = offDefault || setting.value != setting.info.defaultValue;
            EnableWindow(row.restore, setting.value != setting.info.defaultValue);
            if (row.shown == setting.value)
            {
                continue;
            }
            if (row.toggle())
            {
                SendMessageW(row.check, BM_SETCHECK, setting.value != 0.0 ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            else
            {
                const long position = std::lround((setting.value - setting.info.minimum) / setting.info.step);
                SendMessageW(row.slider, TBM_SETPOS, TRUE, std::clamp(position, 0L, static_cast<long>(row.steps)));
                if (everything || focus != row.edit)
                {
                    setText(row.edit, widen(formatNumber(setting.value)));
                }
            }
            row.shown = setting.value;
        }

        setText(dirty, unsaved == 0   ? L"No unsaved changes."
                       : unsaved == 1 ? L"1 unsaved change: in effect now, gone on restart unless saved."
                                      : std::to_wstring(unsaved) + L" unsaved changes: in effect now, gone on restart unless saved.");
        EnableWindow(buttonSave, unsaved > 0);
        EnableWindow(buttonRevert, unsaved > 0);
        EnableWindow(buttonReset, offDefault);

        // The top line: what the game is doing, or that the link has gone quiet.
        std::wstring telemetry;
        for (const StatusItem& item : status.items)
        {
            if (item.key == "game.telemetry" && item.kind == StatusItem::Kind::Text)
            {
                telemetry = widen(item.text);
            }
        }
        const bool quiet = status.ageSeconds < 0.0 || status.ageSeconds > 3.0;
        setText(state, quiet ? L"The link is not reporting" : telemetry.empty() ? L"Linked" : L"Game: " + telemetry);
        if ((SendMessageW(forces, BM_GETCHECK, 0, 0) == BST_CHECKED) != forcesOn)
        {
            SendMessageW(forces, BM_SETCHECK, forcesOn ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        setText(forces, forcesOn ? L"Forces on" : L"Forces off");

        refreshStatus(status);
    }

    void refreshStatus(const StatusSnapshot& status)
    {
        if (list == nullptr)
        {
            return;
        }
        // Rows are made once: the items are registered before the window can exist and never change.
        if (ListView_GetItemCount(list) == 0 && !status.items.empty())
        {
            std::vector<std::wstring> groups;
            ListView_EnableGroupView(list, TRUE);
            for (std::size_t i = 0; i < status.items.size(); ++i)
            {
                const std::wstring group = widen(status.items[i].group);
                auto               found = std::find(groups.begin(), groups.end(), group);
                if (found == groups.end())
                {
                    groups.push_back(group);
                    found = groups.end() - 1;
                    LVGROUP header{};
                    header.cbSize    = sizeof(header);
                    header.mask      = LVGF_HEADER | LVGF_GROUPID;
                    header.pszHeader = const_cast<wchar_t*>(found->c_str());
                    header.iGroupId  = static_cast<int>(found - groups.begin());
                    ListView_InsertGroup(list, -1, &header);
                }
                const std::wstring label = widen(status.items[i].label);
                LVITEMW            item{};
                item.mask     = LVIF_TEXT | LVIF_GROUPID;
                item.iItem    = static_cast<int>(i);
                item.pszText  = const_cast<wchar_t*>(label.c_str());
                item.iGroupId = static_cast<int>(found - groups.begin());
                SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            }
            statusShown.assign(status.items.size(), L"\x01");
        }
        for (std::size_t i = 0; i < status.items.size() && i < statusShown.size(); ++i)
        {
            const StatusItem& item = status.items[i];
            std::wstring      text = L"-";
            if (item.kind == StatusItem::Kind::Text)
            {
                text = widen(item.text);
            }
            else if (item.kind == StatusItem::Kind::Number && std::isfinite(item.number))
            {
                wchar_t number[64];
                std::swprintf(number, 64, L"%.*f", std::clamp(item.decimals, 0, 6), item.number);
                text = number;
                if (!item.unit.empty())
                {
                    text += L" " + widen(item.unit);
                }
            }
            if (text != statusShown[i])
            {
                LVITEMW cell{};
                cell.iSubItem = 1;
                cell.pszText  = const_cast<wchar_t*>(text.c_str());
                SendMessageW(list, LVM_SETITEMTEXTW, i, reinterpret_cast<LPARAM>(&cell));
                statusShown[i] = text;
            }
        }
    }

    // ---- The buttons along the bottom. None of them keeps the lock while a dialog is up. ----

    void say(const std::wstring& text, UINT icon = MB_ICONINFORMATION) { MessageBoxW(window.load(), text.c_str(), L"MOZA FS25 Link", MB_OK | icon); }

    void save()
    {
        std::string error;
        bool        ok = false;
        {
            std::scoped_lock lock(mutex);
            if (!source)
            {
                return;
            }
            ok = source->settings->save(source->settingsFile, &error);
        }
        if (!ok)
        {
            say(L"The settings could not be saved: " + widen(error), MB_ICONWARNING);
        }
        refresh(true);
    }

    void revert()
    {
        if (MessageBoxW(window.load(), L"Go back to the settings as they were last saved?", L"MOZA FS25 Link", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        {
            return;
        }
        {
            std::scoped_lock lock(mutex);
            if (!source)
            {
                return;
            }
            source->settings->load(source->settingsFile);
        }
        refresh(true);
    }

    void resetAll()
    {
        if (MessageBoxW(window.load(), L"Put every setting back to its default?\n\nNothing is saved until you press Save, and Revert undoes this.",
                        L"MOZA FS25 Link", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        {
            return;
        }
        {
            std::scoped_lock lock(mutex);
            if (!source)
            {
                return;
            }
            source->settings->resetToDefaults();
        }
        refresh(true);
    }

    std::wstring askForFile(bool saving)
    {
        wchar_t path[1024] = L"moza-fs-link-settings.ini";
        if (!saving)
        {
            path[0] = L'\0';
        }
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner   = window.load();
        dialog.lpstrFilter = L"Settings files (*.ini)\0*.ini\0All files\0*.*\0";
        dialog.lpstrFile   = path;
        dialog.nMaxFile    = ARRAYSIZE(path);
        dialog.lpstrDefExt = L"ini";
        dialog.lpstrTitle  = saving ? L"Export settings" : L"Import settings";
        dialog.Flags       = OFN_NOCHANGEDIR | OFN_HIDEREADONLY | (saving ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST);
        const BOOL picked  = saving ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
        return picked ? std::wstring(path) : std::wstring();
    }

    void exportSettings()
    {
        const std::wstring path = askForFile(true);
        if (path.empty())
        {
            return;
        }
        std::string text;
        {
            std::scoped_lock lock(mutex);
            if (!source)
            {
                return;
            }
            text = source->settings->exportText();
        }
        std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
        if (!file)
        {
            say(L"The file could not be written.", MB_ICONWARNING);
        }
    }

    void importSettings()
    {
        const std::wstring path = askForFile(false);
        if (path.empty())
        {
            return;
        }
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        std::string   text;
        // A settings file is a few kilobytes; anything far larger is some other file.
        constexpr std::size_t kLargest = 256 * 1024;
        char                  buffer[4096];
        while (file && text.size() <= kLargest)
        {
            file.read(buffer, sizeof(buffer));
            text.append(buffer, static_cast<std::size_t>(file.gcount()));
        }
        if (text.size() > kLargest)
        {
            say(L"That file is too large to be a settings file. Nothing was changed.", MB_ICONWARNING);
            return;
        }
        Settings::ImportResult result;
        {
            std::scoped_lock lock(mutex);
            if (!source)
            {
                return;
            }
            result = source->settings->importText(text);
        }
        refresh(true);
        if (result.applied == 0)
        {
            say(L"That is not a settings file from this program. Nothing was changed.", MB_ICONWARNING);
            return;
        }
        std::wstring message = L"Imported " + std::to_wstring(result.applied) + (result.applied == 1 ? L" setting, " : L" settings, ") +
                               std::to_wstring(result.changed) + L" changed.\n\nThey are in effect now. Press Save to keep them, or Revert to undo.";
        if (!result.warnings.empty())
        {
            message += L"\n\nNot everything in the file could be used:";
            for (std::size_t i = 0; i < result.warnings.size() && i < 8; ++i)
            {
                message += L"\n  " + widen(result.warnings[i]);
            }
            if (result.warnings.size() > 8)
            {
                message += L"\n  and " + std::to_wstring(result.warnings.size() - 8) + L" more";
            }
        }
        say(message, result.warnings.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
    }

    // ---- Messages ----

    void onCommand(int id, int code, HWND control)
    {
        if (id == kIdNav && code == LBN_SELCHANGE)
        {
            selectPage(static_cast<int>(SendMessageW(nav, LB_GETCURSEL, 0, 0)));
            return;
        }
        if (id == IDOK)
        {
            // Enter in a number field takes the number; it does not close the window.
            const int row = rowOf(GetFocus());
            if (row >= 0 && GetFocus() == rows[static_cast<std::size_t>(row)].edit)
            {
                commitEdit(row);
                SendMessageW(rows[static_cast<std::size_t>(row)].edit, EM_SETSEL, 0, -1);
            }
            return;
        }
        if (id == IDCANCEL)
        {
            // Esc in a number field gives up the typing; anywhere else it closes the window.
            const int row = rowOf(GetFocus());
            if (row >= 0 && GetFocus() == rows[static_cast<std::size_t>(row)].edit)
            {
                rows[static_cast<std::size_t>(row)].shown = std::nan("");
                refresh(true);
                return;
            }
            DestroyWindow(window.load());
            return;
        }
        if (code == BN_CLICKED)
        {
            switch (id)
            {
            case kIdForces:
            {
                const bool on = SendMessageW(forces, BM_GETCHECK, 0, 0) == BST_CHECKED;
                {
                    std::scoped_lock lock(mutex);
                    if (source)
                    {
                        source->status->setForcesEnabled(on);
                    }
                }
                refresh();
                return;
            }
            case kIdRelease:
            {
                std::scoped_lock lock(mutex);
                if (source)
                {
                    // Forces also go off, or the link would put them straight back.
                    source->status->setForcesEnabled(false);
                    source->status->requestRelease();
                }
                return;
            }
            case kIdSave: save(); return;
            case kIdRevert: revert(); return;
            case kIdReset: resetAll(); return;
            case kIdImport: importSettings(); return;
            case kIdExport: exportSettings(); return;
            default: break;
            }
        }

        const int row = rowOf(control);
        if (row < 0)
        {
            return;
        }
        const Row& which = rows[static_cast<std::size_t>(row)];
        if (code == EN_SETFOCUS || code == BN_SETFOCUS || code == STN_CLICKED)
        {
            setHint(row);
        }
        if (control == which.edit && code == EN_KILLFOCUS)
        {
            commitEdit(row);
        }
        else if (control == which.check && code == BN_CLICKED)
        {
            setHint(row);
            change(row, SendMessageW(which.check, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1.0 : 0.0);
        }
        else if (control == which.restore && code == BN_CLICKED)
        {
            setHint(row);
            rows[static_cast<std::size_t>(row)].shown = std::nan("");
            change(row, which.info.defaultValue);
        }
    }

    void onSlider(HWND slider)
    {
        const int row = rowOf(slider);
        if (row < 0 || slider != rows[static_cast<std::size_t>(row)].slider)
        {
            return;
        }
        const Row&   which    = rows[static_cast<std::size_t>(row)];
        const auto   position = static_cast<double>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        const double value    = std::min(which.info.minimum + position * which.info.step, which.info.maximum);
        setHint(row);
        change(row, value);
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            Impl* const impl = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
            impl->window = window;
            return DefWindowProcW(window, message, wParam, lParam);
        }
        Impl* const impl = self(window);
        if (impl == nullptr)
        {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        switch (message)
        {
        case WM_COMMAND:
            impl->onCommand(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam));
            return 0;
        case WM_HSCROLL:
            impl->onSlider(reinterpret_cast<HWND>(lParam));
            return 0;
        case WM_MEASUREITEM:
        {
            MEASUREITEMSTRUCT* const measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (measure != nullptr && measure->CtlID == kIdNav)
            {
                measure->itemHeight = static_cast<UINT>(impl->px(30));
                return TRUE;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }
        case WM_DRAWITEM:
        {
            const DRAWITEMSTRUCT* const item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item != nullptr && item->CtlID == kIdNav)
            {
                impl->drawNavItem(*item);
                return TRUE;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }
        case WM_NOTIFY:
        {
            const NMHDR* const header = reinterpret_cast<const NMHDR*>(lParam);
            if (header != nullptr && header->code == static_cast<UINT>(NM_SETFOCUS))
            {
                const int row = impl->rowOf(header->hwndFrom);
                if (row >= 0)
                {
                    impl->setHint(row);
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == kTimerId)
            {
                bool gone;
                {
                    std::scoped_lock lock(impl->mutex);
                    gone = !impl->source;
                }
                if (gone)
                {
                    DestroyWindow(window);
                }
                else
                {
                    impl->refresh();
                }
            }
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        {
            // A white page, like the rest of Windows' own settings, rather than dialog grey.
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(reinterpret_cast<HWND>(lParam) == impl->hint ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kTimerId);
            impl->window = nullptr;
            impl->rows.clear();
            impl->statusShown.clear();
            impl->list = impl->nav = impl->tooltip = nullptr;
            if (impl->font != nullptr)
            {
                DeleteObject(impl->font);
                impl->font = nullptr;
            }
            if (impl->titleFont != nullptr)
            {
                DeleteObject(impl->titleFont);
                impl->titleFont = nullptr;
            }
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    /** Creates the window, hidden, with all its controls. Returns false if there is nothing to show or it could not be made. */
    bool create()
    {
        std::vector<SettingState> settings;
        {
            std::scoped_lock lock(mutex);
            if (!source)
            {
                return false;
            }
            settings = source->settings->snapshot();
        }

        static bool registered = false;
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!registered)
        {
            INITCOMMONCONTROLSEX controls{};
            controls.dwSize = sizeof(controls);
            controls.dwICC  = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
            InitCommonControlsEx(&controls);

            WNDCLASSEXW windowClass{};
            windowClass.cbSize        = sizeof(windowClass);
            windowClass.lpfnWndProc   = &Impl::windowProc;
            windowClass.hInstance     = instance;
            windowClass.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
            windowClass.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
            windowClass.lpszClassName = kClassName;
            windowClass.hIcon         = LoadIconW(instance, MAKEINTRESOURCEW(1));
            windowClass.hIconSm       = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                                      GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
            registered                = RegisterClassExW(&windowClass) != 0;
        }
        const HWND made = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, L"MOZA FS25 Link settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                          CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, nullptr, nullptr, instance, this);
        if (made == nullptr)
        {
            return false;
        }
        build(settings);
        refresh(true);
        return true;
    }
};

SettingsWindow::SettingsWindow() : m_impl(std::make_unique<Impl>()) {}

SettingsWindow::~SettingsWindow()
{
    detach();
}

void SettingsWindow::attach(const Source& source)
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->source = source;
}

void SettingsWindow::detach()
{
    {
        std::scoped_lock lock(m_impl->mutex);
        m_impl->source.reset();
    }
    // From here on no handler touches the settings; the window follows when its thread gets to it.
    if (const HWND window = m_impl->window.load())
    {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
}

bool SettingsWindow::attached() const
{
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->source.has_value();
}

void SettingsWindow::show()
{
    HWND window = m_impl->window.load();
    if (window == nullptr)
    {
        if (!m_impl->create())
        {
            return;
        }
        window = m_impl->window.load();
        SetTimer(window, kTimerId, kRefreshMs, nullptr);
    }
    if (IsIconic(window))
    {
        ShowWindow(window, SW_RESTORE);
    }
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
}

bool SettingsWindow::translate(void* message)
{
    const HWND window = m_impl->window.load();
    return window != nullptr && message != nullptr && IsDialogMessageW(window, static_cast<MSG*>(message)) != FALSE;
}

bool SettingsWindow::renderToFile(const std::filesystem::path& file, int page)
{
    if (m_impl->window.load() == nullptr && !m_impl->create())
    {
        return false;
    }
    const HWND window = m_impl->window.load();
    m_impl->selectPage(page);
    m_impl->refresh(true);

    RECT client{};
    GetClientRect(window, &client);
    const int width  = client.right - client.left;
    const int height = client.bottom - client.top;

    BITMAPINFO info{};
    info.bmiHeader.biSize        = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth       = width;
    info.bmiHeader.biHeight      = height; // bottom-up, as a .bmp file stores it
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 24;
    info.bmiHeader.biCompression = BI_RGB;
    void*         bits   = nullptr;
    const HDC     screen = GetDC(nullptr);
    const HDC     memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    bool ok = false;
    if (memory != nullptr && bitmap != nullptr && bits != nullptr)
    {
        const HGDIOBJ before = SelectObject(memory, bitmap);
        // WM_PRINT draws in the window's coordinates, frame and title bar included; shift it so
        // that the client area lands at the bitmap's corner.
        RECT  outer{};
        POINT corner{0, 0};
        GetWindowRect(window, &outer);
        ClientToScreen(window, &corner);
        const RECT whole{0, 0, width, height};
        FillRect(memory, &whole, GetSysColorBrush(COLOR_WINDOW));
        SetViewportOrgEx(memory, outer.left - corner.x, outer.top - corner.y, nullptr);
        // The frames of the controls (a number box's border, say) are their non-client parts.
        SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
        SetViewportOrgEx(memory, 0, 0, nullptr);
        GdiFlush();
        SelectObject(memory, before);

        const DWORD      rowBytes = (static_cast<DWORD>(width) * 3 + 3) & ~3u;
        const DWORD      size     = rowBytes * static_cast<DWORD>(height);
        BITMAPFILEHEADER header{};
        header.bfType    = 0x4D42;
        header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        header.bfSize    = header.bfOffBits + size;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        out.write(static_cast<const char*>(bits), size);
        ok = static_cast<bool>(out);
    }
    if (bitmap != nullptr)
    {
        DeleteObject(bitmap);
    }
    if (memory != nullptr)
    {
        DeleteDC(memory);
    }
    DestroyWindow(window);
    return ok;
}

std::string SettingsWindow::selfTest()
{
    Impl& impl = *m_impl;
    if (impl.window.load() == nullptr && !impl.create())
    {
        return "FAIL the window could not be made\n";
    }
    const HWND  window = impl.window.load();
    std::string report;
    const auto  check = [&report](bool ok, const std::string& what) { report += (ok ? "ok   " : "FAIL ") + what + "\n"; };
    const auto  command = [&](HWND control, int code) {
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(control), code), reinterpret_cast<LPARAM>(control));
    };
    const auto shown   = [](HWND control) { return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0; };
    const auto enabled = [](HWND control) { return IsWindowEnabled(control) != FALSE; };
    Settings*    settings;
    StatusBoard* status;
    std::filesystem::path file;
    {
        std::scoped_lock lock(impl.mutex);
        settings = impl.source->settings;
        status   = impl.source->status;
        file     = impl.source->settingsFile;
    }

    int number = -1;
    int toggle = -1;
    for (std::size_t i = 0; i < impl.rows.size(); ++i)
    {
        if (impl.rows[i].toggle() && toggle < 0)
        {
            toggle = static_cast<int>(i);
        }
        // One whose default is away from both ends, so that every move below is a change.
        const SettingInfo& candidate = impl.rows[i].info;
        if (!impl.rows[i].toggle() && number < 0 && impl.rows[i].steps >= 10 && candidate.defaultValue < candidate.maximum &&
            candidate.defaultValue > candidate.minimum + candidate.step)
        {
            number = static_cast<int>(i);
        }
    }
    check(number >= 0 && toggle >= 0, "there are sliders and tick boxes: " + std::to_string(impl.rows.size()) + " settings on " +
                                          std::to_string(impl.pages.size() - 1) + " pages");
    if (number < 0 || toggle < 0)
    {
        DestroyWindow(window);
        return report;
    }
    const auto row  = [&](int index) -> Row& { return impl.rows[static_cast<std::size_t>(index)]; };
    const auto info = row(number).info;
    check(!enabled(impl.buttonSave) && !enabled(impl.buttonRevert), "nothing changed yet: Save and Revert are greyed out");
    check(textOf(impl.dirty) == L"No unsaved changes.", "and the bottom line says so");

    // The page list.
    SendMessageW(impl.nav, LB_SETCURSEL, static_cast<WPARAM>(row(number).page), 0);
    command(impl.nav, LBN_SELCHANGE);
    check(impl.page == row(number).page && shown(row(number).slider) && !shown(impl.list), "picking a page shows its settings");
    check(textOf(impl.title) == impl.pages[static_cast<std::size_t>(impl.page)], "and its name as the heading");
    int elsewhere = 0;
    for (const Row& other : impl.rows)
    {
        elsewhere += other.page != impl.page && ((other.slider != nullptr && shown(other.slider)) || (other.check != nullptr && shown(other.check))) ? 1 : 0;
    }
    check(elsewhere == 0, "and hides every other page's");

    // A slider.
    SendMessageW(row(number).slider, TBM_SETPOS, TRUE, row(number).steps);
    SendMessageW(window, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, 0), reinterpret_cast<LPARAM>(row(number).slider));
    check(settings->get(info.key) == info.maximum, "dragging a slider to the end sets the maximum (" + info.key + ")");
    check(textOf(row(number).edit) == widen(formatNumber(info.maximum)), "the number beside it follows");
    check(enabled(impl.buttonSave) && enabled(impl.buttonRevert) && enabled(row(number).restore), "Save, Revert and its Default button come alive");
    check(textOf(impl.dirty).rfind(L"1 unsaved change", 0) == 0, "the bottom line counts one unsaved change");
    check(textOf(impl.hint).rfind(widen(info.label), 0) == 0, "the help line describes the setting being changed");

    // Typing a number.
    const double typed = info.minimum + info.step;
    SetWindowTextW(row(number).edit, widen(formatNumber(typed)).c_str());
    command(row(number).edit, EN_KILLFOCUS);
    check(settings->get(info.key) == typed, "a typed number is taken on leaving the box");
    check(SendMessageW(row(number).slider, TBM_GETPOS, 0, 0) == 1, "and the slider follows");
    SetWindowTextW(row(number).edit, L"abc");
    command(row(number).edit, EN_KILLFOCUS);
    check(settings->get(info.key) == typed && textOf(row(number).edit) == widen(formatNumber(typed)), "nonsense is refused and the old number shown again");
    SetWindowTextW(row(number).edit, L"999999");
    command(row(number).edit, EN_KILLFOCUS);
    check(settings->get(info.key) == info.maximum && textOf(row(number).edit) == widen(formatNumber(info.maximum)), "a number past the end stops at the end");

    // Default, for one setting.
    command(row(number).restore, BN_CLICKED);
    check(settings->get(info.key) == info.defaultValue && !enabled(row(number).restore), "its Default button puts it back and greys out");

    // A tick box.
    const auto   box    = row(toggle).info;
    const double before = settings->get(box.key);
    SendMessageW(impl.nav, LB_SETCURSEL, static_cast<WPARAM>(row(toggle).page), 0);
    command(impl.nav, LBN_SELCHANGE);
    SendMessageW(row(toggle).check, BM_SETCHECK, before != 0.0 ? BST_UNCHECKED : BST_CHECKED, 0);
    command(row(toggle).check, BN_CLICKED);
    check(settings->get(box.key) == (before != 0.0 ? 0.0 : 1.0), "ticking a box flips its setting (" + box.key + ")");

    // Changed from outside the window (an import does this): the controls catch up on the timer.
    settings->set(info.key, info.minimum);
    SendMessageW(window, WM_TIMER, kTimerId, 0);
    check(SendMessageW(row(number).slider, TBM_GETPOS, 0, 0) == 0 && textOf(row(number).edit) == widen(formatNumber(info.minimum)),
          "a change made elsewhere shows up by itself");

    // Save.
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    command(impl.buttonSave, BN_CLICKED);
    check(std::filesystem::exists(file, ignored), "Save writes the settings file");
    check(!settings->dirty() && !enabled(impl.buttonSave) && textOf(impl.dirty) == L"No unsaved changes.", "after which nothing is unsaved");

    // Forces.
    SendMessageW(impl.forces, BM_SETCHECK, BST_UNCHECKED, 0);
    command(impl.forces, BN_CLICKED);
    check(!status->forcesEnabled() && textOf(impl.forces) == L"Forces off", "unticking Forces switches them off and says so");
    SendMessageW(impl.forces, BM_SETCHECK, BST_CHECKED, 0);
    command(impl.forces, BN_CLICKED);
    check(status->forcesEnabled() && textOf(impl.forces) == L"Forces on", "ticking it switches them back on");
    const auto releases = status->releaseRequests();
    command(impl.release, BN_CLICKED);
    SendMessageW(window, WM_TIMER, kTimerId, 0);
    check(status->releaseRequests() == releases + 1 && !status->forcesEnabled() && SendMessageW(impl.forces, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
          "Release forces asks for a release and leaves forces off");

    // The live status page.
    SendMessageW(impl.nav, LB_SETCURSEL, 0, 0);
    command(impl.nav, LBN_SELCHANGE);
    const int items = static_cast<int>(SendMessageW(impl.list, LVM_GETITEMCOUNT, 0, 0));
    wchar_t   cell[128] = L"";
    LVITEMW   item{};
    item.iSubItem   = 1;
    item.pszText    = cell;
    item.cchTextMax = ARRAYSIZE(cell);
    SendMessageW(impl.list, LVM_GETITEMTEXTW, 0, reinterpret_cast<LPARAM>(&item));
    check(shown(impl.list) && items > 10, "the status page lists what the link reports: " + std::to_string(items) + " lines");
    check(std::wstring(cell) == L"live, driving" && textOf(impl.state) == L"Game: live, driving", "with the game's state in the list and at the top");
    status->set("game.speed", 31.5);
    SendMessageW(window, WM_TIMER, kTimerId, 0);
    bool found = false;
    for (int i = 0; i < items; ++i)
    {
        SendMessageW(impl.list, LVM_GETITEMTEXTW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&item));
        found = found || std::wstring(cell) == L"31.5 km/h";
    }
    check(found, "and new values as they arrive");

    // Taken away from under it, as when the link stops: it lets go and closes.
    detach();
    SendMessageW(window, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, 0), reinterpret_cast<LPARAM>(nullptr));
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        DispatchMessageW(&message);
    }
    check(impl.window.load() == nullptr, "when the link goes away the window closes instead of holding on");
    return report;
}

const wchar_t* SettingsWindow::showEventName()
{
    return L"Local\\MozaFsLinkShowSettings";
}

} // namespace bridge
