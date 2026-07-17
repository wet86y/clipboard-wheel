$ErrorActionPreference = "Stop"

$LogDirectory = Join-Path $env:LOCALAPPDATA "超级中键\logs"
$CrashDirectory = Join-Path $env:LOCALAPPDATA "超级中键\crash"
if (-not (Test-Path -LiteralPath $LogDirectory -PathType Container)) {
    throw "未找到诊断日志目录：$LogDirectory"
}

$LogFiles = @(Get-ChildItem -LiteralPath $LogDirectory -Filter "native-diagnostic-*.log" -File |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 12)
if ($LogFiles.Count -eq 0) {
    throw "诊断日志目录存在，但里面没有 native-diagnostic-*.log。"
}

$CrashFiles = @()
if (Test-Path -LiteralPath $CrashDirectory -PathType Container) {
    $CrashWindowStart = ($LogFiles | Sort-Object LastWriteTime | Select-Object -First 1).LastWriteTime.AddMinutes(-2)
    $CrashFiles = @(Get-ChildItem -LiteralPath $CrashDirectory -File |
        Where-Object {
            $_.LastWriteTime -ge $CrashWindowStart -and
            ($_.Name -like "native-hang-shell-*" -or $_.Name -like "native-crash-*")
        } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 8)
}

$Timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$Output = Join-Path ([Environment]::GetFolderPath("Desktop")) "超级中键-诊断日志-$Timestamp.zip"
$StagingRoot = Join-Path $env:TEMP ("SuperMiddleKey-Diagnostic-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $StagingRoot | Out-Null

try {
    foreach ($File in @($LogFiles) + @($CrashFiles)) {
        Copy-Item -LiteralPath $File.FullName -Destination (Join-Path $StagingRoot $File.Name)
    }

    $Events = @()
    $ParseErrors = 0
    foreach ($File in ($LogFiles | Sort-Object LastWriteTime)) {
        foreach ($Line in (Get-Content -LiteralPath $File.FullName -Encoding UTF8)) {
            if ([String]::IsNullOrWhiteSpace($Line)) { continue }
            try {
                $Event = $Line | ConvertFrom-Json
                $Event | Add-Member -NotePropertyName source_file -NotePropertyValue $File.Name
                $Events += $Event
            } catch {
                $ParseErrors++
            }
        }
    }

    $Summary = New-Object System.Collections.Generic.List[string]
    $Summary.Add("超级中键诊断摘要 r2")
    $Summary.Add("生成时间：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')")
    $Summary.Add("日志文件：$($LogFiles.Count)，挂起/崩溃文件：$($CrashFiles.Count)，JSON 解析失败行：$ParseErrors")
    $Summary.Add("")
    $Summary.Add("进程关联：")
    foreach ($Group in @($Events | Group-Object pid | Sort-Object Name)) {
        $Ordered = @($Group.Group | Sort-Object time, qpc)
        if ($Ordered.Count -eq 0) { continue }
        $Summary.Add(("pid={0} events={1} first={2} last={3}" -f
            $Group.Name, $Ordered.Count, $Ordered[0].time, $Ordered[-1].time))
    }
    $Summary.Add("")
    $Summary.Add("关键事件计数：")
    $KeyEvents = @($Events | Where-Object {
        $_.event -match '^(session|process|extended|shell|elevation|single_instance|shutdown)\.'
    })
    foreach ($Group in @($KeyEvents | Group-Object event | Sort-Object Name)) {
        $Summary.Add(("{0}={1}" -f $Group.Name, $Group.Count))
    }
    $Summary.Add("")
    $Summary.Add("关键时间线（最多最后 500 条）：")
    foreach ($Event in @($KeyEvents | Sort-Object time, qpc | Select-Object -Last 500)) {
        $Summary.Add(("{0} pid={1} tid={2} event={3} {4}" -f
            $Event.time, $Event.pid, $Event.tid, $Event.event, $Event.details))
    }
    if (@($CrashFiles | Where-Object Extension -eq ".dmp").Count -gt 0) {
        $Summary.Add("")
        $Summary.Add("注意：压缩包包含自动生成的 .dmp 挂起/崩溃转储，用于还原阻塞线程调用栈；其中可能包含模块路径和少量进程内存。")
    }

    $SummaryPath = Join-Path $StagingRoot "诊断摘要.txt"
    [IO.File]::WriteAllLines($SummaryPath, $Summary, (New-Object Text.UTF8Encoding($true)))
    Compress-Archive -Path (Join-Path $StagingRoot "*") -DestinationPath $Output -CompressionLevel Optimal -Force
} finally {
    if ($StagingRoot.StartsWith(([IO.Path]::GetFullPath($env:TEMP) + "\"), [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $StagingRoot)) {
        Remove-Item -LiteralPath $StagingRoot -Recurse -Force
    }
}

Write-Host "日志包已生成：$Output"
if (@($CrashFiles | Where-Object Extension -eq ".dmp").Count -gt 0) {
    Write-Warning "本次日志包包含 .dmp 调用栈转储，请仅在你愿意提供诊断内存信息时回传。"
}
Start-Process explorer.exe -ArgumentList "/select,`"$Output`""
