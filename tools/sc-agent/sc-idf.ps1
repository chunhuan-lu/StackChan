<#
.SYNOPSIS
    Run idf.py on the StackChan robot while sc-agent has released the robot's COM port.

.DESCRIPTION
    For targets that use the serial port (flash, monitor, erase-flash, efuse-*, ...) it asks sc-agent to release
    the COM port (POST /release), runs "idf.py -p <port> <arguments>" and hands the port back (POST /acquire)
    even when idf.py fails or is interrupted with Ctrl+C. Other targets (build, menuconfig, ...) and runs
    without a reachable sc-agent go straight to idf.py. Run it from an ESP-IDF PowerShell (idf.py available) in
    the firmware directory.

    -p COMx (or -Port / --port) picks the port; otherwise the port reported by sc-agent is used, and without one
    idf.py runs without -p (it then searches the port itself).
    Environment: SC_PORT (agent HTTP port, default 47123), SC_TOKEN_FILE (default %USERPROFILE%\.sc-agent\token).

.EXAMPLE
    ..\tools\sc-agent\sc-idf.ps1 flash

.EXAMPLE
    ..\tools\sc-agent\sc-idf.ps1 -p COM12 flash monitor
#>
# No param() block: a positional parameter would swallow the first idf.py argument ("flash").
$Port = ""
$idfArgs = @()
for ($i = 0; $i -lt $args.Count; $i++) {
    $arg = [string]$args[$i]
    if (($arg -eq "-p" -or $arg -eq "-Port" -or $arg -eq "--port") -and $i + 1 -lt $args.Count) {
        $Port = [string]$args[$i + 1]
        $i++
    } else {
        $idfArgs += $args[$i]
    }
}

# idf.py targets that open the serial port.
$portTargets = @("flash", "app-flash", "bootloader-flash", "partition-table-flash", "encrypted-flash",
    "encrypted-app-flash", "monitor", "erase-flash", "erase-otadata", "read-otadata")
$needsPort = $false
foreach ($arg in $idfArgs) {
    if ($portTargets -contains [string]$arg -or ([string]$arg).StartsWith("efuse-")) {
        $needsPort = $true
    }
}

$ErrorActionPreference = "Stop"
$agentPort = if ($env:SC_PORT) { $env:SC_PORT } else { "47123" }
$tokenFile = if ($env:SC_TOKEN_FILE) { $env:SC_TOKEN_FILE } else { Join-Path $env:USERPROFILE ".sc-agent\token" }
$base = "http://127.0.0.1:$agentPort"
$token = $null
if (Test-Path $tokenFile) {
    $token = (Get-Content -Raw $tokenFile).Trim()
}

function Invoke-Agent([string]$Path) {
    # curl.exe ships with Windows; --noproxy keeps a configured system proxy away from 127.0.0.1.
    $out = & curl.exe -s -f -m 10 --noproxy "*" -X POST -H "X-SC-Token: $token" "$base$Path"
    if ($LASTEXITCODE -ne 0) {
        return @{ curl = $LASTEXITCODE }  # 7: agent not running, 22: HTTP error (token, busy)
    }
    return ($out | ConvertFrom-Json)
}

$released = $false
if (-not $needsPort) {
    # build, menuconfig, size, ...: the COM port is not involved
} elseif (-not $token) {
    Write-Host "sc-agent: no token at $tokenFile, running idf.py directly"
} else {
    $resp = Invoke-Agent "/release"
    if ($resp.curl -eq 7 -or $resp.curl -eq 28) {
        Write-Host "sc-agent: not running, running idf.py directly"
    } else {
        $released = $true  # the agent was reached: always hand the port back afterwards
        if ($resp.curl) {
            Write-Warning "sc-agent: release failed (curl exit $($resp.curl)); wrong token or port still busy"
        } else {
            Write-Host "sc-agent: released $($resp.port)"
            if (-not $Port -and $resp.port) {
                $Port = $resp.port
            }
        }
    }
}

$fullArgs = @(if ($Port) { @("-p", $Port) + $idfArgs } else { $idfArgs })
$exitCode = 1
try {
    Write-Host "idf.py $fullArgs"
    & idf.py @fullArgs
    $exitCode = $LASTEXITCODE
} finally {
    if ($released) {
        $resp = Invoke-Agent "/acquire"
        if ($resp.curl) {
            Write-Warning "sc-agent: acquire failed (curl exit $($resp.curl)); it retakes the port by itself later"
        } else {
            Write-Host "sc-agent: port handed back"
        }
    }
}
exit $exitCode
