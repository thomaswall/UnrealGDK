@if not defined TEAMCITY_CAPTURE_ENV ( echo off ) else ( echo on )

setlocal

pushd "%~dp0"

call :MarkStartOfBlock "%~0"

call :MarkStartOfBlock "Setup the git hooks"
    if defined TEAMCITY_CAPTURE_ENV goto SkipGitHooks
    if not exist .git\hooks goto SkipGitHooks

    echo #!/bin/sh>.git\hooks\post-checkout
    echo cmd.exe /c Setup.bat>>.git\hooks\post-checkout
    echo #!/bin/sh>.git\hooks\post-merge
    echo cmd.exe /c Setup.bat>>.git\hooks\post-merge

    :SkipGitHooks
call :MarkEndOfBlock "Setup the git hooks"

call :MarkStartOfBlock "Check dependencies"
    set /p UNREAL_VERSION=<./SpatialGDK/Extras/unreal-engine.version
    if defined TEAMCITY_CAPTURE_ENV (
        set UNREAL_HOME=C:\Unreal\UnrealEngine-%UNREAL_VERSION%
    )

    if not defined UNREAL_HOME (
        echo Error: Please set UNREAL_HOME environment variable to point to the Unreal Engine folder.
        if not defined TEAMCITY_CAPTURE_ENV pause
        exit /b 1
    )

    rem Use Unreal Engine's script to get the path to MSBuild. This turns off echo so turn it back on for TeamCity.
    call "%UNREAL_HOME%\Engine\Build\BatchFiles\GetMSBuildPath.bat"
    if defined TEAMCITY_CAPTURE_ENV echo on

    if not defined MSBUILD_EXE (
        echo Error: Could not find the MSBuild executable. Please make sure you have Microsoft Visual Studio or Microsoft Build Tools installed.
        if not defined TEAMCITY_CAPTURE_ENV pause
        exit /b 1
    )

    where spatial >nul
    if ERRORLEVEL 1 (
        echo Error: Could not find spatial. Please make sure you have it installed and the containing directory added to PATH environment variable.
        if not defined TEAMCITY_CAPTURE_ENV pause
        exit /b 1
    )
call :MarkEndOfBlock "Check dependencies"

call :MarkStartOfBlock "Setup variables"
    set /p PINNED_CORE_SDK_VERSION=<.\SpatialGDK\Extras\core-sdk.version

    set BUILD_DIR=%~dp0SpatialGDK\Build
    set CORE_SDK_DIR=%BUILD_DIR%\core_sdk
    set WORKER_SDK_DIR=%~dp0SpatialGDK\Source\Public\WorkerSdk
    set BINARIES_DIR=%~dp0SpatialGDK\Binaries\ThirdParty\Improbable
    set SCHEMA_COPY_DIR=%~dp0..\..\..\spatial\schema\unreal\gdk
    set SCHEMA_STD_COPY_DIR=%~dp0..\..\..\spatial\build\dependencies\schema\standard_library
call :MarkEndOfBlock "Setup variables"

if not exist "%CORE_SDK_DIR%\core-sdk.version" goto NoCachedCoreSDK

set /p CACHED_CORE_SDK_VERSION=<"%CORE_SDK_DIR%\core-sdk.version"
if "%PINNED_CORE_SDK_VERSION%" == "%CACHED_CORE_SDK_VERSION%" (
    echo.
    echo CoreSDK version has not changed since the last run of Setup.bat. CoreSDK dependencies will be skipped.
    echo If you wish to re-download them, please delete %CORE_SDK_DIR% folder and run Setup.bat again.
    echo.
    goto SkipCoreSDKDependencies
)

:NoCachedCoreSDK

call :MarkStartOfBlock "Clean folders"
    rd /s /q "%CORE_SDK_DIR%"           2>nul
    rd /s /q "%WORKER_SDK_DIR%"         2>nul
    rd /s /q "%BINARIES_DIR%"           2>nul
    rd /s /q "%SCHEMA_STD_COPY_DIR%"    2>nul
call :MarkEndOfBlock "Clean folders"

call :MarkStartOfBlock "Create folders"
    md "%WORKER_SDK_DIR%"            >nul 2>nul
    md "%CORE_SDK_DIR%\schema"       >nul 2>nul
    md "%CORE_SDK_DIR%\tools"        >nul 2>nul
    md "%CORE_SDK_DIR%\worker_sdk"   >nul 2>nul
    md "%BINARIES_DIR%"              >nul 2>nul
    md "%SCHEMA_STD_COPY_DIR%"       >nul 2>nul
call :MarkEndOfBlock "Create folders"

