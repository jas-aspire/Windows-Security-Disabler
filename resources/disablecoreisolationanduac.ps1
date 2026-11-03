#Requires -RunAsAdministrator

$ErrorActionPreference = "Stop"

Write-Host "Disabling Windows security protections..." -ForegroundColor Yellow

# ------------------------------------------------------------
# Registry paths
# ------------------------------------------------------------

$DeviceGuard = "HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard"
$HVCI        = "$DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity"
$System      = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System"

# ------------------------------------------------------------
# Create backup
# ------------------------------------------------------------

$BackupDir = "$env:SystemDrive\WindowsSecurityBackup"
New-Item -ItemType Directory -Path $BackupDir -Force | Out-Null

Write-Host "Backing up current registry configuration..."

reg.exe export `
    "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard" `
    "$BackupDir\DeviceGuard.reg" /y | Out-Null

reg.exe export `
    "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" `
    "$BackupDir\UAC.reg" /y | Out-Null

# ------------------------------------------------------------
# Disable VBS
# ------------------------------------------------------------

New-Item -Path $DeviceGuard -Force | Out-Null

Set-ItemProperty `
    -Path $DeviceGuard `
    -Name "EnableVirtualizationBasedSecurity" `
    -Type DWord `
    -Value 0

# Prevent VBS from being locked through UEFI
Set-ItemProperty `
    -Path $DeviceGuard `
    -Name "Locked" `
    -Type DWord `
    -Value 0

# ------------------------------------------------------------
# Disable Memory Integrity / HVCI
# ------------------------------------------------------------

New-Item -Path $HVCI -Force | Out-Null

Set-ItemProperty `
    -Path $HVCI `
    -Name "Enabled" `
    -Type DWord `
    -Value 0

Set-ItemProperty `
    -Path $HVCI `
    -Name "Locked" `
    -Type DWord `
    -Value 0

# ------------------------------------------------------------
# Disable Kernel-mode Hardware-enforced Stack Protection
# ------------------------------------------------------------

$KMSP = "$DeviceGuard\Scenarios\KernelModeHardwareEnforcedStackProtection"

if (Test-Path $KMSP) {
    Set-ItemProperty `
        -Path $KMSP `
        -Name "Enabled" `
        -Type DWord `
        -Value 0 `
        -ErrorAction SilentlyContinue

    Set-ItemProperty `
        -Path $KMSP `
        -Name "Locked" `
        -Type DWord `
        -Value 0 `
        -ErrorAction SilentlyContinue
}

# Also remove the policy that can force the feature on.
$VBSKey = "$DeviceGuard"

if (Test-Path $VBSKey) {
    Set-ItemProperty `
        -Path $VBSKey `
        -Name "EnableVirtualizationBasedSecurity" `
        -Type DWord `
        -Value 0
}

# ------------------------------------------------------------
# Disable Microsoft Vulnerable Driver Blocklist
# ------------------------------------------------------------

$CI = "HKLM:\SYSTEM\CurrentControlSet\Control\CI\Config"

New-Item -Path $CI -Force | Out-Null

Set-ItemProperty `
    -Path $CI `
    -Name "VulnerableDriverBlocklistEnable" `
    -Type DWord `
    -Value 0

# ------------------------------------------------------------
# Disable Local Security Authority protection
# ------------------------------------------------------------

$LSA = "HKLM:\SYSTEM\CurrentControlSet\Control\Lsa"

Set-ItemProperty `
    -Path $LSA `
    -Name "RunAsPPL" `
    -Type DWord `
    -Value 0 `
    -ErrorAction SilentlyContinue

Set-ItemProperty `
    -Path $LSA `
    -Name "RunAsPPLBoot" `
    -Type DWord `
    -Value 0 `
    -ErrorAction SilentlyContinue

# ------------------------------------------------------------
# Disable UAC
# ------------------------------------------------------------

Set-ItemProperty `
    -Path $System `
    -Name "EnableLUA" `
    -Type DWord `
    -Value 0

Set-ItemProperty `
    -Path $System `
    -Name "ConsentPromptBehaviorAdmin" `
    -Type DWord `
    -Value 0

# ------------------------------------------------------------
# Disable Windows Defender Exploit Guard controlled-folder/etc.
# NOTE: intentionally not modifying Defender itself.
# ------------------------------------------------------------

Write-Host ""
Write-Host "Security settings have been configured for OFF." -ForegroundColor Green
Write-Host ""
Write-Host "Backup created at:" -ForegroundColor Cyan
Write-Host $BackupDir -ForegroundColor White
Write-Host ""
Write-Host "A RESTART IS REQUIRED." -ForegroundColor Yellow
Write-Host ""

$answer = Read-Host "Restart now? (Y/N)"

if ($answer -match "^[Yy]$") {
    Restart-Computer
}