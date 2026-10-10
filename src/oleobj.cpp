// ============================================================================
//  oleobj.cpp -- object embedding and drag/drop
//
//  GETTING IRichEditOle
//  RichEdit exposes its OLE site through EM_GETOLEINTERFACE. The message name
//  is misleading: what it returns is an IUnknown*, NOT an IRichEditOle*. The
//  pointer must be QueryInterface'd for IRichEditOle and then Released. Treating
//  the raw result as IRichEditOle compiles and then calls through a vtable that
//  is offset by one interface, which crashes on the first method call.
//
//  INSERTING AN OBJECT
//  The sequence is fixed and each step matters:
//    1. IRichEditOle::GetClientSite   -- RichEdit's own client site. Using a
//                                        site of our own would detach the object
//                                        from the control's undo and save paths.
//    2. OleCreateFromFile             -- with that site, so the object is bound
//                                        to RichEdit from the moment it exists.
//    3. OleSetContainedDocument       -- marks it embedded rather than linked.
//                                        Skipping this leaves a linked object
//                                        that tries to reopen its source file.
//    4. IRichEditOle::InsertObject    -- with REOBJECT fully populated,
//                                        including clsid from GetUserClassID.
//
//  WHY NO OleUIInsertObject
//  The "Insert Object" dialog (create-new-or-from-file) is OleUIInsertObject,
//  whose OLEUIINSERTOBJECT structure carries an IRichEditOle pointer and several
//  fields whose exact spelling has moved between SDK releases. Rather than guess
//  at a struct layout that cannot be verified without the SDK headers in front
//  of us, InsertObject here takes the file-based path -- which needs no dialog
//  struct at all and uses exactly the same four steps as InsertPicture. The
//  "Create New" half of that dialog is a documented TODO in ROADMAP.md.
// ============================================================================
#include "stdafx.h"
#include "LetterPad.h"

