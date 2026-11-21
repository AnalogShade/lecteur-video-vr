$ErrorActionPreference = 'Stop'

$deps = Join-Path $PSScriptRoot '.deps'
$bin = Join-Path $PSScriptRoot 'bin'
$obj = Join-Path $deps 'obj'
$exe = Join-Path $bin 'LecteurVideoVR.exe'
$stagedExe = Join-Path $obj 'LecteurVideoVR.exe'
New-Item -ItemType Directory -Path $deps, $bin, $obj -Force | Out-Null

function Get-Dependency($name, $asset, $url, $hash, $expected) {
    $archive = Join-Path $deps $asset
    $destination = Join-Path $deps $name
    if (!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive).Hash -ne $hash) {
        $temporary = "$archive.download"
        curl.exe --fail --silent --show-error --location $url --output $temporary
        if ($LASTEXITCODE -ne 0) { throw "Téléchargement impossible pour $name." }
        if ((Get-FileHash -LiteralPath $temporary).Hash -ne $hash) { throw "Somme SHA-256 invalide pour $name." }
        Move-Item -LiteralPath $temporary -Destination $archive -Force
    }
    if (!(Test-Path -LiteralPath (Join-Path $destination $expected))) {
        Expand-Archive -LiteralPath $archive -DestinationPath $destination -Force
    }
}

Get-Dependency 'libmpv' 'libmpv-v0.42.0-x86_64-w64-mingw32-lgpl.zip.zip' `
    'https://github.com/mpv-player/mpv/releases/download/v0.42.0/libmpv-v0.42.0-x86_64-w64-mingw32-lgpl.zip' `
    '8E5F77430EFC53EF175804BA6618527E3AF0448A0DCABF04D3CC72659D1A6FCE' 'include/mpv/render_d3d11.h'
Get-Dependency 'OpenXR' 'OpenXR.Loader.1.1.65.nupkg.zip' `
    'https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-1.1.65/OpenXR.Loader.1.1.65.nupkg' `
    '05638C526F07BA3A5F1084BBC3308D654D5167F844FFA1E35515325CB72E0C34' 'native/x64/release/lib/openxr_loader.lib'

$puffFolder = Join-Path $deps 'puff'
New-Item -ItemType Directory -Path $puffFolder -Force | Out-Null
$puffHashes = @{
    'puff.c' = '5B9D75AEB5BAF3575415BC6ADE3F2A02E50B6B971B3F8B4FDA2B03543BC6E52F'
    'puff.h' = '969B7BE2A930DB0CDCB19B0E5B29AE6741F5A8F663B6DBA6D647E12EC60CFA8E'
}
foreach ($filename in $puffHashes.Keys) {
    $file = Join-Path $puffFolder $filename
    if (!(Test-Path -LiteralPath $file) -or (Get-FileHash -LiteralPath $file).Hash -ne $puffHashes[$filename]) {
        $download = "$file.download"
        curl.exe --fail --silent --show-error --location "https://raw.githubusercontent.com/madler/zlib/v1.3.1/contrib/puff/$filename" --output $download
        if ($LASTEXITCODE -ne 0 -or (Get-FileHash -LiteralPath $download).Hash -ne $puffHashes[$filename]) {
            throw "Dépendance $filename indisponible ou incorrecte."
        }
        Move-Item -LiteralPath $download -Destination $file -Force
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Installe Visual Studio Build Tools et le composant Développement Desktop en C++.' }
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw 'Le compilateur x64 de Visual Studio est introuvable.' }
$vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
$command = 'call "{0}" >nul && cl.exe /nologo /std:c++17 /utf-8 /EHsc /permissive- /W3 /O2 /MT /DUNICODE /D_UNICODE /I..\.deps\puff /I..\.deps\libmpv\include /I..\.deps\OpenXR\include ..\src\*.cpp ..\.deps\puff\puff.c /Fo..\.deps\obj\ /Fe:..\.deps\obj\LecteurVideoVR.exe /link /SUBSYSTEM:WINDOWS /LIBPATH:..\.deps\OpenXR\native\x64\release\lib openxr_loader.lib d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib uuid.lib comctl32.lib' -f $vcvars
Push-Location -LiteralPath $bin
try {
    cmd.exe /d /c $command
    if ($LASTEXITCODE -ne 0) { throw 'Échec de la compilation du lecteur vidéo VR.' }
} finally {
    Pop-Location
}

function Install-RuntimeFile($source, $name) {
    $from = Join-Path $deps $source
    $to = Join-Path $bin $name
    if (!(Test-Path -LiteralPath $to) -or (Get-FileHash -LiteralPath $from).Hash -ne (Get-FileHash -LiteralPath $to).Hash) {
        Copy-Item -LiteralPath $from -Destination $to -Force
    }
}

Install-RuntimeFile 'libmpv/libmpv-2.dll' 'libmpv-2.dll'
Install-RuntimeFile 'libmpv/LICENSE.LGPL' 'libmpv-LICENSE.LGPL'
Install-RuntimeFile 'OpenXR/native/x64/release/bin/openxr_loader.dll' 'openxr_loader.dll'
Install-RuntimeFile 'OpenXR/OpenXR.Loader.nuspec' 'OpenXR.Loader.nuspec'
if (Test-Path -LiteralPath $exe) {
    [IO.File]::Replace($stagedExe, $exe, (Join-Path $obj 'LecteurVideoVR.previous.exe'))
} else {
    [IO.File]::Move($stagedExe, $exe)
}
Write-Host "Lecteur Windows disponible dans $exe"
