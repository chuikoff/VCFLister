$origPath = "vcf_view.cpp.orig"
$content = Get-Content $origPath -Raw

# 1) Add #include "vcf_theme.hpp" immediately after #include "vcf_view.hpp"
$content = [regex]::Replace($content, '(?m)^(#include "vcf_view\.hpp")$', '$1`r`n#include "vcf_theme.hpp"')

# 2) Remove entire duplicated theme block: from THEME comment through extern "C" VCFView_RefreshTheme line (inclusive)
$start = [regex]::Escape('// ===================== THEME (Auto + INI override) + Язык TC =====================')
$endLine = [regex]::Escape('extern "C" void VCFView_RefreshTheme')
$pattern = "(?s)$start.*?$endLine.*?(\r?\n)"
$content = [regex]::Replace($content, $pattern, "")

# 3) Insert PHOTO skip in BuildFromRawBlock after 'std::wstring headUp = ToUpperASCII(head);'
$insert = "        // Skip PHOTO fields — displayed separately in the photo panel`r`n        if (headUp.rfind(L`"PHOTO`", 0) == 0) continue;"
$content = [regex]::Replace($content, "(\s*std::wstring headUp = ToUpperASCII\(head\);)", "`$1`r`n$insert")

# Write final file
Set-Content "vcf_view.cpp" -Value $content -NoNewline
Write-Host "vcf_view.cpp regenerated successfully."
