// ============================================================================
//  ribbon.cpp -- IUIApplication + IUICommandHandler implementation
//
//  THE RIBBON'S CONTRACT, WHICH IS THE OPPOSITE OF A NORMAL UI TOOLKIT
//  -----------------------------------------------------------------------
//  Nothing here pushes state into the ribbon. The framework PULLS it: whenever
//  it needs to render a control it calls UpdateProperty(nCmdID, key,
//  pCurrentValue, pNewValue) and expects us to fill pNewValue. So "make the
//  Bold button look pressed" is not a call we make -- it is an answer we give
//  when asked.
//
//  The only thing we push is invalidation: after anything changes we call
//  IUIFramework::InvalidateUI, which tells the framework its cached answers are
//  stale and it should re-query. Get that backwards and the ribbon renders the
//  state the document had when it was first shown.
//
//  UpdateProperty must return S_OK and fill pNewValue for every key the
//  framework asks about. Returning E_NOTIMPL for a key the framework needs
//  makes the control render blank or disabled, with no diagnostic. The default
//  branch below therefore copies pCurrentValue into pNewValue rather than
//  failing: "I have no opinion, keep what you have" is always a safe answer.
//
//  PROPVARIANT LIFETIME
//  Any pNewValue we fill with an allocated type (VT_BSTR, VT_UNKNOWN,
//  VT_STREAM) is freed by the framework with PropVariantClear. Anything we fill
//  with a scalar is copied. Allocating a BSTR and NOT handing it over leaks it;
//  handing over a stack BSTR crashes. Every BSTR below comes from
//  SysAllocString.
//
//  NO PROPSYS DEPENDENCY
//  The convenient helpers (InitPropVariantFromString, PropVariantChangeType)
//  live in propsys.lib. They are avoided in favour of filling PROPVARIANT
//  fields directly, which keeps LetterPad's link line to the DLLs it genuinely
//  needs. The one place that is genuinely awkward is VT_DECIMAL, which the
//  ribbon uses for font size; the converters below handle it by hand.
// ============================================================================
#include "stdafx.h"
#include "LetterPad.h"

