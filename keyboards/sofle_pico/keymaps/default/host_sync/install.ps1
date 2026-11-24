#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$InstallDir = "",
    [string]$BaseUrl = "",
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

if (-not $InstallDir) {
    if ($env:SOFLE_SYNC_DIR) {
        $InstallDir = $env:SOFLE_SYNC_DIR
    } else {
        $InstallDir = Join-Path $env:LOCALAPPDATA "sofle-pico-sync"
    }
}

if (-not $BaseUrl) {
    if ($env:SOFLE_SYNC_BASE_URL) {
        $BaseUrl = $env:SOFLE_SYNC_BASE_URL
    } else {
        $BaseUrl = "https://raw.githubusercontent.com/IanBurwell/qmk_firmware/ib-sofle-pico/keyboards/sofle_pico/keymaps/default/host_sync"
    }
}
$BaseUrl = $BaseUrl.TrimEnd("/")

function Find-RealPython {
    $candidates = New-Object System.Collections.Generic.List[string]

    $pyLauncher = Get-Command "py" -ErrorAction SilentlyContinue
    if ($pyLauncher) {
        try {
            $pyPath = & $pyLauncher.Source -3 -c "import sys; print(sys.executable)" 2>$null
            if ($LASTEXITCODE -eq 0 -and $pyPath) {
                $candidates.Add($pyPath.Trim())
            }
        } catch {
        }
    }

    foreach ($cmdName in @("python", "python3")) {
        $cmds = @(Get-Command $cmdName -All -ErrorAction SilentlyContinue)
        foreach ($cmd in $cmds) {
            if ($cmd.Source -and ($cmd.Source -notlike "*\Microsoft\WindowsApps\*")) {
                $candidates.Add($cmd.Source)
            }
        }
    }

    $searchRoots = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Python"),
        "C:\Program Files"
    )
    foreach ($root in $searchRoots) {
        if (Test-Path $root) {
            $dirs = @(Get-ChildItem -Path $root -Directory -Filter "Python3*" -ErrorAction SilentlyContinue | Sort-Object Name -Descending)
            foreach ($dir in $dirs) {
                $exe = Join-Path $dir.FullName "python.exe"
                if (Test-Path $exe) {
                    $candidates.Add($exe)
                }
            }
        }
    }

    foreach ($exe in $candidates) {
        if (-not (Test-Path $exe)) {
            continue
        }
        try {
            $verified = & $exe -c "import sys; assert sys.version_info >= (3, 9); print(sys.executable)" 2>$null
            if ($LASTEXITCODE -eq 0 -and $verified) {
                $realExe = $verified.Trim()
                $pythonw = Join-Path (Split-Path $realExe -Parent) "pythonw.exe"
                if (-not (Test-Path $pythonw)) {
                    $pythonw = $realExe
                }
                return @{
                    Python  = $realExe
                    PythonW = $pythonw
                }
            }
        } catch {
        }
    }
    return $null
}

$tmpDir = Join-Path ([IO.Path]::GetTempPath()) ("sofle_sync_" + [Guid]::NewGuid().ToString("N"))

