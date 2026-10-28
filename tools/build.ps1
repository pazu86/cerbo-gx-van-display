# Build / flash helper: activates ESP-IDF v5.5.5 and runs idf.py in firmware/.
#
#   .\tools\build.ps1                 # build
#   .\tools\build.ps1 ota             # build + update over WiFi (no USB needed)
#   .\tools\build.ps1 ha              # build + publish to Home Assistant: the display installs it
#                                     # itself (a minute after it joins a network, then every 30 min)
#   .\tools\build.ps1 flash           # build + flash over USB, then boot the app
#   .\tools\build.ps1 flash monitor   # ...and open the serial monitor (Ctrl+] to exit)
#   .\tools\build.ps1 monitor         # monitor without resetting the board
#   $env:DISPLAY_PORT = 'COM11'        # override the default COM10
#   $env:DISPLAY_HOST = '192.168.1.50'    # override van-display.local for ota
#   $env:DISPLAY_HA_SSH = 'root@homeassistant.local'   # ssh target for 'ha' (default: ha)
#   $env:DISPLAY_WEB_PASS = '...'      # override the password from secrets.h (e.g. after changing it)
#
# Why the special USB handling: GPIO0 (boot strap) doubles as an LCD data line,
# and on the S3's native USB an RTS reset leaves the chip in download mode. So
# after flashing we reset via the RTC watchdog instead, and the monitor never
# resets. OTA avoids all of that.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$IdfArgs)

# Not 'Stop': idf.py writes CMake warnings to stderr, which PowerShell 5.1
# would turn into terminating errors. We check $LASTEXITCODE instead.
$ErrorActionPreference = 'Continue'
$firmware = Resolve-Path (Join-Path $PSScriptRoot '..\firmware')
# Build output goes to a ClaudeBuildSpace folder beside this project's folder when one exists
# (keeps it out of the source tree), otherwise the usual firmware\build.
$buildSpace = Join-Path (Split-Path (Split-Path $firmware -Parent) -Parent) 'ClaudeBuildSpace'
$buildDir = if (Test-Path $buildSpace) { Join-Path $buildSpace (Split-Path (Split-Path $firmware -Parent) -Leaf) } else { Join-Path $firmware 'build' }
$port = if ($env:DISPLAY_PORT) { $env:DISPLAY_PORT } else { 'COM10' }
$hostName = if ($env:DISPLAY_HOST) { $env:DISPLAY_HOST } else { 'van-display.local' }

. 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1' | Out-Null

if (-not $IdfArgs) { $IdfArgs = @('build') }
$monitor = $IdfArgs -contains 'monitor'
$ota = $IdfArgs -contains 'ota'
$publish = $IdfArgs -contains 'ha'
$build = @($IdfArgs | Where-Object { $_ -ne 'monitor' -and $_ -ne 'ota' -and $_ -ne 'ha' })
if (($ota -or $publish) -and -not $build) { $build = @('build') }
$haSsh = if ($env:DISPLAY_HA_SSH) { $env:DISPLAY_HA_SSH } else { 'ha' }

