#!/usr/bin/env pwsh
# ============================================================
# Fuchey — check-no-secret-logging.ps1
# Regression gate: fails if private key material can be dumped
# to the serial console via ESP log macros.
#
# Usage: pwsh -NoProfile -File scripts/check-no-secret-logging.ps1
# Exit code 0 = clean, 1 = secret logging detected.
#
# Deliberate, reviewed exceptions (app-level, string only):
#   - One-time mnemonic display at 'wallet_create' (main.cpp)
#   - 'wallet_export' base58 private-key reveal (explicit confirm)
# Both print via %s string logging, never raw byte dumps.
# ============================================================

$ErrorActionPreference = 'Stop'

$repo    = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$fwRoot  = Join-Path $repo 'firmware'
$ext     = @('*.c','*.cc','*.cpp','*.h','*.hpp')
$sources = Get-ChildItem -LiteralPath $fwRoot -Recurse -File -Include $ext

$blocked = @()

# Rule A — raw byte dumps are forbidden anywhere in the tree.
$blocked += $sources |
    Select-String -Pattern 'ESP_LOG_BUFFER_HEX' |
    ForEach-Object { "{0}:{1}: {2}" -f $_.Path.Replace($repo,'.'), $_.LineNumber, $_.Line.Trim() }

# Rule B — a secret variable passed as an argument after the log format
# string (the way key material would actually leak). The only exception is
# the deliberate one-time mnemonic display at wallet_create.
$secretToken = '\b(priv_key|priv|chain_code|out_secret|secret|seed|mnemonic)\b'
$logWithArg   = 'ESP_LOG\w*\s*\([^,]+,\s*"[^"]*",[^)]*'
$blocked += $sources |
    Where-Object { $_.FullName -match '\\src\\' -or $_.FullName -match '\\lib\\' } |
    Select-String -Pattern ($logWithArg + $secretToken) |
    Where-Object { $_.Line -notmatch 'mnemonic\.c_str\(\)' } |
    ForEach-Object { "{0}:{1}: {2}" -f $_.Path.Replace($repo,'.'), $_.LineNumber, $_.Line.Trim() }

if ($blocked.Count -gt 0) {
    Write-Host "SECRET-LOGGING DETECTED - commit blocked:" -ForegroundColor Red
    $blocked | ForEach-Object { Write-Host ("  " + $_) -ForegroundColor Red }
    exit 1
}

Write-Host "OK: no secret logging detected" -ForegroundColor Green
exit 0