call :MarkStartOfBlock "Retrieve dependencies"
    spatial package retrieve tools           schema_compiler-x86_64-win32           %PINNED_CORE_SDK_VERSION%       "%CORE_SDK_DIR%\tools\schema_compiler-x86_64-win32.zip"
    spatial package retrieve schema          standard_library                       %PINNED_CORE_SDK_VERSION%       "%CORE_SDK_DIR%\schema\standard_library.zip"
    spatial package retrieve worker_sdk      c-dynamic-x86-msvc_md-win32            %PINNED_CORE_SDK_VERSION%       "%CORE_SDK_DIR%\worker_sdk\c-dynamic-x86-msvc_md-win32.zip"
    spatial package retrieve worker_sdk      c-dynamic-x86_64-msvc_md-win32         %PINNED_CORE_SDK_VERSION%       "%CORE_SDK_DIR%\worker_sdk\c-dynamic-x86_64-msvc_md-win32.zip"
    spatial package retrieve worker_sdk      c-dynamic-x86_64-gcc_libstdcpp-linux   %PINNED_CORE_SDK_VERSION%       "%CORE_SDK_DIR%\worker_sdk\c-dynamic-x86_64-gcc_libstdcpp-linux.zip"
call :MarkEndOfBlock "Retrieve dependencies"

call :MarkStartOfBlock "Unpack dependencies"
    powershell -Command "Expand-Archive -Path \"%CORE_SDK_DIR%\worker_sdk\c-dynamic-x86-msvc_md-win32.zip\"             -DestinationPath \"%BINARIES_DIR%\Win32\" -Force; "^
                        "Expand-Archive -Path \"%CORE_SDK_DIR%\worker_sdk\c-dynamic-x86_64-msvc_md-win32.zip\"          -DestinationPath \"%BINARIES_DIR%\Win64\" -Force; "^
                        "Expand-Archive -Path \"%CORE_SDK_DIR%\worker_sdk\c-dynamic-x86_64-gcc_libstdcpp-linux.zip\"    -DestinationPath \"%BINARIES_DIR%\Linux\" -Force; "^
                        "Expand-Archive -Path \"%CORE_SDK_DIR%\tools\schema_compiler-x86_64-win32.zip\"                 -DestinationPath \"%BINARIES_DIR%\Programs\" -Force; "^
                        "Expand-Archive -Path \"%CORE_SDK_DIR%\schema\standard_library.zip\"                            -DestinationPath \"%BINARIES_DIR%\Programs\schema\" -Force;"

    xcopy /s /i /q "%BINARIES_DIR%\Win64\include" "%WORKER_SDK_DIR%"
call :MarkEndOfBlock "Unpack dependencies"

call :MarkStartOfBlock "Copy standard library schema"
    echo Copying standard library schemas to "%SCHEMA_STD_COPY_DIR%"
    xcopy /s /i /q "%BINARIES_DIR%\Programs\schema" "%SCHEMA_STD_COPY_DIR%"
call :MarkEndOfBlock "Copy standard library schema"

call :MarkStartOfBlock "Update cached CoreSDK version"
    echo %PINNED_CORE_SDK_VERSION%>%CORE_SDK_DIR%\core-sdk.version
call :MarkEndOfBlock "Update cached CoreSDK version"

:SkipCoreSDKDependencies

call :MarkStartOfBlock "Copy GDK schema"
    rd /s /q "%SCHEMA_COPY_DIR%"      2>nul
    md "%SCHEMA_COPY_DIR%"       >nul 2>nul

    echo Copying schemas to "%SCHEMA_COPY_DIR%".
    xcopy /s /i /q "%~dp0\SpatialGDK\Extras\schema" "%SCHEMA_COPY_DIR%"
call :MarkEndOfBlock "Copy GDK schema"

call :MarkStartOfBlock "Build C# utilities"
    %MSBUILD_EXE% /nologo /verbosity:minimal .\SpatialGDK\Build\Programs\Improbable.Unreal.Scripts\Improbable.Unreal.Scripts.sln /property:Configuration=Release
call :MarkEndOfBlock "Build C# utilities"

call :MarkEndOfBlock "%~0"

popd

echo UnrealGDK build completed successfully^!

if not defined NO_PAUSE (
    if not defined TEAMCITY_CAPTURE_ENV (
        pause
    )
)

exit /b %ERRORLEVEL%

:MarkStartOfBlock
if defined TEAMCITY_CAPTURE_ENV (
    echo ##teamcity[blockOpened name='%~1']
) else (
    echo Starting: %~1
)
exit /b 0

:MarkEndOfBlock
if defined TEAMCITY_CAPTURE_ENV (
    echo ##teamcity[blockClosed name='%~1']
) else (
    echo Finished: %~1
)
exit /b 0
