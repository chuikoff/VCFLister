$src = Get-Content vcf_view.cpp -Raw
$src = $src -replace "case WM_KEYDOWN: {", "case WM_KEYDOWN: {`n        if (!st || st->contacts.empty()) return 0;`n        size_t sel = st->sel, count = st->contacts.size(); bool handled = false;`n        switch (w) {`n        case VK_ESCAPE: {`n            DestroyWindow(h);`n            return 0;`n        }`n        case VK_UP:"
$src = $src -replace "static WNDPROC g_EditOldProc = nullptr;.*EditSubclassProc.*?\}\$", ""
$src = $src -replace "g_EditOldProc = \(WNDPROC\)SetWindowLongPtrW.*", ""
$src = $src -replace "bool VCFView_CopyActive\(HWND\) \{ return false; \}", "bool VCFView_CopyActive(HWND h) { auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA); if (!st) return false; HWND hEditCtrl = st->hEdit; if (hEditCtrl && IsWindow(hEditCtrl)) { SendMessageW(hEditCtrl, WM_COPY, 0, 0); return true; } return false; }"
$src | Set-Content vcf_view.cpp -NoNewline
Write-Output done