Push-Location $firmware
try {
    if ($build) {
        idf.py -p $port -B $buildDir @build
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        if ($build -contains 'flash') {
            python -m esptool --chip esp32s3 -p $port --before no_reset --after watchdog_reset chip_id | Out-Null
            Write-Host "Rebooted into app via watchdog reset"
        }
    }
    if ($publish) {
        # The van_display_update component (homeassistant\) serves /config/van_display/firmware.bin
        # only to requests with the display's key: the image embeds secrets.h, so never /config/www.
        # Needs SSH to HA (the "Advanced SSH & Web Terminal" add-on) as $haSsh.
        $bin = Join-Path $buildDir 'van_display.bin'
        $version = [regex]::Match((Get-Content (Join-Path $firmware 'CMakeLists.txt') -Raw), 'PROJECT_VER "([^"]+)"').Groups[1].Value
        if (-not (Select-String -Quiet -Path (Join-Path $firmware 'main\secrets.h') -Pattern 'DISPLAY_UPDATE_KEY')) {
            Write-Host 'No DISPLAY_UPDATE_KEY in secrets.h: that build could never update itself. Run python tools\setup_update_key.py first.'
            exit 1
        }
        # cmd /c keeps the upload binary-safe (PowerShell 5.1 pipes would corrupt it); written to .new
        # first so the display never sees half a file.
        cmd /c "ssh $haSsh `"mkdir -p /config/van_display && cat > /config/van_display/firmware.bin.new && mv /config/van_display/firmware.bin.new /config/van_display/firmware.bin`" < `"$bin`""
        if ($LASTEXITCODE -ne 0) { Write-Host 'Publishing to HA failed'; exit 1 }
        $remote = (ssh $haSsh 'sha256sum /config/van_display/firmware.bin').Split(' ')[0]
        $local = (Get-FileHash $bin -Algorithm SHA256).Hash.ToLower()
        if ($remote -ne $local) { Write-Host "Published file doesn't match the build ($remote vs $local)"; exit 1 }
        Write-Host "Published $version to Home Assistant ($([math]::Round((Get-Item $bin).Length / 1MB, 2)) MB, sha256 $($local.Substring(0, 12))...)."
        Write-Host 'The display installs it about a minute after it next joins a network (else within 30 min); see update.van_display_firmware.'
    }
    if ($ota) {
        # Web credentials live in the git-ignored secrets.h
        $secrets = Get-Content (Join-Path $firmware 'main\secrets.h') -Raw
        $user = [regex]::Match($secrets, 'DISPLAY_WEB_USER\s+"([^"]*)"').Groups[1].Value
        $pass = [regex]::Match($secrets, 'DISPLAY_WEB_PASS\s+"([^"]*)"').Groups[1].Value
        if ($env:DISPLAY_WEB_PASS) { $pass = $env:DISPLAY_WEB_PASS }
        $bin = Join-Path $buildDir 'van_display.bin'
        $health = { curl.exe -s -m 4 -u "${user}:${pass}" "http://$hostName/api/health" | ConvertFrom-Json }
        $h0 = & $health
        $before = $h0.firmware
        $slot = $h0.partition   # a successful update always boots the other slot
        Write-Host "Uploading $([math]::Round((Get-Item $bin).Length / 1MB, 2)) MB to http://$hostName (running $before) ..."
        curl.exe --fail-with-body -sS -u "${user}:${pass}" --data-binary "@$bin" `
            -H 'Content-Type: application/octet-stream' "http://$hostName/api/ota"
        Write-Host ''
        if ($LASTEXITCODE -ne 0) { Write-Host 'OTA failed'; exit 1 }

        # Follow the restart and the new firmware's self-confirmation; don't
        # touch the board until it's done or the update rolls back.
        $deadline = (Get-Date).AddMinutes(3)
        $rebooted = $false
        $last = ''
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 2
            try { $h = & $health } catch { $h = $null }
            if (-not $h) { $rebooted = $true; $state = 'restarting...' }
            elseif ($h.update.phase -eq 'verifying') { $rebooted = $true; $state = "confirming $($h.firmware) ($($h.update.pct) %)" }
            elseif ($h.update.phase -eq 'restarting') { $state = "restarting in $($h.update.secs)" }
            elseif ($h.uptime_s -lt 120 -and ($rebooted -or $h.update.phase -eq 'idle')) {
                if ($h.partition -ne $slot) { Write-Host "Updated $before -> $($h.firmware), confirmed." }
                else { Write-Host "Rolled back - still running $($h.firmware)."; exit 1 }
                break
            } else { $state = 'waiting...' }
            if ($state -ne $last) { Write-Host "  $state"; $last = $state }
        }
    }
    if ($monitor) {
        idf.py -p $port -B $buildDir monitor --no-reset
    }
} finally {
    Pop-Location
}
