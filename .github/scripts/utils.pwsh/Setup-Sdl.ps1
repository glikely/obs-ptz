function Setup-Sdl {
    if ( ! ( Test-Path function:Log-Output ) ) {
        . $PSScriptRoot/Logger.ps1
    }

    if ( ! ( Test-Path function:Check-Git ) ) {
        . $PSScriptRoot/Check-Git.ps1
    }

    Check-Git

    if ( ! ( Test-Path function:Ensure-Location ) ) {
        . $PSScriptRoot/Ensure-Location.ps1
    }

    if ( ! ( Test-Path function:Invoke-GitCheckout ) ) {
        . $PSScriptRoot/Invoke-GitCheckout.ps1
    }

    if ( ! ( Test-Path function:Invoke-External ) ) {
        . $PSScriptRoot/Invoke-External.ps1
    }

    Log-Information 'Fetching SDL...'

    $SdlVersion = $BuildSpec.dependencies.'sdl'.version
    $SdlRepository = $BuildSpec.dependencies.'sdl'.repository
    $SdlBranch = $BuildSpec.dependencies.'sdl'.branch
    $SdlHash = $BuildSpec.dependencies.'sdl'.hash

    if ( $SdlVersion -eq '' ) {
        throw 'No SDL version found in buildspec.json.'
    }

    Push-Location -Stack BuildTemp
    Ensure-Location -Path "$(Resolve-Path -Path "${ProjectRoot}/../")/sdl"

    if ( ! ( ( $script:SkipAll ) -or ( $script:SkipUnpack ) ) ) {
        Invoke-GitCheckout -Uri $SdlRepository -Commit $SdlHash -Path . -Branch $SdlBranch
    }

    if ( ! ( ( $script:SkipAll ) -or ( $script:SkipBuild ) ) ) {
        Log-Information 'Configuring SDL...'

        $NumProcessors = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors

        if ( $NumProcessors -gt 1 ) {
            $env:UseMultiToolTask = $true
            $env:EnforceProcessCountAcrossBuilds = $true
        }

        $DepsPath = "plugin-deps-${script:DepsVersion}-qt${script:QtVersion}-${script:Target}"

        $CmakeArgs = @(
            '-G', $CmakeGenerator
            "-DCMAKE_SYSTEM_VERSION=${script:PlatformSDK}"
            "-DCMAKE_GENERATOR_PLATFORM=$(if (${script:Target} -eq "x86") { "Win32" } else { "x64" })"
            "-DCMAKE_BUILD_TYPE=${script:Configuration}"
            "-DCMAKE_INSTALL_PREFIX:PATH=$(Resolve-Path -Path "${ProjectRoot}/../obs-build-dependencies/${DepsPath}")"
            "-DCMAKE_PREFIX_PATH:PATH=$(Resolve-Path -Path "${ProjectRoot}/../obs-build-dependencies/${DepsPath}")"
        )

        Log-Debug "Attempting to configure SDL with CMake arguments: $($CmakeArgs | Out-String)"
        Log-Information "Configuring SDL..."
        Invoke-External cmake -S . -B plugin_build_${script:Target} @CmakeArgs

        Log-Information 'Building libsdl...'
        $CmakeArgs = @(
            '--config', "$( if ( $script:Configuration -eq '' ) { 'RelWithDebInfo' } else { $script:Configuration })"
        )

        if ( $VerbosePreference -eq 'Continue' ) {
            $CmakeArgs+=('--verbose')
        }

        Invoke-External cmake --build plugin_build_${script:Target} @CmakeArgs
        Invoke-External cmake --install plugin_build_${script:Target} @CmakeArgs
    }
    Pop-Location -Stack BuildTemp
}