try {
    Write-Host "[sofle-sync] Installing/updating Sofle Pico sync service in $InstallDir" -ForegroundColor Cyan

    $pyInfo = Find-RealPython
    if (-not $pyInfo) {
        $winget = Get-Command "winget" -ErrorAction SilentlyContinue
        if ($winget) {
            Write-Host "[sofle-sync] Python 3.9+ not found; attempting install via winget..." -ForegroundColor Yellow
            & $winget.Source install --id Python.Python.3.12 -e --source winget --accept-package-agreements --accept-source-agreements
            $pyInfo = Find-RealPython
        }
    }
    if (-not $pyInfo) {
        throw "Python 3.9+ was not found on this PC. Install Python 3 from https://www.python.org/downloads/windows/ and re-run this key."
    }
    Write-Host "[sofle-sync] Using Python: $($pyInfo.Python)"

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

    $files = @(
        "sofle_sync.py",
        "sofle_flash.py",
        "sofle_sync_windows.vbs",
        "README.md",
        "ha_config.example.json",
        "audio_config.example.json"
    )

    $anyChanged = $false
    foreach ($file in $files) {
        $url = "{0}/{1}?cb={2}" -f $BaseUrl, $file, [DateTime]::UtcNow.Ticks
        $tmpFile = Join-Path $tmpDir $file
        $destFile = Join-Path $InstallDir $file

        try {
            Invoke-WebRequest -Uri $url -OutFile $tmpFile -UseBasicParsing -Headers @{ "Cache-Control" = "no-cache" }
        } catch {
            throw "Failed downloading '$file' from '$url': $($_.Exception.Message)"
        }
        $newHash = (Get-FileHash -Algorithm SHA256 -Path $tmpFile).Hash

        if (-not (Test-Path $destFile)) {
            Move-Item -Force -Path $tmpFile -Destination $destFile
            Write-Host "  [new]     $file" -ForegroundColor Green
            $anyChanged = $true
        } else {
            $oldHash = (Get-FileHash -Algorithm SHA256 -Path $destFile).Hash
            if ($oldHash -ne $newHash) {
                Copy-Item -Force -Path $tmpFile -Destination $destFile
                Remove-Item -Force -Path $tmpFile
                Write-Host "  [updated] $file" -ForegroundColor Green
                $anyChanged = $true
            } else {
                Remove-Item -Force -Path $tmpFile
                Write-Host "  [current] $file"
            }
        }
    }

    $scriptPath = Join-Path $InstallDir "sofle_sync.py"
    $startupDir = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\Startup"
    New-Item -ItemType Directory -Force -Path $startupDir | Out-Null
    $shortcutPath = Join-Path $startupDir "Sofle Pico Sync.lnk"
    $desiredArgs = "`"$scriptPath`""

    $wshShell = New-Object -ComObject WScript.Shell
    $shortcutExisted = Test-Path $shortcutPath
    $shortcut = $wshShell.CreateShortcut($shortcutPath)
    $shortcutChanged = $false
    if (-not $shortcutExisted -or $shortcut.TargetPath -ne $pyInfo.PythonW -or $shortcut.Arguments -ne $desiredArgs -or $shortcut.WorkingDirectory -ne $InstallDir) {
        $shortcut.TargetPath = $pyInfo.PythonW
        $shortcut.Arguments = $desiredArgs
        $shortcut.WorkingDirectory = $InstallDir
        $shortcut.Description = "Sofle Pico Dual-USB Split Sync Service"
        $shortcut.WindowStyle = 7
        $shortcut.Save()
        $shortcutChanged = $true
        Write-Host "  [startup] Configured $shortcutPath" -ForegroundColor Green
    } else {
        Write-Host "  [startup] Shortcut up to date"
    }

    $running = @(Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='pythonw.exe'" -ErrorAction SilentlyContinue | Where-Object {
        $_.CommandLine -and ($_.CommandLine -like "*sofle_sync.py*")
    })

    $escapedScript = [Regex]::Escape($scriptPath)
    $alreadyCurrentProc = ($running.Count -eq 1) -and ($running[0].CommandLine -match $escapedScript)

    if (-not $anyChanged -and -not $shortcutChanged -and $alreadyCurrentProc) {
        Write-Host "[sofle-sync] Already up to date and running (PID $($running[0].ProcessId))." -ForegroundColor Cyan
    } else {
        foreach ($proc in $running) {
            Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue
        }
        if ($running.Count -gt 0) {
            Start-Sleep -Milliseconds 300
        }

        $launched = Start-Process -FilePath $pyInfo.PythonW -ArgumentList $desiredArgs -WorkingDirectory $InstallDir -PassThru
        Start-Sleep -Seconds 1
        $alive = Get-Process -Id $launched.Id -ErrorAction SilentlyContinue
        if ($alive) {
            Write-Host "[sofle-sync] Running in background (PID $($launched.Id))." -ForegroundColor Green
        } else {
            Write-Host "[sofle-sync] Launched service (PID $($launched.Id)); verify with: & `"$($pyInfo.Python)`" `"$scriptPath`" --verbose" -ForegroundColor Yellow
        }
    }

    if (-not $NoPause) {
        Start-Sleep -Seconds 4
    }
} catch {
    Write-Host ""
    Write-Host "[sofle-sync] ERROR: $($_.Exception.Message)" -ForegroundColor Red
    if (-not $NoPause) {
        Read-Host "Press Enter to close" | Out-Null
    }
    exit 1
} finally {
    if (Test-Path $tmpDir) {
        Remove-Item -Recurse -Force -Path $tmpDir -ErrorAction SilentlyContinue
    }
}
