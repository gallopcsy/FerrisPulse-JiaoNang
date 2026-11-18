# build.ps1 — 构建测试 + 胶囊可执行文件
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# 定位 g++（winget 用户级安装不会刷新已开 shell 的 PATH）
$gxx = (Get-Command g++ -ErrorAction SilentlyContinue)
if (-not $gxx) {
    $pkg = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages') -Directory -Filter 'BrechtSanders.WinLibs*' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pkg) {
        $hit = Get-ChildItem $pkg.FullName -Recurse -Filter 'g++.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { $gxx = $hit.FullName }
    }
}
if (-not $gxx) { Write-Error 'g++ not found. Install: winget install -e --id BrechtSanders.WinLibs.MCF.UCRT'; exit 1 }
$gxx = "$gxx"

# -ffunction-sections/-fdata-sections + --gc-sections：丢弃未引用的函数/数据（含 libstdc++ 模板实例）
# -fno-exceptions/-fno-rtti：代码零异常、零 RTTI，去掉展开表与 typeinfo（进一步瘦身）
$cxx = @('-std=c++17', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
         '-fno-exceptions', '-fno-rtti',
         '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00')

New-Item -ItemType Directory -Force -Path bin | Out-Null

# 1) 单元测试（接缝 CapsuleCore）
& $gxx @cxx -O2 -static -Isrc -o 'bin\capsule-tests.exe' 'tests\test_core.cpp' 'src\core\CapsuleCore.cpp'
if ($LASTEXITCODE -ne 0) { Write-Error 'test build failed'; exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'bin\capsule-tests.exe')
if ($LASTEXITCODE -ne 0) { Write-Error "tests FAILED ($LASTEXITCODE)"; exit $LASTEXITCODE }

# 2) 主程序（绿色单文件：静态链接、无控制台、裁剪符号）
$srcs = @('src\ui\main.cpp', 'src\adapters\RegistryReader.cpp', 'src\adapters\Environment.cpp', 'src\core\CapsuleCore.cpp')
$libs = @('-lgdiplus', '-lgdi32', '-lmsimg32', '-lshell32', '-ladvapi32', '-luser32', '-lole32')
& $gxx @cxx -Os -municode -mwindows -static -s -flto '-Wl,--gc-sections' -Isrc -o 'bin\capsule.exe' @srcs @libs
if ($LASTEXITCODE -ne 0) { Write-Error 'exe build failed'; exit $LASTEXITCODE }

$out = Join-Path $PSScriptRoot 'bin\设备任务栏胶囊.exe'
if (Test-Path $out) { Remove-Item $out -Force }
Rename-Item 'bin\capsule.exe' '设备任务栏胶囊.exe'
Write-Host ("BUILD OK  ->  {0}  ({1:N0} bytes)" -f $out, (Get-Item $out).Length)