namespace {

// ---------------------------------------------------------------------------
//  VT_DECIMAL helpers
//
//  The ribbon expresses font size as a VT_DECIMAL in points, with one decimal
//  place (11.0, 8.5). DECIMAL is a scaled integer: the true value is
//  (Lo64 | Hi32<<64) / 10^scale, negated if sign is nonzero.
//
//  The critical ordering rule: in a PROPVARIANT, `decVal.wReserved` occupies the
//  same two bytes as `vt`. Writing decVal fields first and `vt` LAST is
//  mandatory; the reverse silently clears the type tag and the framework reads
//  the value as VT_EMPTY.
// ---------------------------------------------------------------------------
void PropVariantSetDecimal(PROPVARIANT* pv, double value)
{
    if (!pv) return;

    bool negative = value < 0;
    if (negative) value = -value;

    // One decimal place is enough for any font size and matches what the
    // ribbon itself sends, so round-tripping a value we received is exact.
    ULONGLONG scaled = static_cast<ULONGLONG>(value * 10.0 + 0.5);

    pv->decVal.Lo64  = scaled & 0xFFFFFFFFFFFFFFFFULL;
    pv->decVal.Hi32  = static_cast<ULONG>(scaled >> 32 >> 32);
    pv->decVal.scale = 1;
    pv->decVal.sign  = negative ? 1 : 0;
    pv->vt           = VT_DECIMAL;      // MUST be last; see above
}

double PropVariantGetDecimal(const PROPVARIANT* pv)
{
    if (!pv) return 0.0;

    // The framework always sends VT_DECIMAL for UI_PKEY_FontSize, but a control
    // can be queried before it has a value, in which case it sends VT_EMPTY or
    // VT_UI4. Accepting those rather than returning 0 keeps the font box from
    // showing "0" on first render.
    switch (pv->vt)
    {
    case VT_DECIMAL:
    {
        double v = static_cast<double>(pv->decVal.Lo64);
        // Hi32 contributes bits above 2^64. A font size will never reach that,
        // but including it keeps the conversion correct for any VT_DECIMAL.
        v += static_cast<double>(pv->decVal.Hi32) * 18446744073709551616.0;
        for (unsigned char i = 0; i < pv->decVal.scale; ++i) v /= 10.0;
        return pv->decVal.sign ? -v : v;
    }
    case VT_R4:  return pv->fltVal;
    case VT_R8:  return pv->dblVal;
    case VT_UI4: return static_cast<double>(pv->ulVal);
    case VT_I4:  return static_cast<double>(pv->lVal);
    default:     return 0.0;
    }
}

// Copies a scalar or already-allocated PROPVARIANT into a destination the
// framework owns. Only the types the ribbon actually exchanges are handled;
// anything else is passed through as VT_EMPTY, which tells the framework "no
// change" and is safe.
void PropVariantCopyShallow(PROPVARIANT* dst, const PROPVARIANT* src)
{
    if (!dst) return;
    if (!src) { dst->vt = VT_EMPTY; return; }

    *dst = *src;      // scalars copy correctly by value

    if (src->vt == VT_BSTR && src->bstrVal)
    {
        // A BSTR is a heap allocation with a length prefix. Copying the pointer
        // would give the framework ownership of memory it will PropVariantClear,
        // while we still believe we own it. Duplicate it.
        dst->bstrVal = SysAllocString(src->bstrVal);
        if (!dst->bstrVal) dst->vt = VT_EMPTY;
    }
}

// ---------------------------------------------------------------------------
//  Font-control state constants
//
//  The ribbon uses a three-valued state for every font property, not a boolean:
//     NOTSET   the property is off for the whole selection
//     SET      the property is on for the whole selection
//     CHANGED  the selection is mixed -- render as indeterminate
//  NOTAVAILABLE means the control cannot report it at all.
//
//  Mapping RichEdit's CHARFORMAT2W onto this is the interesting part: RichEdit
//  has no "mixed" indicator. Its convention is that a dwEffects bit is set only
//  when the property is uniform AND on. A uniform-off property and a mixed
//  property both come back as "bit clear", and they need different ribbon
//  states. Resolving that requires a second query, which is expensive enough
//  that the mapping below reports SET or NOTSET only -- mixed selections render
//  as NOTSET. This is a deliberate, documented simplification (ROADMAP.md).
// ---------------------------------------------------------------------------
UINT FontStateFromBool(bool on)
{
    return on ? UI_FONTWEIGHT_SET : UI_FONTWEIGHT_NOTSET;
}

UINT UnderlineStateFromType(BYTE type)
{
    // CFU_UNDERLINENONE is 0; every other value is some flavour of underline.
    // Reporting the specific style back would need UI_FONTUNDERLINE_* values
    // that not all SDK releases define, so the ribbon shows plain on/off.
    return (type != CFU_UNDERLINENONE) ? UI_FONTUNDERLINE_SET
                                       : UI_FONTUNDERLINE_NOTSET;
}

UINT VerticalStateFromEffects(DWORD effects)
{
    if (effects & CFE_SUPERSCRIPT) return UI_FONTVERTICALPOSITION_SUPERSCRIPT;
    if (effects & CFE_SUBSCRIPT)   return UI_FONTVERTICALPOSITION_SUBSCRIPT;
    return UI_FONTVERTICALPOSITION_NOTSET;
}

// ---------------------------------------------------------------------------
//  Colour mapping
//
//  The ribbon's colour picker works in UI_SWATCHCOLOUR_* indices, but the
//  ForegroundColor / BackgroundColor properties are plain RGB in a VT_UI4.
//  Passing RGB through directly is correct and avoids maintaining a swatch
//  table that would have to match the ribbon's built-in palette.
// ---------------------------------------------------------------------------
COLORREF ColourFromPropVariant(const PROPVARIANT* pv)
{
    if (!pv) return RGB(0, 0, 0);
    switch (pv->vt)
    {
    case VT_UI4: return static_cast<COLORREF>(pv->ulVal);
    case VT_I4:  return static_cast<COLORREF>(pv->lVal);
    default:     return RGB(0, 0, 0);
    }
}

} // anonymous namespace

// ===========================================================================
//  RibbonHandler
//
//  One instance per frame, created by RibbonApp_Create and owned by the
//  framework (which holds the IUIApplication reference). Refcounting is plain
//  InterlockedIncrement/Decrement: the ribbon runs on the UI thread and the
//  framework is single-threaded apartment, but the COM contract does not
//  promise that, and a non-atomic refcount in a COM object is a bug waiting for
//  the first background query.
// ===========================================================================
class RibbonHandler : public IUIApplication, public IUICommandHandler
{
public:
    explicit RibbonHandler(AppFrame* frame) : m_frame(frame), m_ref(1) {}

    // ---- IUnknown --------------------------------------------------------
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;

