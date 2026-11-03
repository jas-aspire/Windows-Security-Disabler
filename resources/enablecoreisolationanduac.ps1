#Requires -RunAsAdministrator

$ErrorActionPreference = "Stop"

$BackupDir = "$env:SystemDrive\WindowsSecurityBackup"

if (-not (Test-Path $BackupDir)) {
    Write-Host "Backup directory not found:" -ForegroundColor Red
    Write-Host $BackupDir
    Write-Host ""
    Write-Host "The disable script must be run first."
    exit 1
}

Write-Host "Restoring Windows security configuration..." -ForegroundColor Yellow
Write-Host ""

# ------------------------------------------------------------
# Restore Device Guard / VBS / HVCI
# ------------------------------------------------------------

$DeviceGuardBackup = "$BackupDir\DeviceGuard.reg"

if (Test-Path $DeviceGuardBackup) {
    Write-Host "Restoring Device Guard / VBS / Core Isolation..."

    reg.exe import $DeviceGuardBackup | Out-Null
}
else {
    Write-Host "DeviceGuard backup not found." -ForegroundColor Red
}

# ------------------------------------------------------------
# Restore UAC
# ------------------------------------------------------------

$UACBackup = "$BackupDir\UAC.reg"

if (Test-Path $UACBackup) {
    Write-Host "Restoring UAC..."

    reg.exe import $UACBackup | Out-Null
}
else {
    Write-Host "UAC backup not found." -ForegroundColor Red
}

# ------------------------------------------------------------
# Restore vulnerable-driver blocklist / LSA settings
# ------------------------------------------------------------

# These weren't included in the two broad exports above,
# so remove our explicit overrides. Windows will then use
# its normal/default policy where applicable.

$CI = "HKLM:\SYSTEM\CurrentControlSet\Control\CI\Config"

if (Test-Path $CI) {
    Remove-ItemProperty `
        -Path $CI `
        -Name "VulnerableDriverBlocklistEnable" `
        -ErrorAction SilentlyContinue
}

$LSA = "HKLM:\SYSTEM\CurrentControlSet\Control\Lsa"

if (Test-Path $LSA) {
    Remove-ItemProperty `
        -Path $LSA `
        -Name "RunAsPPL" `
        -ErrorAction SilentlyContinue

    Remove-ItemProperty `
        -Path $LSA `
        -Name "RunAsPPLBoot" `
        -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Configuration restored." -ForegroundColor Green
Write-Host ""
Write-Host "A RESTART IS REQUIRED for the changes to fully take effect." -ForegroundColor Yellow
Write-Host ""

$answer = Read-Host "Restart now? (Y/N)"

if ($answer -match "^[Yy]$") {
    Restart-Computer
}