namespace {

// ---------------------------------------------------------------------------
//  Retrieves and caches nothing: IRichEditOle is fetched fresh each time.
//  Caching it across calls would hold a COM reference that outlives the control
//  if the document is reloaded, and the pointer would dangle. The message round
//  trip is cheap relative to the object creation that follows it.
// ---------------------------------------------------------------------------
bool GetRichEditOle(HWND hEdit, ComPtr<IRichEditOle>& out)
{
    out = nullptr;
    if (!hEdit) return false;

    ComPtr<IUnknown> pUnk;
    SendMessageW(hEdit, EM_GETOLEINTERFACE, 0,
                 LParamFromPtr(reinterpret_cast<void**>(pUnk.Adopt())));
    if (!pUnk) return false;

    return SUCCEEDED(pUnk->QueryInterface(IID_IRichEditOle,
                                          reinterpret_cast<void**>(out.Adopt())));
}

// ---------------------------------------------------------------------------
//  The insertion sequence described in the file header.
//  Shared by picture insertion and file-based object insertion.
// ---------------------------------------------------------------------------
HRESULT InsertFileAsObject(HWND hEdit, PCWSTR path)
{
    if (!hEdit || !path || !path[0])
        return E_INVALIDARG;

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return HrFromWin32(ERROR_FILE_NOT_FOUND);

    ComPtr<IRichEditOle> pOle;
    if (!GetRichEditOle(hEdit, pOle))
        return E_FAIL;

    // Step 1: RichEdit's client site.
    ComPtr<IOleClientSite> pSite;
    HRESULT hr = pOle->GetClientSite(pSite.Adopt());
    if (FAILED(hr)) return hr;

    // Step 1b: a storage for the object.
    // OleCreateFromFile does NOT manufacture one when pStg is null -- the call
    // simply fails -- and RichEdit needs REOBJECT::pstg in order to serialise
    // the embedded object into RTF on save. Building it on an ILockBytes over a
    // fresh HGLOBAL gives an in-memory compound document whose lifetime matches
    // the REOBJECT's. (IRichEditOleCallback::GetNewStorage is the other supported
    // source; using it would mean implementing the callback, which buys nothing
    // for the from-file case.)
    ComPtr<ILockBytes> pLock;
    hr = CreateILockBytesOnHGlobal(nullptr, TRUE, pLock.Adopt());
    if (FAILED(hr)) return hr;

    ComPtr<IStorage> pStg;
    hr = StgCreateDocfileOnILockBytes(pLock.Get(),
                                      STGM_SHARE_EXCLUSIVE | STGM_CREATE | STGM_READWRITE,
                                      0, pStg.Adopt());
    if (FAILED(hr)) return hr;

    // Step 2: create the object bound to that site and that storage.
    //   CLSID_NULL       let OLE choose the handler from the file extension
    //   OLERENDER_DRAW   the object renders itself; do not pre-render to a
    //                    metafile, which would freeze a picture at its current
    //                    size and lose the ability to edit it in place
    ComPtr<IOleObject> pObj;
    hr = OleCreateFromFile(CLSID_NULL, path, IID_IOleObject, OLERENDER_DRAW,
                           nullptr, pSite.Get(), pStg.Get(),
                           reinterpret_cast<void**>(pObj.Adopt()));
    if (FAILED(hr) || !pObj)
        return hr;

    // Step 3: embedded, not linked.
    hr = OleSetContainedDocument(pObj.Get(), OLE_CONTAINED);
    if (FAILED(hr)) return hr;

    // The object must be running for RichEdit to query its extent and draw it.
    // Without SetObjectRects/OleRun the insertion succeeds but the object shows
    // as a blank rectangle until the document is reopened.
    pObj->Update();

    // Step 4: build the REOBJECT and insert it at the selection.
    REOBJECT reobj = {};
    reobj.cbStruct   = sizeof(reobj);
    // REO_IOCP_SELECTION inserts at the current selection and replaces it,
    // which is the behaviour every "insert" command has.
    reobj.cp         = REO_IOCP_SELECTION;
    reobj.dvaspect   = DVASPECT_CONTENT;
    reobj.poleobj    = pObj.Get();
    reobj.polesite   = pSite.Get();
    // The storage created above. Without it the object has nowhere to live and
    // RichEdit cannot write it into the RTF stream on save.
    reobj.pstg       = pStg.Get();
    // REO_BELOWBASELINE puts the object on the text baseline rather than
    // floating, which is what a word processor wants for an inline picture.
    // REO_RESIZABLE lets the user drag its sizing handles.
    reobj.dwFlags    = REO_BELOWBASELINE | REO_RESIZABLE | REO_USEALTRESOLUTION;

    // clsid must come from the object itself. Assuming it from the extension is
    // wrong for any file whose handler is registered against a different CLSID,
    // and RichEdit uses it to decide how to serialise the object into RTF.
    hr = pObj->GetUserClassID(&reobj.clsid);
    if (FAILED(hr))
        reobj.clsid = CLSID_NULL;      // RichEdit tolerates a null clsid

    hr = pOle->InsertObject(&reobj);
    if (FAILED(hr)) return hr;

    // The control does not mark itself modified for an OLE insertion performed
    // through its own interface, so the caller would never be prompted to save.
    SendMessageW(hEdit, EM_SETMODIFY, TRUE, 0);
    return S_OK;
}

// ---------------------------------------------------------------------------
//  Drop target
//
//  Registered on the RichEdit HWND. RichEdit does not register its own drop
//  target, so without this, dragging a file onto the document does nothing --
//  not even a WM_DROPFILES, because OLE drag/drop suppresses the legacy message
//  once any drop target is registered on the window.
//
//  Only CF_HDROP is accepted. That covers every file drag from Explorer, which
//  is the case that matters. Accepting arbitrary IDataObjects would mean
//  handling embedded-object drags between RichEdit instances, which needs the
//  object's IStorage and is a substantially larger feature.
// ---------------------------------------------------------------------------
class DropTarget : public IDropTarget
{
public:
    explicit DropTarget(HWND hEdit) : m_hEdit(hEdit), m_ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropTarget))
        {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef()  override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // Reports whether this data object carries a file drop.
    static bool HasFileDrop(IDataObject* pData)
    {
        if (!pData) return false;
        FORMATETC fe = {};
        fe.cfFormat = CF_HDROP;
        fe.dwAspect = DVASPECT_CONTENT;
        fe.tymed    = TYMED_HGLOBAL;
        return pData->QueryGetData(&fe) == S_OK;
    }

    STDMETHODIMP DragEnter(IDataObject* pData, DWORD grfKeyState,
                           POINTL pt, DWORD* pdwEffect) override
    {
        (void)grfKeyState; (void)pt;
        if (!pdwEffect) return E_POINTER;
        // Record the decision here. DragOver is not given the data object again,
        // so it can only repeat what DragEnter concluded; without this flag the
        // cursor would flip to "no drop" the moment the mouse moved a pixel.
        m_fAccept = HasFileDrop(pData);
        *pdwEffect = m_fAccept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    STDMETHODIMP DragOver(DWORD /*grfKeyState*/, POINTL /*pt*/,
                          DWORD* pdwEffect) override
    {
        if (!pdwEffect) return E_POINTER;
        *pdwEffect = m_fAccept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    STDMETHODIMP DragLeave() override
    {
        m_fAccept = false;
        return S_OK;
    }

    STDMETHODIMP Drop(IDataObject* pData, DWORD /*grfKeyState*/,
                      POINTL /*pt*/, DWORD* pdwEffect) override
    {
        if (!pdwEffect) return E_POINTER;
        m_fAccept = false;

        if (!HasFileDrop(pData))
        {
            *pdwEffect = DROPEFFECT_NONE;
            return S_OK;
        }

        FORMATETC fe = {};
        fe.cfFormat = CF_HDROP;
        fe.dwAspect = DVASPECT_CONTENT;
        fe.tymed    = TYMED_HGLOBAL;

        STGMEDIUM stg = {};
        HRESULT hr = pData->GetData(&fe, &stg);
        if (FAILED(hr))
        {
            *pdwEffect = DROPEFFECT_NONE;
            return hr;
        }

        HDROP hDrop = static_cast<HDROP>(stg.hGlobal);
        UINT nFiles = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);

        // Only the first file is inserted. Dropping several would stack them all
        // at the same insertion point with no separator, which produces a run of
        // objects the user cannot tell apart. Inserting one and ignoring the
        // rest is predictable; inserting all would need an explicit design
        // decision about separators.
        if (nFiles > 0)
        {
            WCHAR path[MAX_PATH] = {};
            if (DragQueryFileW(hDrop, 0, path, _countof(path)) > 0)
            {
                if (SUCCEEDED(InsertFileAsObject(m_hEdit, path)))
                    *pdwEffect = DROPEFFECT_COPY;
                else
                    *pdwEffect = DROPEFFECT_NONE;
            }
            else *pdwEffect = DROPEFFECT_NONE;
        }
        else *pdwEffect = DROPEFFECT_NONE;

        // ReleaseStgMedium frees the HGLOBAL. OLE allocated it and the contract
        // is that the drop target owns it for the duration of Drop.
        ReleaseStgMedium(&stg);
        return S_OK;
    }

    // Called by DragEnter to record the decision DragOver must repeat.
    void SetAccept(bool f) { m_fAccept = f; }

private:
    HWND  m_hEdit;
    LONG  m_ref;
    bool  m_fAccept = true;
};

} // anonymous namespace

