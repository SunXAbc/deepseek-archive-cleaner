# 执行严格模式，任何夹具创建或构建错误都应阻止发布。
$ErrorActionPreference = 'Stop'
# 运行器所属目录，构建与测试路径均由此定位。
$testDirectory = $PSScriptRoot
# vswhere 由 Visual Studio Installer 提供，不依赖个人安装路径。
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
# MSBuild 路径来自已安装的 VS 2022 工具链。
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
if (-not $msbuild) { throw '未找到 Visual Studio MSBuild。' }
& $msbuild (Join-Path $testDirectory 'CoreTests.vcxproj') /p:Configuration=Release /p:Platform=x64 /nologo /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw '核心测试编译失败。' }
# 合成数据位于源码测试目录内的独立临时子目录，便于受限构建环境授予同范围写入权限。
$workDirectory = Join-Path $testDirectory '.work'
# 本轮随机夹具根目录，绝不指向真实 DSH 数据。
$fixtureRoot = Join-Path $workDirectory ('DeepSeekCleaner-junction-tests-' + [guid]::NewGuid().ToString('N'))
# 夹具联接列表用于先移除联接本身，再删除普通测试目录。
$junctionPaths = @()
# 模拟 DSH 的子进程由本运行器拥有，只终止该子进程。
$guardProcess = $null
try {
    New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'outside') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $fixtureRoot 'outside\keep.txt') -Value 'synthetic external marker' -Encoding utf8
    # dataName 分别构造工作区联接和会话内部联接，均指向虚构外部标记。
    foreach ($dataName in @('junction', 'inner')) {
        # 当前联接样本的数据根目录。
        $dataRoot = Join-Path $fixtureRoot $dataName
        New-Item -ItemType Directory -Path (Join-Path $dataRoot 'storages') -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $dataRoot 'storages\workspace.json') -Value '{"global":{"archivedSessionIds":["session-a"]}}' -Encoding utf8
        if ($dataName -eq 'junction') {
            New-Item -ItemType Directory -Path (Join-Path $dataRoot 'sessions') -Force | Out-Null
            # 工作区目录本身是联接，生产扫描应立即拒绝。
            $junction = Join-Path $dataRoot 'sessions\linked-workspace'
        } else {
            New-Item -ItemType Directory -Path (Join-Path $dataRoot 'sessions\test-workspace\session-a') -Force | Out-Null
            # 会话内部目录是联接，递归检查应立即拒绝。
            $junction = Join-Path $dataRoot 'sessions\test-workspace\session-a\linked-content'
        }
        New-Item -ItemType Junction -Path $junction -Target (Join-Path $fixtureRoot 'outside') | Out-Null
        $junctionPaths += $junction
    }
    # 核心测试直接调用与成品相同的删除函数，不需要管理员权限或真实 DSH 数据。
    $testExecutable = Join-Path $testDirectory 'bin\CoreTests.exe'
    & $testExecutable $fixtureRoot
    if ($LASTEXITCODE -ne 0) { throw '核心回归失败。' }
    # 已有 DSH 运行时直接验证保护，不关闭或修改用户的进程。
    if (-not (Get-Process -Name 'DeepSeek Harness' -ErrorAction SilentlyContinue)) {
        # 没有真实 DSH 时，只启动本测试程序的短暂等待模式。
        $guardExecutable = Join-Path $fixtureRoot 'DeepSeek Harness.exe'
        Copy-Item -LiteralPath $testExecutable -Destination $guardExecutable
        # 使用直接进程启动，避免系统 Shell 为测试辅助程序弹出窗口。
        $guardStart = [Diagnostics.ProcessStartInfo]::new($guardExecutable, '--hold')
        $guardStart.UseShellExecute = $false
        $guardStart.CreateNoWindow = $true
        $guardStart.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
        $guardProcess = [Diagnostics.Process]::Start($guardStart)
    }
    & $testExecutable --guard $fixtureRoot
    if ($LASTEXITCODE -ne 0) { throw '进程保护回归失败。' }
} finally {
    if ($guardProcess -and -not $guardProcess.HasExited) { Stop-Process -Id $guardProcess.Id -ErrorAction SilentlyContinue }
    # 清理前验证绝对路径仍在测试工作目录内，只删除本次随机前缀目录。
    $resolvedRoot = [IO.Path]::GetFullPath($fixtureRoot)
    $temporaryRoot = [IO.Path]::GetFullPath($workDirectory) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
        -not ([IO.Path]::GetFileName($resolvedRoot)).StartsWith('DeepSeekCleaner-junction-tests-')) {
        throw '测试清理路径不安全，停止自动清理。'
    }
    # junctionPath 是夹具拥有的联接；先移除链接本身，禁止递归进入目标。
    foreach ($junctionPath in $junctionPaths) {
        if (Test-Path -LiteralPath $junctionPath) { [IO.Directory]::Delete($junctionPath) }
    }
    if (Test-Path -LiteralPath $resolvedRoot) { Remove-Item -LiteralPath $resolvedRoot -Recurse -Force }
}
