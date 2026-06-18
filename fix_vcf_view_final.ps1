# Start from original backup
$lines = Get-Content "vcf_view.cpp.orig"

# 1) Insert #include "vcf_theme.hpp" after #include "vcf_view.hpp"
for ($i=0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^#include "vcf_view\.hpp"$') {
        $lines = $lines[0..$i] + @('#include "vcf_theme.hpp"') + $lines[($i+1)..($lines.Count-1)]
        break
    }
}

# 2) Remove theme block: find start comment and end extern line
$startIdx = -1
$endIdx = -1
for ($i=0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^// ===================== THEME ') { $startIdx = $i }
    if ($lines[$i] -match '^extern "C" void VCFView_RefreshTheme') { $endIdx = $i; break }
}
if ($startIdx -ge 0 -and $endIdx -ge $startIdx) {
    $lines = $lines[0..($startIdx-1)] + $lines[($endIdx+1)..($lines.Count-1)]
}

# 3) Insert PHOTO skip after line containing headUp assignment
for ($i=0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s*std::wstring headUp = ToUpperASCII\(head\);$') {
        $insert = @(
            '        // Skip PHOTO fields — displayed separately in the photo panel',
            '        if (headUp.rfind(L"PHOTO", 0) == 0) continue;'
        )
        $lines = $lines[0..$i] + $insert + $lines[($i+1)..($lines.Count-1)]
        break
    }
}

# Write final file using CRLF line endings
$final = ($lines -join "`r`n")
[System.IO.File]::WriteAllText("vcf_view.cpp", $final, [System.Text.Encoding]::UTF8)
Write-Host "vcf_view.cpp regenerated successfully."
