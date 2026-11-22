# Windows PowerShell：同时编译接收器与有界异步日志；不修改系统/防火墙。
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build\windows'),
    [string]$SourceDirectory = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw '未找到 .NET Framework C# 编译器' }
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$receiver = Join-Path $OutputDirectory 'Receiver.exe'
& $compiler /nologo /warn:4 /warnaserror+ /target:exe "/out:$receiver" (Join-Path $SourceDirectory 'Receiver.cs') (Join-Path $SourceDirectory 'AsyncLog.cs')
if ($LASTEXITCODE -ne 0) { throw '接收器编译失败' }
& $receiver --self-test
if ($LASTEXITCODE -ne 0) { throw '协议自测失败' }
$test = Join-Path $OutputDirectory 'TestAsyncLog.exe'
& $compiler /nologo /warn:4 /warnaserror+ /target:exe "/out:$test" (Join-Path $SourceDirectory 'AsyncLog.cs') (Join-Path $SourceDirectory '..\tests\TestAsyncLog.cs')
if ($LASTEXITCODE -ne 0) { throw '日志测试编译失败' }
& $test
if ($LASTEXITCODE -ne 0) { throw '日志测试失败' }
Write-Output "编译和自测通过：$receiver"
