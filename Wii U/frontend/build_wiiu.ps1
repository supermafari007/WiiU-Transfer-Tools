$ErrorActionPreference = "Stop"

$DevkitPro = "C:/devkitPro/opt/devkitpro"
$env:DEVKITPRO = $DevkitPro
$env:DEVKITPPC = "$DevkitPro/devkitPPC"

$Gxx      = "$DevkitPro/devkitPPC/bin/powerpc-eabi-g++.exe"
$Elf2Rpl  = "$DevkitPro/tools/bin/elf2rpl.exe"
$WuhbTool = "$DevkitPro/tools/bin/wuhbtool.exe"

$Root = $PSScriptRoot
$BuildDir = Join-Path $Root "build"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$Src      = Join-Path $Root "source/main.cpp"
$Obj      = Join-Path $BuildDir "main.o"
$Elf      = Join-Path $Root "WiiU_Transfer.elf"
$Rpx      = Join-Path $Root "WiiU_Transfer.rpx"
$Wuhb     = Join-Path $Root "WiiU_Transfer.wuhb"
$OutCopy  = Join-Path (Split-Path $Root -Parent) "WiiU_Transfer.wuhb"

$IncProto = Join-Path (Split-Path $Root -Parent) "protocol"
$IncWut   = "$DevkitPro/wut/include"
$LibWut   = "$DevkitPro/wut/lib"
$Specs    = "$DevkitPro/wut/share/wut.specs"

Write-Host "[1/4] Compilando source/main.cpp..."
& $Gxx -DESPRESSO -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra -std=gnu++17 `
    -ffunction-sections -fdata-sections `
    "-I$IncProto" "-I$IncWut" -c $Src -o $Obj
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[2/4] Enlazando WiiU_Transfer.elf..."
& $Gxx -mcpu=750 -meabi -mhard-float "-specs=$Specs" $Obj "-L$LibWut" -lwut -lm -o $Elf
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[3/4] Convirtiendo a WiiU_Transfer.rpx..."
& $Elf2Rpl $Elf $Rpx
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[4/4] Empaquetando WiiU_Transfer.wuhb..."
& $WuhbTool $Rpx $Wuhb `
    --name="Wii U Transfer Tools" `
    --short-name="Wii U Transfer Tools" `
    --author="El Diuca" `
    "--icon=$Root/assets/icon.png" `
    "--tv-image=$Root/assets/tv.png" `
    "--drc-image=$Root/assets/drc.png"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-Item -Force $Wuhb $OutCopy
Write-Host "Listo: $OutCopy"
