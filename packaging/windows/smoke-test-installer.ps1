# CI smoke test: silent install, check files, silent uninstall, check removal,
# then a CLAP-only install to check the component choice. Needs admin (CI runners have it).
param([Parameter(Mandatory = $true)][string]$Setup)
$ErrorActionPreference = 'Stop'
$vst3 = Join-Path $env:CommonProgramFiles 'VST3\Tone Legion Multiband.vst3'
$clap = Join-Path $env:CommonProgramFiles 'CLAP\Tone Legion Multiband.clap'
$app  = Join-Path $env:ProgramFiles 'Tone Legion\Multiband'

function Invoke-Quiet([string]$exe, [string[]]$extra) {
    $argList = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') + $extra
    $p = Start-Process -FilePath $exe -ArgumentList $argList -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "$exe exited with $($p.ExitCode)" }
}
function Assert([bool]$cond, [string]$what) {
    if (-not $cond) { throw "FAILED: $what" }
    Write-Host "ok: $what"
}
function Uninstall {
    Invoke-Quiet (Join-Path $app 'unins000.exe') @()
    # The uninstaller hands off to a temporary copy of itself; wait for it to finish.
    for ($i = 0; $i -lt 60 -and (Test-Path (Join-Path $app 'unins000.exe')); $i++) { Start-Sleep -Seconds 1 }
}

Invoke-Quiet $Setup @('/LOG=install-full.log')
Assert (Test-Path (Join-Path $vst3 'Contents\x86_64-win\Tone Legion Multiband.vst3')) 'VST3 bundle installed'
Assert (Test-Path $clap) 'CLAP installed'
Assert (Test-Path (Join-Path $app 'Tester notes.txt')) 'tester notes installed'
Uninstall
Assert (-not (Test-Path $vst3)) 'VST3 removed by uninstall'
Assert (-not (Test-Path $clap)) 'CLAP removed by uninstall'
Assert (-not (Test-Path $app)) 'install folder removed by uninstall'

Invoke-Quiet $Setup @('/COMPONENTS=clap', '/LOG=install-clap.log')
Assert (Test-Path $clap) 'CLAP-only install has CLAP'
Assert (-not (Test-Path $vst3)) 'CLAP-only install has no VST3'
Uninstall
Assert (-not (Test-Path $clap)) 'CLAP removed after CLAP-only install'
Write-Host 'Installer smoke test passed.'
