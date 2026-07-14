$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$charsetFile = Join-Path $projectRoot "main/fonts/app_chinese_chars.txt"
$commonCharsetFile = Join-Path $projectRoot "main/fonts/gb2312_level1.txt"
$fontFile = Join-Path $projectRoot "managed_components/lvgl__lvgl/scripts/built_in_font/SimSun.woff"
$outputFile = Join-Path $projectRoot "main/fonts/app_font_chinese_16.c"

if (-not (Test-Path $fontFile)) {
    throw "SimSun.woff was not found. Run idf.py reconfigure first."
}

$extraSymbols = (Get-Content $charsetFile -Raw -Encoding UTF8) -replace '\s', ''
$commonSymbols = (Get-Content $commonCharsetFile -Raw -Encoding UTF8) -replace '\s', ''

if ($commonSymbols.Length -ne 3755) {
    throw "Expected 3755 GB2312 level-1 characters, got $($commonSymbols.Length) from $commonCharsetFile."
}
# GB2312 一级字库包含 3755 个最常用的简体汉字。相比只维护一小段手写字符表，
# 它能覆盖绝大多数普通话识别结果，同时仍能放进本项目的 2 MB Flash。
Write-Host "Generating $($commonSymbols.Length) common Chinese characters plus $($extraSymbols.Length) extra characters..."

# 16px fits a 240x240 LCD. 2bpp keeps the font smaller than 4bpp.
npx.cmd --yes lv_font_conv `
    --size 16 `
    --bpp 2 `
    --format lvgl `
    --font $fontFile `
    --range "0x20-0x7E" `
    --symbols $commonSymbols `
    --symbols $extraSymbols `
    --lv-font-name app_font_chinese_16 `
    --lv-include "lvgl.h" `
    --force-fast-kern-format `
    --no-compress `
    --no-prefilter `
    --output $outputFile

if ($LASTEXITCODE -ne 0) {
    throw "lv_font_conv failed with exit code $LASTEXITCODE."
}

Write-Host "Generated LVGL font: $outputFile"
