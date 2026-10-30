<#
.SYNOPSIS
构建 Windows x64 单文件成品，可选运行虚构数据回归。
.PARAMETER Test
构建完成后执行核心回归，不访问默认用户的 DSH 会话。
#>
param([switch]$Test)

# 任何构建或测试错误都应返回失败，禁止继续发布。
$ErrorActionPreference = 'Stop'
# 源码根目录由脚本自身位置确定，不包含个人安装路径。
$projectDirectory = $PSScriptRoot
# Visual Studio Installer 自带的工具定位入口。
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw '请安装 Visual Studio 2022 C++ 桌面开发和 v143 MFC 组件。' }
# MSBuild 来自本机实际安装的 Visual Studio。
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
if (-not $msbuild) { throw '未找到 MSBuild。' }
& $msbuild (Join-Path $projectDirectory 'SessionCleaner.vcxproj') /p:Configuration=Release /p:Platform=x64 /m /nologo /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'Release x64 构建失败。' }
if ($Test) { & (Join-Path $projectDirectory 'tests\run-tests.ps1') }
# 只显示本次成品路径，不生成压缩包或累计版本目录。
Write-Output (Join-Path $projectDirectory 'bin\Release\DeepSeek归档删除工具.exe')
