<#
.SYNOPSIS
    Flash the StackChan robot over USB, make sure it boots the new firmware, then start the AI Agent.

.DESCRIPTION
    1. idf.py flash through sc-idf.ps1: sc-agent releases the COM port, esptool resets the ESP32-S3 into the ROM
       download mode over USB-Serial/JTAG, flashes and resets it, and sc-agent takes the port back.
    2. Waits until sc-agent is linked to the restarted robot (new boot id). A robot that stays in the ROM loader
       (e.g. download mode entered by hand) is restarted with esptool --after watchdog_reset.
    3. Asks the robot to start the AI Agent (POST /agent, USB link command "agent") and waits until it is ready.

    Run it from an ESP-IDF PowerShell (idf.py available) in the firmware directory, after a build. Needs sc-agent
    (start-sc-agent.bat) and firmware with the USB link (CONFIG_SC_USB_LINK); the "agent" command needs this
    fork's firmware from REQ-006 on. -NoAgent skips step 3, -p COMx picks the port.
    Environment: SC_PORT (agent HTTP port, default 47123), SC_TOKEN_FILE (default %USERPROFILE%\.sc-agent\token).

.EXAMPLE
    ..\tools\sc-agent\sc-flash.ps1

.EXAMPLE
    ..\tools\sc-agent\sc-flash.ps1 -p COM12 -NoAgent
#>
# No param() block, like sc-idf.ps1: unknown arguments go to idf.py flash.
$Port = ""
$NoAgent = $false
$extra = @()
for ($i = 0; $i -lt $args.Count; $i++) {
    $arg = [string]$args[$i]
    if (($arg -eq "-p" -or $arg -eq "-Port" -or $arg -eq "--port") -and $i + 1 -lt $args.Count) {
        $Port = [string]$args[$i + 1]
        $i++
    } elseif ($arg -eq "-NoAgent") {
        $NoAgent = $true
    } else {
        $extra += $args[$i]
    }
}

$ErrorActionPreference = "Stop"
$agentPort = if ($env:SC_PORT) { $env:SC_PORT } else { "47123" }
$tokenFile = if ($env:SC_TOKEN_FILE) { $env:SC_TOKEN_FILE } else { Join-Path $env:USERPROFILE ".sc-agent\token" }
$base = "http://127.0.0.1:$agentPort"
if (-not (Test-Path $tokenFile)) {
    Write-Error "sc-flash: no sc-agent token at $tokenFile (run setup.bat and start-sc-agent.bat first)"
}
$token = (Get-Content -Raw $tokenFile).Trim()

function Invoke-Agent([string]$Method, [string]$Path) {
    # curl.exe ships with Windows; --noproxy keeps a configured system proxy away from 127.0.0.1.
    $out = & curl.exe -s -f -m 10 --noproxy "*" -X $Method -H "X-SC-Token: $token" "$base$Path"
    if ($LASTEXITCODE -ne 0) {
        return $null
    }
    return ($out | ConvertFrom-Json)
}

# Wait until $Condition (a script block taking the status) holds; returns the last status or $null on timeout.
function Wait-Status([scriptblock]$Condition, [int]$Seconds, [string]$What) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    $shown = ""
    while ((Get-Date) -lt $deadline) {
        $status = Invoke-Agent "GET" "/status"
        if ($status) {
            $now = "link $($status.link), robot $($status.robot_st)"
            if ($now -ne $shown) {
                Write-Host "  $What... $now"
                $shown = $now
            }
            if (& $Condition $status) {
                return $status
            }
        }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

$before = Invoke-Agent "GET" "/status"
if (-not $before) {
    Write-Error "sc-flash: sc-agent does not answer on $base (start-sc-agent.bat)"
}
$oldBoot = $before.boot
$started = Get-Date

# 1. Flash
$idfArgs = @()
if ($Port) {
    $idfArgs += @("-p", $Port)
}
$idfArgs += "flash"
$idfArgs += $extra
& (Join-Path $PSScriptRoot "sc-idf.ps1") @idfArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "sc-flash: flashing failed (exit $LASTEXITCODE)"
}

# 2. The new firmware is up when sc-agent links with a new boot id.
$rebooted = { param($s) $s.link -eq "linked" -and $s.boot -and $s.boot -ne $oldBoot }
$status = Wait-Status $rebooted 30 "waiting for the robot"
if (-not $status) {
    Write-Host "sc-flash: no link after the flash, restarting the robot with a watchdog reset"
    $resp = Invoke-Agent "POST" "/release"
    $resetPort = if ($Port) { $Port } elseif ($resp -and $resp.port) { $resp.port } else { "" }
    try {
        if (-not $resetPort) {
            Write-Error "sc-flash: robot port unknown, pass -p COMx"
        }
        # The robot leaves USB while it resets, so esptool ends with a pySerial error: not a failure.
        $ErrorActionPreference = "Continue"
        & python -m esptool --chip esp32s3 -p $resetPort --after watchdog_reset chip_id
        $ErrorActionPreference = "Stop"
    } finally {
        Invoke-Agent "POST" "/acquire" | Out-Null
    }
    $status = Wait-Status $rebooted 30 "waiting for the robot"
    if (-not $status) {
        Write-Error "sc-flash: the robot did not come back; press RST once and check the serial log"
    }
}
Write-Host "sc-flash: robot runs fw $($status.fw) (boot $($status.boot))"

if ($NoAgent) {
    exit 0
}

# 3. Start the AI Agent and wait until it is ready (Wi-Fi and the server connection take a few seconds).
if (-not (Invoke-Agent "POST" "/agent")) {
    Write-Error "sc-flash: sc-agent did not accept /agent (older sc-agent?)"
}
$newBoot = $status.boot
$readyOrRestarted = { param($s) $s.robot_st -eq "idle" -or $s.robot_st -eq "talk" -or ($s.boot -and $s.boot -ne $newBoot) }
$ready = Wait-Status $readyOrRestarted 90 "starting the AI Agent"
if ($ready -and $ready.boot -ne $newBoot) {
    # Upstream gives Wi-Fi 60 s (IP included), then restarts into the App setup page (a reset gets out of it).
    Write-Error "sc-flash: the robot restarted while the AI Agent started (Wi-Fi timeout? see the serial log in %USERPROFILE%\.sc-agent\logs)"
}
if (-not $ready) {
    $last = Invoke-Agent "GET" "/status"
    if ($last -and $null -eq $last.robot_ai) {
        Write-Error "sc-flash: the firmware does not report the AI Agent state (no 'agent' command)"
    }
    Write-Error "sc-flash: the AI Agent is not ready after 90 s (Wi-Fi? see the serial log in %USERPROFILE%\.sc-agent\logs)"
}
$seconds = [int]((Get-Date) - $started).TotalSeconds
Write-Host "sc-flash: AI Agent ready ($seconds s in total)"
exit 0