        // Both interfaces are served by this one object. Returning the same
        // `this` for either is correct only because IUIApplication and
        // IUICommandHandler are both derived from IUnknown at the same offset
        // here -- they are the first two base classes, so no adjustment is
        // needed. If a third interface were added between them this would need
        // a static_cast per branch.
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IUIApplication))
            *ppv = static_cast<IUIApplication*>(this);
        else if (IsEqualIID(riid, IID_IUICommandHandler))
            *ppv = static_cast<IUICommandHandler*>(this);
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&m_ref);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // ---- IUIApplication ---------------------------------------------------
    STDMETHODIMP OnViewChanged(UINT viewId, UI_VIEWTYPE typeId, IUnknown* pView,
                               UI_VIEWVERB verb, INT32 /*uReasonCode*/) override
    {
        if (viewId != ID_RIBBON_ROOT || typeId != UI_VIEWTYPE_RIBBON)
            return S_OK;

        switch (verb)
        {
        case UI_VIEWVERB_CREATE:
            // The ribbon now exists and has a height. The frame must re-lay-out
            // or the edit control stays where it was before the ribbon appeared.
            if (m_frame) m_frame->LayoutChildren();
            break;

        case UI_VIEWVERB_SIZE:
            // Fired on every ribbon height change: minimise/restore, tab
            // switching in minimised mode, DPI change, window resize.
            if (m_frame) m_frame->LayoutChildren();
            break;

        case UI_VIEWVERB_DESTROY:
            // The framework is going away. Drop the frame pointer so a late
            // UpdateProperty cannot reach a destroyed window.
            m_frame = nullptr;
            break;

        case UI_VIEWVERB_ERROR:
            // The ribbon failed to render. There is nothing useful to do from
            // here: InitRibbon already succeeded, so the fallback toolbar
            // cannot be installed retroactively without recreating the window.
            break;
        }
        (void)pView;
        return S_OK;
    }

    STDMETHODIMP OnCreateUICommand(UINT /*nCmdID*/, UI_COMMANDTYPE /*typeID*/,
                                   IUICommandHandler** ppHandler) override
    {
        if (!ppHandler) return E_POINTER;
        // This object handles every command. Returning itself (with an AddRef,
        // which QueryInterface performs) is the normal pattern for a single-
        // handler ribbon host.
        return QueryInterface(IID_IUICommandHandler,
                              reinterpret_cast<void**>(ppHandler));
    }

    STDMETHODIMP OnDestroyUICommand(UINT /*nCmdID*/, UI_COMMANDTYPE /*typeID*/,
                                    IUICommandHandler* pHandler) override
    {
        if (pHandler) pHandler->Release();   // balances OnCreateUICommand's AddRef
        return S_OK;
    }

    // ---- IUICommandHandler::Execute ---------------------------------------
    STDMETHODIMP Execute(UINT nCmdID, UI_EXECUTIONVERB verb, const PROPERTYKEY* key,
                         const PROPVARIANT* value,
                         IUISimplePropertySet* /*pExecProps*/) override
    {
        if (!m_frame) return E_UNEXPECTED;

        // CANCEL arrives when the user starts an action and abandons it (a
        // colour picker dismissed with Escape). Nothing to undo for any command
        // LetterPad has, so it is acknowledged and ignored.
        if (verb == UI_EXECUTIONVERB_CANCEL)
            return S_OK;

        // CATALOG is only sent for gallery controls, of which there are none.
        if (verb == UI_EXECUTIONVERB_CATALOG)
            return S_OK;

        if (verb != UI_EXECUTIONVERB_EXECUTE)
            return S_OK;

        // ---------------------------------------------------------------
        //  Toggle buttons and check boxes: the new checked state arrives in
        //  `value` under UI_PKEY_Boolean. Reading it is mandatory -- executing
        //  a toggle as "flip the current state" would double-flip, because the
        //  framework has already computed the new state and is telling us what
        //  it decided.
        // ---------------------------------------------------------------
        if (key && IsEqualPropertyKey(*key, UI_PKEY_Boolean) && value)
        {
            bool on = (value->vt == VT_BOOL) ? (value->boolVal == VARIANT_TRUE) : false;
            HandleToggle(nCmdID, on);
            return S_OK;
        }

        // ---------------------------------------------------------------
        //  Font control: every change arrives as a property, not a command.
        //  The framework sends the specific key that changed, so this dispatch
        //  is on the key rather than on nCmdID.
        // ---------------------------------------------------------------
        if (nCmdID == ID_CMD_FONTCONTROL && key)
            return ExecuteFontProperty(*key, value);

        // ---------------------------------------------------------------
        //  Everything else is a plain command. Forwarded to the frame's single
        //  dispatch switch, which is the same function the accelerator table
        //  and the fallback toolbar call -- that is the whole point of the
        //  shared ID space.
        // ---------------------------------------------------------------
        m_frame->OnCommand(nCmdID);
        return S_OK;
    }

    // ---- IUICommandHandler::UpdateProperty --------------------------------
    STDMETHODIMP UpdateProperty(UINT nCmdID, REFPROPERTYKEY key,
                                const PROPVARIANT* pCurrentValue,
                                PROPVARIANT* pNewValue) override
    {
        if (!pNewValue) return E_POINTER;
        if (!m_frame)
        {
            // The frame is gone. Answering with the current value is safer than
            // failing: a failed UpdateProperty during teardown can leave the
            // framework holding a half-initialised control.
            PropVariantCopyShallow(pNewValue, pCurrentValue);
            return S_OK;
        }

        // ---- labels, tooltips and images ---------------------------------
        // All defined in Ribbon.xml. The framework queries them anyway (it does
        // not cache markup values across invalidations), and the correct answer
        // is "the value you already have".
        if (IsEqualPropertyKey(key, UI_PKEY_Label) ||
            IsEqualPropertyKey(key, UI_PKEY_LabelTitle) ||
            IsEqualPropertyKey(key, UI_PKEY_LabelDescription) ||
            IsEqualPropertyKey(key, UI_PKEY_TooltipTitle) ||
            IsEqualPropertyKey(key, UI_PKEY_TooltipDescription) ||
            IsEqualPropertyKey(key, UI_PKEY_Keytip) ||
            IsEqualPropertyKey(key, UI_PKEY_LargeImage) ||
            IsEqualPropertyKey(key, UI_PKEY_SmallImage) ||
            IsEqualPropertyKey(key, UI_PKEY_LargeHighContrastImage) ||
            IsEqualPropertyKey(key, UI_PKEY_SmallHighContrastImage))
        {
            PropVariantCopyShallow(pNewValue, pCurrentValue);
            return S_OK;
        }

        // ---- enabled state ------------------------------------------------
        if (IsEqualPropertyKey(key, UI_PKEY_Enabled))
        {
            pNewValue->vt      = VT_BOOL;
            pNewValue->boolVal = IsCommandEnabled(nCmdID) ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;
        }

        // ---- checked state for toggles and check boxes ---------------------
        if (IsEqualPropertyKey(key, UI_PKEY_Boolean))
        {
            pNewValue->vt      = VT_BOOL;
            pNewValue->boolVal = IsCommandChecked(nCmdID) ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;
        }

        // ---- font control properties --------------------------------------
        if (nCmdID == ID_CMD_FONTCONTROL)
            return UpdateFontProperty(key, pCurrentValue, pNewValue);

        // ---- everything else: no opinion ----------------------------------
        PropVariantCopyShallow(pNewValue, pCurrentValue);
        return S_OK;
    }

