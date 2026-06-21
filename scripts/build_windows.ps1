# ButterflyBlades Windows 快速构建脚本
# 需要: Visual Studio 2022 + CMake 3.20+

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host " ButterflyBlades Build Script (Windows)" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# 检查 CMake
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Host "[ERROR] CMake not found. Please install CMake 3.20+." -ForegroundColor Red
    exit 1
}
Write-Host "[OK] CMake found: $(cmake --version | Select-Object -First 1)" -ForegroundColor Green

# 创建构建目录
$buildDir = "build_windows"
if (Test-Path $buildDir) {
    Write-Host "[CLEAN] Removing existing build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}
New-Item -ItemType Directory -Path $buildDir | Out-Null

# CMake 配置
Write-Host "[CMAKE] Configuring..." -ForegroundColor Cyan
Push-Location $buildDir
cmake .. -G "Visual Studio 17 2022" -A x64 `
    -DBB_STATIC_MODULES=ON `
    -DBB_BUILD_CONSOLE=ON

if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] CMake configuration failed." -ForegroundColor Red
    Pop-Location
    exit 1
}

# 编译
Write-Host "[BUILD] Compiling..." -ForegroundColor Cyan
cmake --build . --config Release --parallel

if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Build failed." -ForegroundColor Red
    Pop-Location
    exit 1
}

Pop-Location
Write-Host "========================================" -ForegroundColor Green
Write-Host " Build completed successfully!" -ForegroundColor Green
Write-Host " Output: $buildDir\Release\ButterflyBlades.exe" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