// ===========================================================================
//  EditView OLE entry points
// ===========================================================================
HRESULT EditView::InsertPicture(PCWSTR path)
{
    // Pictures and other files take the same path. OLE resolves the handler from
    // the extension, so a .png becomes a Paint Picture object and a .docx
    // becomes whatever is registered for it. There is no special-casing of image
    // types here, and adding any would be a regression: the handler already
    // knows more about the file than an extension check could.
    HRESULT hr = InsertFileAsObject(m_hEdit, path);
    if (SUCCEEDED(hr) && m_frame)
    {
        m_frame->UpdateCaption();
        m_frame->UpdateStatusBar();
    }
    return hr;
}

HRESULT EditView::InsertObject(HWND hOwner)
{
    (void)hOwner;      // see the file header: no dialog struct is used

    // File-based insertion. The picker is unrestricted because an embedded
    // object can be any file type with a registered OLE handler, and filtering
    // would hide exactly the types the user is most likely to want.
    static const WCHAR kFilter[] =
        L"All Files\0*.*\0"
        L"Picture Files\0*.bmp;*.jpg;*.jpeg;*.gif;*.png;*.tif;*.tiff;*.wmf;*.emf\0"
        L"Documents\0*.doc;*.docx;*.xls;*.xlsx;*.ppt;*.pptx;*.pdf\0\0";

    WCHAR buf[kMaxPathBuffer] = {};   // see kMaxPathBuffer in LetterPad.h
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hOwner ? hOwner : GetParent(m_hEdit);
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = static_cast<DWORD>(_countof(buf));
    ofn.lpstrTitle  = L"Insert Object";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ENABLESIZING;

    if (!GetOpenFileNameW(&ofn))
        return S_FALSE;      // cancelled: not an error

    HRESULT hr = InsertFileAsObject(m_hEdit, buf);
    if (SUCCEEDED(hr) && m_frame)
    {
        m_frame->UpdateCaption();
        m_frame->UpdateStatusBar();
    }
    return hr;
}

// ===========================================================================
//  Drop target registration
//
//  RegisterDragDrop takes its own reference on the target, so the local ComPtr
//  releasing at the end of this function leaves exactly one reference held by
//  OLE. RevokeDragDrop releases it. Creating the target with a refcount of 1 and
//  letting the ComPtr drop it to the OLE-held 1 is the correct ownership dance;
//  constructing at 0 would destroy it before RegisterDragDrop could add its own.
// ===========================================================================
void Ole_RegisterDropTarget(HWND hEdit)
{
    if (!hEdit) return;

    ComPtr<IDropTarget> pTarget(new (std::nothrow) DropTarget(hEdit));
    if (!pTarget) return;

    // A failure here (usually DRAGDROP_E_ALREADYREGISTERED, if something else
    // beat us to it) is not fatal: the document still works, it just cannot
    // accept drops. Silent by design -- a message box on startup for a cosmetic
    // loss would be worse than the loss.
    RegisterDragDrop(hEdit, pTarget.Get());
}

void Ole_RevokeDropTarget(HWND hEdit)
{
    if (hEdit) RevokeDragDrop(hEdit);
}
