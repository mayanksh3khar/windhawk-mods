# windhawk-mods
My windhawk mods


## Start Menu Native Widgets setup

The Native Windows Widgets Link tile uses two Microsoft Adaptive Cards packages. For a clean reinstall, disable the mod and delete `C:\ProgramData\AdaptiveCards` if it already exists. You will have to manually delete this folder if you decide to uninstall this mod.

Paste this into Admin PowerShell. If the session isn’t elevated, it opens an elevated PowerShell window and prompts for UAC approval. It creates `%ProgramData%\AdaptiveCards` if needed, switches to that directory, and deletes the two ZIPs after both packages extract successfully.

```powershell
$installAdaptiveCards = {
    $ErrorActionPreference = 'Stop'

    $folder = Join-Path $env:ProgramData 'AdaptiveCards'
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    Set-Location -LiteralPath $folder

    $packages = [ordered]@{}

    foreach ($name in @('AdaptiveCards.ObjectModel.Uwp', 'AdaptiveCards.Rendering.Uwp')) {
        $id = $name.ToLowerInvariant()
        $index = Invoke-RestMethod -Uri "https://api.nuget.org/v3-flatcontainer/$id/index.json"

        $version = $index.versions |
            Where-Object { $_ -notmatch '-' } |
            Sort-Object { [version]$_ } -Descending |
            Select-Object -First 1

        if (-not $version) {
            throw "No stable NuGet version found for $name."
        }

        $packages[$name] = $version
    }

    $archives = foreach ($package in $packages.GetEnumerator()) {
        $name = $package.Key
        $id = $name.ToLowerInvariant()
        $version = $package.Value
        $url = "https://api.nuget.org/v3-flatcontainer/$id/$version/$id.$version.nupkg"
        $zip = Join-Path $folder "$name.zip"

        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip
        Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $folder $name) -Force
        $zip
    }

    Remove-Item -LiteralPath $archives
    Write-Host "Adaptive Cards installed in $folder"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)

if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    & $installAdaptiveCards
} else {
    $encoded = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($installAdaptiveCards.ToString())
    )
    Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
        -Verb RunAs -ArgumentList '-NoProfile', '-NoExit', '-EncodedCommand', $encoded
}
```

The command overwrites matching package files if you rerun it. Once it prints `Adaptive Cards installed`, re-enable the mod and check Native Widgets in Start.
