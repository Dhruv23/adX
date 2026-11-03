# Build the adX MSI (phase_10.md 4.8).
#
#   powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1 [-Out build\installer]
#
# Stages an embedded Python 3.12 with adX's wheel and its runtime packages, the
# examples and the licence files, then harvests the stage with WiX 3.14 (heat, candle,
# light) into adX-<version>.msi. The embeddable Python and WiX are fetched into
# %LOCALAPPDATA%\adx-tools and checked against the hashes below.

param(
    [string]$Out = "build\installer"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Tools = Join-Path $env:LOCALAPPDATA "adx-tools"
$Out = Join-Path $Root $Out
$Stage = Join-Path $Out "stage"

$PythonZip = "python-3.12.10-embed-amd64.zip"
$PythonUrl = "https://www.python.org/ftp/python/3.12.10/$PythonZip"
$PythonSha = "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3"
$WixUrl = "https://github.com/wixtoolset/wix3/releases/download/wix3141rtm/wix314-binaries.zip"
$WixSha = "6ac824e1642d6f7277d0ed7ea09411a508f6116ba6fae0aa5f2c7daa2ff43d31"

function Fetch([string]$Url, [string]$Path, [string]$Sha) {
    if (-not (Test-Path $Path) -or (Get-FileHash $Path -Algorithm SHA256).Hash.ToLower() -ne $Sha) {
        New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
        Invoke-WebRequest -Uri $Url -OutFile $Path -UseBasicParsing
    }
    $got = (Get-FileHash $Path -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $Sha) { throw "$Path has SHA-256 $got, pinned $Sha" }
}

$CMakeText = Get-Content -Raw (Join-Path $Root "CMakeLists.txt")
$Version = [regex]::Match($CMakeText, 'project\(\s*adx[^)]*?VERSION\s+([0-9.]+)').Groups[1].Value
if (-not $Version) { throw "no project(adx VERSION ...) in CMakeLists.txt" }
Write-Host "adX $Version -> $Out"

# --- the tools -----------------------------------------------------------------------
Fetch $PythonUrl (Join-Path $Tools $PythonZip) $PythonSha
$WixZip = Join-Path $Tools "wix314\wix.zip"
Fetch $WixUrl $WixZip $WixSha
$Wix = Join-Path $Tools "wix314"
if (-not (Test-Path (Join-Path $Wix "candle.exe"))) { Expand-Archive $WixZip -DestinationPath $Wix -Force }

# --- the stage ----------------------------------------------------------------------
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
$Python = Join-Path $Stage "python"
Expand-Archive (Join-Path $Tools $PythonZip) -DestinationPath $Python
# The embeddable interpreter reads its paths from ._pth: add site-packages and site.
$Pth = Join-Path $Python "python312._pth"
@("python312.zip", ".", "Lib\site-packages", "import site") | Set-Content -Path $Pth -Encoding ascii
$SitePackages = Join-Path $Python "Lib\site-packages"
New-Item -ItemType Directory -Force $SitePackages | Out-Null

# adX's own wheel (the engine built Release, the plugin host beside it), then what it
# needs at run time, into the embedded interpreter's site-packages.
$Wheels = Join-Path $Out "wheels"
if (Test-Path $Wheels) { Remove-Item -Recurse -Force $Wheels }
& py -3.12 -m pip wheel $Root --no-deps -w $Wheels
if ($LASTEXITCODE -ne 0) { throw "building the adX wheel failed" }
$Wheel = (Get-ChildItem $Wheels -Filter "adx-*.whl" | Select-Object -First 1).FullName
& py -3.12 -m pip install --no-compile --only-binary ":all:" --target $SitePackages $Wheel
if ($LASTEXITCODE -ne 0) { throw "installing adX into the stage failed" }

# Compiled now, by the interpreter that will run them, so nothing is written into the
# install folder later and the uninstall leaves it empty.
& (Join-Path $Python "python.exe") -m compileall -q $SitePackages | Out-Null

Copy-Item -Recurse (Join-Path $Root "docs\examples") (Join-Path $Stage "examples")
Copy-Item (Join-Path $Root "installer\LICENSE.txt") $Stage
& py -3.12 (Join-Path $Root "tools\gen_third_party.py") | Out-Null
Copy-Item (Join-Path $Root "installer\THIRD-PARTY.md") $Stage

# The stage runs: the same check the MSI's test makes after installing.
& (Join-Path $Python "python.exe") -c "import adx, adx_engine; print('stage:', adx.__version__)"
if ($LASTEXITCODE -ne 0) { throw "the staged interpreter cannot import adx" }

# --- the MSI ------------------------------------------------------------------------
$Obj = Join-Path $Out "obj"
New-Item -ItemType Directory -Force $Obj | Out-Null
$Files = Join-Path $Obj "files.wxs"
& (Join-Path $Wix "heat.exe") dir $Stage -nologo -cg AppFiles -dr INSTALLDIR -gg -g1 -srd -sreg -scom -var var.Stage -out $Files
if ($LASTEXITCODE -ne 0) { throw "heat failed" }
& (Join-Path $Wix "candle.exe") -nologo -arch x64 "-dStage=$Stage" "-dVersion=$Version" -out "$Obj\" (Join-Path $Root "installer\adx.wxs") $Files
if ($LASTEXITCODE -ne 0) { throw "candle failed" }
$Msi = Join-Path $Out "adX-$Version.msi"
# A per-user install keys its components on files, which ICE38/64/91 flag for
# per-machine packages; the package is per-user only.
& (Join-Path $Wix "light.exe") -nologo -sice:ICE38 -sice:ICE64 -sice:ICE91 -out $Msi "$Obj\adx.wixobj" "$Obj\files.wixobj"
if ($LASTEXITCODE -ne 0) { throw "light failed" }
Write-Host "built $Msi ($([math]::Round((Get-Item $Msi).Length / 1MB)) MB)"
