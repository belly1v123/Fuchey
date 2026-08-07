#!/usr/bin/env pwsh
# ============================================================
# Fuchey — install-hooks.ps1
# Installs the pre-commit hook that blocks commits which could
# leak private key material to the serial console.
# Run: pwsh -NoProfile -File scripts/install-hooks.ps1
# ============================================================

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$hookDir  = Join-Path $repo '.git\hooks'
$hookPath = Join-Path $hookDir 'pre-commit'

if (-not (Test-Path -LiteralPath $hookDir)) {
    Write-Host "ERROR: not a git repository (.git\hooks missing)" -ForegroundColor Red
    exit 1
}

$hookContent = @'
#!/bin/sh
# Fuchey pre-commit hook: block commits that could leak key material.
ROOT="$(git rev-parse --show-toplevel)"
SCRIPT="$ROOT/scripts/check-no-secret-logging.ps1"
if ! command -v pwsh >/dev/null 2>&1; then
    echo "pre-commit: pwsh not found -- skipping secret-logging check"
    exit 0
fi
pwsh -NoProfile -File "$SCRIPT"
exit $?
'@

Set-Content -LiteralPath $hookPath -Value $hookContent -Encoding ASCII
Write-Host "Installed pre-commit hook: $hookPath" -ForegroundColor Green
