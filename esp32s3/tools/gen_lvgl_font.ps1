$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$charsetFile = Join-Path $projectRoot "main/fonts/app_chinese_chars.txt"
$fontFile = Join-Path $projectRoot "managed_components/lvgl__lvgl/scripts/built_in_font/SimSun.woff"
$outputFile = Join-Path $projectRoot "main/fonts/app_font_chinese_16.c"

if (-not (Test-Path $fontFile)) {
    throw "SimSun.woff was not found. Run idf.py reconfigure first."
}

$symbols = (Get-Content $charsetFile -Raw -Encoding UTF8) -replace '\s', ''

# 16px fits a 240x240 LCD. 2bpp keeps the font smaller than 4bpp.
npx.cmd --yes lv_font_conv `
    --size 16 `
    --bpp 2 `
    --format lvgl `
    --font $fontFile `
    --range "0x20-0x7E" `
    --symbols $symbols `
    --lv-font-name app_font_chinese_16 `
    --lv-include "lvgl.h" `
    --force-fast-kern-format `
    --no-compress `
    --no-prefilter `
    --output $outputFile

Write-Host "Generated LVGL font: $outputFile"
