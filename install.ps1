param(
    [switch]$AllUsers
)

$ErrorActionPreference = "Stop"

$repository = "sammwyy/clay"
$assetName = "clay-win-x86_64.exe"
$headers = @{ "User-Agent" = "clay-installer" }

if (-not $AllUsers) {
    $choice = Read-Host "Install for the current user or all users? [U/a]"
    $AllUsers = $choice -match "^(a|all)$"
}

$isAdministrator = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)

if ($AllUsers -and -not $isAdministrator) {
    Write-Host "Requesting administrator permission..."
    $scriptPath = $PSCommandPath
    if (-not $scriptPath) {
        $scriptPath = Join-Path ([IO.Path]::GetTempPath()) "clay-install.ps1"
        Invoke-WebRequest `
            -Uri "https://raw.githubusercontent.com/$repository/main/install.ps1" `
            -Headers $headers `
            -OutFile $scriptPath
    }
    $arguments = @(
        "-NoProfile"
        "-ExecutionPolicy"
        "Bypass"
        "-File"
        ('"{0}"' -f $scriptPath)
        "-AllUsers"
    )
    $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}

$release = Invoke-RestMethod `
    -Uri "https://api.github.com/repos/$repository/releases/latest" `
    -Headers $headers
$version = $release.tag_name
$asset = $release.assets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1

if (-not $asset) {
    throw "The latest Clay release does not contain $assetName."
}

if ($AllUsers) {
    $installDirectory = Join-Path $env:ProgramFiles "Clay"
    $pathScope = [EnvironmentVariableTarget]::Machine
} else {
    $installDirectory = Join-Path $HOME ".clay\bin"
    $pathScope = [EnvironmentVariableTarget]::User
}

$installPath = Join-Path $installDirectory "clay.exe"
$temporaryPath = Join-Path ([IO.Path]::GetTempPath()) ("clay-{0}.exe" -f ([guid]::NewGuid()))

try {
    Write-Host "Downloading latest release ($version)..."
    Invoke-WebRequest -Uri $asset.browser_download_url -Headers $headers -OutFile $temporaryPath

    New-Item -ItemType Directory -Force -Path $installDirectory | Out-Null
    Move-Item -Force -Path $temporaryPath -Destination $installPath

    $currentPath = [Environment]::GetEnvironmentVariable("Path", $pathScope)
    $pathEntries = @($currentPath -split ";" | Where-Object { $_ -and $_.Trim() })
    if ($pathEntries -notcontains $installDirectory) {
        $newPath = (($pathEntries + $installDirectory) -join ";")
        [Environment]::SetEnvironmentVariable("Path", $newPath, $pathScope)
    }
    if (($env:Path -split ";") -notcontains $installDirectory) {
        $env:Path = "$installDirectory;$env:Path"
    }

    Write-Host "Installed under $installPath"
    Write-Host "PATH updated for this session."
} finally {
    if (Test-Path $temporaryPath) {
        Remove-Item -Force $temporaryPath
    }
}
