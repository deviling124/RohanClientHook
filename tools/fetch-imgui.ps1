$ErrorActionPreference = 'Stop'
$destination = Join-Path $PSScriptRoot '../vendor/imgui'
New-Item -ItemType Directory -Force "$destination/backends" | Out-Null
$files = @('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp','imgui.h','imgui_internal.h','imconfig.h','imstb_rectpack.h','imstb_textedit.h','imstb_truetype.h','LICENSE.txt','backends/imgui_impl_dx9.cpp','backends/imgui_impl_dx9.h','backends/imgui_impl_win32.cpp','backends/imgui_impl_win32.h')
foreach ($file in $files) {
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/ocornut/imgui/v1.92.5/$file" -OutFile (Join-Path $destination $file) -TimeoutSec 30
}
Get-ChildItem $destination -File -Recurse | Get-FileHash -Algorithm SHA256 | ForEach-Object { "$($_.Hash) $($_.Path.Substring((Resolve-Path $destination).Path.Length + 1))" } | Set-Content (Join-Path $destination 'SHA256SUMS.txt')
