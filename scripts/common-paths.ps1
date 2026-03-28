function Get-GaYmArtifactLayout {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration = 'Debug'
    )

    $stageRoot = if ($Configuration -eq 'Debug') {
        Join-Path $Root 'out\dev'
    } else {
        Join-Path $Root 'out\release'
    }

    $repoCandidate = [pscustomobject]@{
        Mode              = 'Repo'
        Root              = $Root
        ArtifactRoot      = $stageRoot
        DriverUpperRoot   = Join-Path $stageRoot 'driver-upper'
        DriverLowerRoot   = Join-Path $stageRoot 'driver-lower'
        ToolsRoot         = Join-Path $stageRoot 'tools'
        StateRoot         = Join-Path $Root 'out'
        HasDriverPackages = (Test-Path (Join-Path $stageRoot 'driver-upper\package\GaYmXboxFilter.inf')) -and
                            (Test-Path (Join-Path $stageRoot 'driver-lower\package\GaYmFilter.inf'))
        HasTools          = Test-Path (Join-Path $stageRoot 'tools\GaYmCLI.exe')
    }

    $bundleCandidate = [pscustomobject]@{
        Mode              = 'Bundle'
        Root              = $Root
        ArtifactRoot      = $Root
        DriverUpperRoot   = Join-Path $Root 'driver-upper'
        DriverLowerRoot   = Join-Path $Root 'driver-lower'
        ToolsRoot         = Join-Path $Root 'tools'
        StateRoot         = Join-Path $Root 'out'
        HasDriverPackages = (Test-Path (Join-Path $Root 'driver-upper\package\GaYmXboxFilter.inf')) -and
                            (Test-Path (Join-Path $Root 'driver-lower\package\GaYmFilter.inf'))
        HasTools          = Test-Path (Join-Path $Root 'tools\GaYmCLI.exe')
    }

    if ($repoCandidate.HasDriverPackages -or $repoCandidate.HasTools) {
        return $repoCandidate
    }

    if ($bundleCandidate.HasDriverPackages -or $bundleCandidate.HasTools) {
        return $bundleCandidate
    }

    return $repoCandidate
}

function Get-GaYmDriverPackagePaths {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Layout
    )

    return [pscustomobject]@{
        LowerInf = Join-Path $Layout.DriverLowerRoot 'package\GaYmFilter.inf'
        UpperInf = Join-Path $Layout.DriverUpperRoot 'package\GaYmXboxFilter.inf'
        LowerCat = Join-Path $Layout.DriverLowerRoot 'package\gaymfilter.cat'
        UpperCat = Join-Path $Layout.DriverUpperRoot 'package\gaymxboxfilter.cat'
    }
}

function Get-GaYmCertificatePaths {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Layout
    )

    $candidates = @(
        (Join-Path $Layout.DriverUpperRoot 'GaYmXboxFilter.cer'),
        (Join-Path $Layout.DriverLowerRoot 'GaYmFilter.cer'),
        (Join-Path $Layout.DriverUpperRoot 'package\GaYmXboxFilter.cer'),
        (Join-Path $Layout.DriverLowerRoot 'package\GaYmFilter.cer')
    )

    return $candidates | Where-Object { Test-Path $_ } | Select-Object -Unique
}

function Get-GaYmToolPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Layout,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    return Join-Path $Layout.ToolsRoot $Name
}

function New-GaYmStatePath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Layout,
        [Parameter(Mandatory = $true)]
        [string]$LeafName
    )

    return Join-Path $Layout.StateRoot $LeafName
}
