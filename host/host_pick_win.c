/* ===========================================================================
 * host_pick_win.c - the Windows Open dialog (IFileDialog).
 *
 * IFileDialog rather than the older GetOpenFileName: it is the dialog the rest
 * of Windows uses, it has the user's Quick Access and OneDrive places in it,
 * and it is the only one that can pick a FOLDER without the ancient
 * SHBrowseForFolder tree, which nobody has used willingly since Vista.
 *
 * Written against the C bindings (COBJMACROS) because this tree is C, not C++.
 * ======================================================================== */
#ifdef _WIN32

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

int host_pick_path(int want_folder, char *out, int cap)
{
    IFileOpenDialog *dlg = 0;
    IShellItem *item = 0;
    PWSTR wide = 0;
    HRESULT hr;
    DWORD opts = 0;
    int ok = 0;
    /* The editor may already have COM up on this thread (SDL does not), so
     * take whichever answer CoInitializeEx gives and only uninitialise if we
     * were the ones who initialised it. */
    HRESULT init = CoInitializeEx(0, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    hr = CoCreateInstance(&CLSID_FileOpenDialog, 0, CLSCTX_INPROC_SERVER,
                          &IID_IFileOpenDialog, (void **)&dlg);
    if (FAILED(hr)) goto done;

    if (SUCCEEDED(IFileOpenDialog_GetOptions(dlg, &opts))) {
        opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (want_folder) opts |= FOS_PICKFOLDERS;
        else             opts |= FOS_FILEMUSTEXIST;
        IFileOpenDialog_SetOptions(dlg, opts);
    }
    IFileOpenDialog_SetTitle(dlg, want_folder ? L"Open Folder" : L"Open File");

    hr = IFileOpenDialog_Show(dlg, 0);
    if (FAILED(hr)) goto done;            /* includes the user cancelling */
    if (FAILED(IFileOpenDialog_GetResult(dlg, &item))) goto done;
    if (FAILED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &wide))) goto done;

    /* UTF-16 -> UTF-8: the seam below this is bytes, and the editor now
     * handles UTF-8 throughout, so a path with an accent in it survives. */
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, cap, 0, 0) > 0) ok = 1;

done:
    if (wide) CoTaskMemFree(wide);
    if (item) IShellItem_Release(item);
    if (dlg)  IFileOpenDialog_Release(dlg);
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

#endif /* _WIN32 */