private:
    // -----------------------------------------------------------------------
    //  Enabled / checked state, pulled from the document
    // -----------------------------------------------------------------------
    bool IsCommandEnabled(UINT id) const
    {
        EditView& v = m_frame->View();
        if (!v) return false;

        switch (id)
        {
        case ID_CMD_CUT:
        case ID_CMD_COPY:
            return v.HasSelection();

        case ID_CMD_PASTE:
            return v.CanPaste();

        case ID_CMD_UNDO:
            return SendMessageW(v.Hwnd(), EM_CANUNDO, 0, 0) != 0;

        case ID_CMD_REDO:
            // EM_CANREDO is not available on every RichEdit version. Reporting
            // "always enabled" and letting the command no-op is less jarring
            // than a permanently greyed Redo button.
            return true;

        case ID_CMD_FIND:
        case ID_CMD_REPLACE:
        case ID_CMD_SELECTALL:
        case ID_CMD_SAVE:
        case ID_CMD_SAVEAS:
        case ID_CMD_PRINT:
        case ID_CMD_QUICKPRINT:
        case ID_CMD_PRINTPREVIEW:
        case ID_CMD_PAGESETUP:
        case ID_CMD_NEW:
        case ID_CMD_OPEN:
        case ID_CMD_GOTO:
        case ID_CMD_DATETIME:
        case ID_CMD_PICTURE:
        case ID_CMD_OBJECT:
            return true;

        case ID_CMD_CLOSE:
        case ID_CMD_OPTIONS:
        case ID_CMD_EXIT:
        case ID_CMD_ZOOMIN:
        case ID_CMD_ZOOMOUT:
        case ID_CMD_ZOOM100:
        case ID_CMD_RULER:
        case ID_CMD_STATUSBAR:
            return true;

        default:
            return true;   // unknown commands are enabled rather than hidden
        }
    }

    bool IsCommandChecked(UINT id) const
    {
        EditView& v = m_frame->View();
        if (!v) return false;

        // The document's current formatting, read once. Several of these cases
        // need it and each EM_GETCHARFORMAT is a message round trip.
        CHARFORMAT2W cf;
        v.GetCharFormat(cf);

        PARAFORMAT2 pf;
        v.GetParaFormat(pf);

        switch (id)
        {
        case ID_CMD_BOLD:       return (cf.dwEffects & CFE_BOLD) != 0;
        case ID_CMD_ITALIC:     return (cf.dwEffects & CFE_ITALIC) != 0;
        case ID_CMD_UNDERLINE:  return (cf.dwEffects & CFE_UNDERLINE) != 0;
        case ID_CMD_STRIKEOUT:  return (cf.dwEffects & CFE_STRIKEOUT) != 0;
        case ID_CMD_SUBSCRIPT:  return (cf.dwEffects & CFE_SUBSCRIPT) != 0;
        case ID_CMD_SUPERSCRIPT:return (cf.dwEffects & CFE_SUPERSCRIPT) != 0;

        case ID_CMD_BULLETS:    return pf.wNumbering == PFN_BULLET;

        case ID_CMD_ALIGN_LEFT:   return pf.wAlignment == PFA_LEFT;
        case ID_CMD_ALIGN_CENTER: return pf.wAlignment == PFA_CENTER;
        case ID_CMD_ALIGN_RIGHT:  return pf.wAlignment == PFA_RIGHT;
        case ID_CMD_ALIGN_JUSTIFY:return pf.wAlignment == PFA_JUSTIFY;

        // View toggles reflect the settings, not the document.
        case ID_CMD_RULER:        return m_frame->Settings().fShowRuler != FALSE;
        case ID_CMD_STATUSBAR:    return m_frame->Settings().fShowStatusBar != FALSE;

        case ID_CMD_WRAP_NONE:    return m_frame->Settings().wrap == kWrapNone;
        case ID_CMD_WRAP_WINDOW:  return m_frame->Settings().wrap == kWrapWindow;
        case ID_CMD_WRAP_RULER:   return m_frame->Settings().wrap == kWrapRuler;

        default: return false;
        }
    }

    // -----------------------------------------------------------------------
    //  Toggle execution
    //
    //  The framework has already decided the new state and is telling us what
    //  it is. Applying it absolutely -- rather than flipping -- is correct here
    //  and is the opposite of what EditView::ToggleBold does for keyboard
    //  accelerators. The difference is not an inconsistency: the ribbon knows
    //  the target state, a keystroke does not.
    // -----------------------------------------------------------------------
    void HandleToggle(UINT id, bool on)
    {
        EditView& v = m_frame->View();
        if (!v) return;

        CHARFORMAT2W cf;
        v.GetCharFormat(cf);
        cf.cbSize = sizeof(cf);

        switch (id)
        {
        case ID_CMD_BOLD:
            cf.dwMask    = CFM_BOLD;
            cf.dwEffects = on ? (cf.dwEffects | CFE_BOLD) : (cf.dwEffects & ~CFE_BOLD);
            v.SetCharFormat(cf);
            break;

        case ID_CMD_ITALIC:
            cf.dwMask    = CFM_ITALIC;
            cf.dwEffects = on ? (cf.dwEffects | CFE_ITALIC) : (cf.dwEffects & ~CFE_ITALIC);
            v.SetCharFormat(cf);
            break;

        case ID_CMD_STRIKEOUT:
            cf.dwMask    = CFM_STRIKEOUT;
            cf.dwEffects = on ? (cf.dwEffects | CFE_STRIKEOUT)
                              : (cf.dwEffects & ~CFE_STRIKEOUT);
            v.SetCharFormat(cf);
            break;

        case ID_CMD_UNDERLINE:
            cf.dwMask         = CFM_UNDERLINE | CFM_UNDERLINETYPE;
            cf.dwEffects      = on ? (cf.dwEffects | CFE_UNDERLINE)
                                   : (cf.dwEffects & ~CFE_UNDERLINE);
            cf.bUnderlineType = on ? CFU_UNDERLINE : CFU_UNDERLINENONE;
            v.SetCharFormat(cf);
            break;

        case ID_CMD_SUBSCRIPT:
            cf.dwMask    = CFM_OFFSET;
            cf.dwEffects = on ? ((cf.dwEffects & ~CFE_SUPERSCRIPT) | CFE_SUBSCRIPT)
                              : (cf.dwEffects & ~(CFE_SUBSCRIPT | CFE_SUPERSCRIPT));
            v.SetCharFormat(cf);
            break;

        case ID_CMD_SUPERSCRIPT:
            cf.dwMask    = CFM_OFFSET;
            cf.dwEffects = on ? ((cf.dwEffects & ~CFE_SUBSCRIPT) | CFE_SUPERSCRIPT)
                              : (cf.dwEffects & ~(CFE_SUBSCRIPT | CFE_SUPERSCRIPT));
            v.SetCharFormat(cf);
            break;

        case ID_CMD_BULLETS:
        {
            PARAFORMAT2 pf;
            v.GetParaFormat(pf);
            bool currently = (pf.wNumbering == PFN_BULLET);
            if (currently != on)
                v.ToggleBullets();     // ToggleBullets also manages the indent
            break;
        }

        case ID_CMD_ALIGN_LEFT:   v.SetAlignment(PFA_LEFT);    break;
        case ID_CMD_ALIGN_CENTER: v.SetAlignment(PFA_CENTER);  break;
        case ID_CMD_ALIGN_RIGHT:  v.SetAlignment(PFA_RIGHT);   break;
        case ID_CMD_ALIGN_JUSTIFY:v.SetAlignment(PFA_JUSTIFY); break;

        case ID_CMD_RULER:
        case ID_CMD_STATUSBAR:
        case ID_CMD_WRAP_NONE:
        case ID_CMD_WRAP_WINDOW:
        case ID_CMD_WRAP_RULER:
            // These are view toggles handled by the frame. Routing them through
            // OnCommand keeps their implementation in one place even though the
            // ribbon already knows the target state -- the frame re-reads the
            // setting it just flipped, which is correct because OnCommand is
            // written as a toggle.
            if (IsCommandChecked(id) != on)
                m_frame->OnCommand(id);
            break;

        default:
            break;
        }

        m_frame->UpdateCaption();
        m_frame->UpdateStatusBar();
    }

    // -----------------------------------------------------------------------
    //  Font control -- UpdateProperty (framework asks what the state is)
    // -----------------------------------------------------------------------
    HRESULT UpdateFontProperty(REFPROPERTYKEY key, const PROPVARIANT* pCurrent,
                               PROPVARIANT* pNew)
    {
        EditView& v = m_frame->View();
        if (!v)
        {
            PropVariantCopyShallow(pNew, pCurrent);
            return S_OK;
        }

        CHARFORMAT2W cf;
        v.GetCharFormat(cf);

        // The composite "does this control have anything to report" property.
        // NOTAVAILABLE would make the whole font box render greyed out, which
        // is wrong: there is always a current font at the insertion point.
        if (IsEqualPropertyKey(key, UI_PKEY_FontProperties))
        {
            pNew->vt    = VT_UI4;
            pNew->ulVal = UI_FONTPROPERTIES_SET;
            return S_OK;
        }

        if (IsEqualPropertyKey(key, UI_PKEY_FontFamily))
        {
            pNew->vt      = VT_BSTR;
            pNew->bstrVal = SysAllocString(cf.szFaceName);
            if (!pNew->bstrVal) { pNew->vt = VT_EMPTY; return E_OUTOFMEMORY; }
            return S_OK;
        }

        if (IsEqualPropertyKey(key, UI_PKEY_FontSize))
        {
            // yHeight is twips; the ribbon wants points as VT_DECIMAL.
            double pts = static_cast<double>(cf.yHeight) / kTwipsPerPt;
            if (pts <= 0) pts = 11.0;
            PropVariantSetDecimal(pNew, pts);
            return S_OK;
        }

        if (IsEqualPropertyKey(key, UI_PKEY_FontBold))
        {
            pNew->vt    = VT_UI4;
            pNew->ulVal = FontStateFromBool((cf.dwEffects & CFE_BOLD) != 0);
            return S_OK;
        }

        if (IsEqualPropertyKey(key, UI_PKEY_FontItalic))
        {
            pNew->vt    = VT_UI4;
            pNew->ulVal = FontStateFromBool((cf.dwEffects & CFE_ITALIC) != 0);
            return S_OK;
        }

        if (IsEqualPropertyKey(key, UI_PKEY_FontStrikethrough))
        {
            pNew->vt    = VT_UI4;
            pNew->ulVal = FontStateFromBool((cf.dwEffects & CFE_STRIKEOUT) != 0);
            return S_OK;
        }

        if (IsEqualPropertyKey(key, UI_PKEY_FontUnderline))
        {
            pNew->vt    = VT_UI4;
            pNew->ulVal = UnderlineStateFromType(cf.bUnderlineType);
            return S_OK;
        }

        if (IsEqualPropertyKey(key, UI_PKEY_FontVerticalPositioning))
        {
            pNew->vt    = VT_UI4;
            pNew->ulVal = VerticalStateFromEffects(cf.dwEffects);
            return S_OK;
        }

        if (IsEqualPropertyKey(key, UI_PKEY_FontForegroundColor))
        {
            pNew->vt    = VT_UI4;
            // CFM_AUTOCOLOR set means "follow the system text colour". The
            // ribbon has no "automatic" swatch in a plain colour property, so
            // black is reported -- which is what automatic resolves to on any
            // non-high-contrast theme.
            pNew->ulVal = (cf.dwEffects & CFE_AUTOCOLOR) ? RGB(0, 0, 0) : cf.crTextColor;
            return S_OK;
        }

        if (IsEqualPropertyKey(key, UI_PKEY_FontBackgroundColor))
        {
            pNew->vt    = VT_UI4;
            // A background of "no highlight" is reported as white rather than as
            // an unset value, because the ribbon's highlight swatch has no
            // "none" state that round-trips through this property.
            pNew->ulVal = (cf.crBackColor == 0) ? RGB(255, 255, 255) : cf.crBackColor;
            return S_OK;
        }

        // Kerning, spacing, style and the underline COLOUR are all queried by a
        // RichFont control. Reporting the current value unchanged is correct and
        // keeps the control from rendering them as unavailable.
        PropVariantCopyShallow(pNew, pCurrent);
        return S_OK;
    }

    // -----------------------------------------------------------------------
    //  Font control -- Execute (user changed something)
    //
    //  `value` is the new state the framework wants applied. Each key maps onto
    //  a CHARFORMAT2W field plus its mask bit. The mask bit is set per-property
    //  rather than in one broad write, because writing CFM_BOLD alongside a
    //  font-size change would apply whatever stale bold state the struct
    //  happened to carry.
    // -----------------------------------------------------------------------
    HRESULT ExecuteFontProperty(const PROPERTYKEY& key, const PROPVARIANT* value)
    {
        EditView& v = m_frame->View();
        if (!v || !value) return S_OK;

        CHARFORMAT2W cf;
        v.GetCharFormat(cf);
        cf.cbSize = sizeof(cf);

        if (IsEqualPropertyKey(key, UI_PKEY_FontFamily))
        {
            if (value->vt != VT_BSTR || !value->bstrVal) return S_OK;
            cf.dwMask = CFM_FACE;
            wcsncpy_s(cf.szFaceName, _countof(cf.szFaceName), value->bstrVal, _TRUNCATE);
            v.SetCharFormat(cf);
        }
        else if (IsEqualPropertyKey(key, UI_PKEY_FontSize))
        {
            double pts = PropVariantGetDecimal(value);
            if (pts < 1.0)   pts = 1.0;      // RichEdit rejects a zero height
            if (pts > 1638.0) pts = 1638.0;  // its documented maximum
            cf.dwMask  = CFM_SIZE;
            cf.yHeight = static_cast<LONG>(pts * kTwipsPerPt + 0.5);
            v.SetCharFormat(cf);
        }
        else if (IsEqualPropertyKey(key, UI_PKEY_FontBold))
        {
            UINT st = (value->vt == VT_UI4) ? value->ulVal : UI_FONTWEIGHT_NOTSET;
            if (st == UI_FONTWEIGHT_CHANGED) return S_OK;   // mixed: nothing to apply
            cf.dwMask    = CFM_BOLD;
            cf.dwEffects = (st == UI_FONTWEIGHT_SET) ? (cf.dwEffects | CFE_BOLD)
                                                     : (cf.dwEffects & ~CFE_BOLD);
            v.SetCharFormat(cf);
        }
        else if (IsEqualPropertyKey(key, UI_PKEY_FontItalic))
        {
            UINT st = (value->vt == VT_UI4) ? value->ulVal : UI_FONTWEIGHT_NOTSET;
            if (st == UI_FONTWEIGHT_CHANGED) return S_OK;
            cf.dwMask    = CFM_ITALIC;
            cf.dwEffects = (st == UI_FONTWEIGHT_SET) ? (cf.dwEffects | CFE_ITALIC)
                                                     : (cf.dwEffects & ~CFE_ITALIC);
            v.SetCharFormat(cf);
        }
        else if (IsEqualPropertyKey(key, UI_PKEY_FontStrikethrough))
        {
            UINT st = (value->vt == VT_UI4) ? value->ulVal : UI_FONTWEIGHT_NOTSET;
            if (st == UI_FONTWEIGHT_CHANGED) return S_OK;
            cf.dwMask    = CFM_STRIKEOUT;
            cf.dwEffects = (st == UI_FONTWEIGHT_SET) ? (cf.dwEffects | CFE_STRIKEOUT)
                                                     : (cf.dwEffects & ~CFE_STRIKEOUT);
            v.SetCharFormat(cf);
        }
        else if (IsEqualPropertyKey(key, UI_PKEY_FontUnderline))
        {
            UINT st = (value->vt == VT_UI4) ? value->ulVal : UI_FONTUNDERLINE_NOTSET;
            if (st == UI_FONTUNDERLINE_CHANGED) return S_OK;
            bool on = (st != UI_FONTUNDERLINE_NOTSET && st != UI_FONTUNDERLINE_NOTAVAILABLE);
            cf.dwMask         = CFM_UNDERLINE | CFM_UNDERLINETYPE;
            cf.dwEffects      = on ? (cf.dwEffects | CFE_UNDERLINE)
                                   : (cf.dwEffects & ~CFE_UNDERLINE);
            // The ribbon can report a specific underline style. Mapping the
            // common ones is worth doing; anything unrecognised falls back to a
            // plain single underline rather than dropping the change.
            switch (st)
            {
            case UI_FONTUNDERLINE_DOUBLE:     cf.bUnderlineType = CFU_UNDERLINEDOUBLE;   break;
            case UI_FONTUNDERLINE_DOTTED:     cf.bUnderlineType = CFU_UNDERLINEDOTTED;   break;
            case UI_FONTUNDERLINE_THICK:      cf.bUnderlineType = CFU_UNDERLINETHICK;    break;
            case UI_FONTUNDERLINE_WAVE:       cf.bUnderlineType = CFU_UNDERLINEWAVE;     break;
            case UI_FONTUNDERLINE_WORDS:      cf.bUnderlineType = CFU_UNDERLINEWORDS;    break;
            default: cf.bUnderlineType = on ? CFU_UNDERLINE : CFU_UNDERLINENONE; break;
            }
            v.SetCharFormat(cf);
        }
        else if (IsEqualPropertyKey(key, UI_PKEY_FontVerticalPositioning))
        {
            UINT st = (value->vt == VT_UI4) ? value->ulVal
                                            : UI_FONTVERTICALPOSITION_NOTSET;
            cf.dwMask = CFM_OFFSET;
            cf.dwEffects &= ~(CFE_SUBSCRIPT | CFE_SUPERSCRIPT);
            if (st == UI_FONTVERTICALPOSITION_SUPERSCRIPT) cf.dwEffects |= CFE_SUPERSCRIPT;
            else if (st == UI_FONTVERTICALPOSITION_SUBSCRIPT) cf.dwEffects |= CFE_SUBSCRIPT;
            v.SetCharFormat(cf);
        }
        else if (IsEqualPropertyKey(key, UI_PKEY_FontForegroundColor))
        {
            COLORREF cr = ColourFromPropVariant(value);
            // Clear CFE_AUTOCOLOR when the user picks an explicit colour, or
            // RichEdit keeps overriding it with the system text colour and the
            // change appears to do nothing.
            cf.dwMask     = CFM_COLOR | CFM_AUTOCOLOR;
            cf.dwEffects &= ~CFE_AUTOCOLOR;
            cf.crTextColor = cr;
            v.SetCharFormat(cf);
        }
        else if (IsEqualPropertyKey(key, UI_PKEY_FontBackgroundColor))
        {
            cf.dwMask      = CFM_BACKCOLOR;
            cf.crBackColor = ColourFromPropVariant(value);
            v.SetCharFormat(cf);
        }
        // Any other font key (kerning, spacing, style, underline colour) is
        // accepted and ignored. Returning S_OK without applying is deliberate:
        // returning an error makes the ribbon mark the control as failed.

        m_frame->UpdateCaption();
        m_frame->UpdateStatusBar();
        return S_OK;
    }

private:
    AppFrame* m_frame;
    LONG      m_ref;
};

// ===========================================================================
//  RibbonApp_Create
// ===========================================================================
HRESULT RibbonApp_Create(IUIFramework* /*fw*/, AppFrame* frame, IUIApplication** ppApp)
{
    if (!ppApp) return E_POINTER;
    *ppApp = nullptr;
    if (!frame) return E_INVALIDARG;

    // Constructed with a refcount of 1, which QueryInterface immediately bumps
    // to 2. The trailing Release brings it back to 1, held by the framework.
    // Getting this dance wrong either leaks the handler or destroys it before
    // the framework has stored it.
    RibbonHandler* p = new (std::nothrow) RibbonHandler(frame);
    if (!p) return E_OUTOFMEMORY;

    HRESULT hr = p->QueryInterface(IID_IUIApplication,
                                   reinterpret_cast<void**>(ppApp));
    p->Release();

    if (FAILED(hr)) *ppApp = nullptr;
    return hr;
}
