param(
    [string]$SearchPath = "C:\Windows\System32",
    [string]$Pattern = "Microsoft",
    [string]$Find4wPath = ".\build\Release\find4w.exe",
    [string]$RipgrepPath = "rg"
)

Write-Host "=== find4w benchmark ===" -ForegroundColor Cyan
Write-Host "Path:    $SearchPath"
Write-Host "Pattern: $Pattern"
Write-Host ""

Write-Host "--- find4w ---" -ForegroundColor Green
$f4w_time = Measure-Command { & $Find4wPath $Pattern $SearchPath --no-color 2>$null | Out-Null }
Write-Host ("  Time: {0:F1} ms" -f $f4w_time.TotalMilliseconds)

if (Get-Command $RipgrepPath -ErrorAction SilentlyContinue) {
    Write-Host "--- ripgrep ---" -ForegroundColor Yellow
    $rg_time = Measure-Command { & $RipgrepPath $Pattern $SearchPath --no-heading 2>$null | Out-Null }
    Write-Host ("  Time: {0:F1} ms" -f $rg_time.TotalMilliseconds)

    Write-Host ""
    $ratio = $rg_time.TotalMilliseconds / $f4w_time.TotalMilliseconds
    Write-Host ("find4w is {0:F2}x vs ripgrep" -f $ratio) -ForegroundColor Cyan
} else {
    Write-Host "ripgrep not found, skipping comparison" -ForegroundColor DarkGray
}
