param(
    [string]$IdfPath = $env:IDF_PATH,
    [string]$ToolsPath = $env:IDF_TOOLS_PATH,
    [ValidateSet('real', 'sim')][string]$Profile = 'real',
    [string]$Python = '',
    [switch]$Menuconfig
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $IdfPath) {
    $localTools = Join-Path $env:USERPROFILE '.cache/conrad-tools'
    $localIdf = Join-Path $localTools 'esp-idf-v5.5.1'
    if (Test-Path -LiteralPath (Join-Path $localIdf 'tools/idf.py')) {
        $IdfPath = $localIdf
        if (-not $ToolsPath) { $ToolsPath = Join-Path $localTools 'idf-tools' }
    }
}
if (-not $IdfPath -or -not (Test-Path -LiteralPath (Join-Path $IdfPath 'tools/idf.py'))) {
    throw 'Set IDF_PATH or pass -IdfPath pointing to ESP-IDF v5.5.1. See docs/development.md.'
}
if (-not $ToolsPath) { $ToolsPath = Join-Path $env:USERPROFILE '.espressif' }
$env:IDF_PATH = (Resolve-Path -LiteralPath $IdfPath).Path
$env:IDF_TOOLS_PATH = $ToolsPath
if (-not $Python) {
    $candidate = Get-ChildItem -Path (Join-Path $ToolsPath 'python_env/idf5.5_py*_env/Scripts/python.exe') -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $candidate) { throw 'ESP-IDF Python environment missing. Run the ESP-IDF installer first.' }
    $Python = $candidate.FullName
}
$exportLines = & $Python (Join-Path $IdfPath 'tools/idf_tools.py') export --format key-value
if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF tools export failed.' }
foreach ($line in $exportLines) {
    if ($line -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') {
        $variableName = $Matches[1]
        $variableValue = $Matches[2].Replace('%PATH%', $env:PATH)
        [Environment]::SetEnvironmentVariable($variableName, $variableValue, 'Process')
    }
}
$buildRoot = Join-Path $repoRoot ('build/firmware-' + $Profile)
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
$defaults = Join-Path $repoRoot 'firmware/sdkconfig.defaults'
if ($Profile -eq 'sim') { $defaults += ';' + (Join-Path $repoRoot 'firmware/sdkconfig.sim.defaults') }
$task = if ($Menuconfig) { 'menuconfig' } else { 'build' }
Push-Location (Join-Path $repoRoot 'firmware')
try {
    $ErrorActionPreference = 'Continue'
    & $Python (Join-Path $IdfPath 'tools/idf.py') -B $buildRoot -D "SDKCONFIG=$buildRoot/sdkconfig" -D "SDKCONFIG_DEFAULTS=$defaults" $task
    $buildExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($buildExit -ne 0) { throw "ESP-IDF $task failed (exit $buildExit)." }
} finally { Pop-Location }
