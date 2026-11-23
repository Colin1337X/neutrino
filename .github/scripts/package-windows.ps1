#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$work = Join-Path $env:RUNNER_TEMP 'neutrino-release'
$dist = Join-Path $env:RUNNER_TEMP 'neutrino-dist'
$package = Join-Path $work 'package'
$licenses = Join-Path $package 'licenses'
New-Item -ItemType Directory -Force -Path $licenses, $dist | Out-Null

function Expand-VerifiedArchive($Url, $Sha256) {
    $archive = Join-Path $work ($Url.Split('/')[-1])
    if (!(Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri $Url -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Sha256) {
        throw "SHA-256 verification failed for $archive"
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $work -Force
}

$zigRelease = 'zig-x86_64-windows-0.15.2'
$curlRelease = 'curl-8.23.0_9-win64-mingw'
Expand-VerifiedArchive "https://ziglang.org/download/0.15.2/$zigRelease.zip" `
    '3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c'
Expand-VerifiedArchive "https://curl.se/windows/dl-8.23.0_9/$curlRelease.zip" `
    '89cb71930f4242d884e8e349463455932ab1d089b20f4f99318261a3cbff9167'
$zig = Join-Path $work "$zigRelease/zig.exe"
$curl = Join-Path $work $curlRelease
$build = Join-Path $work 'build'

cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_C_COMPILER=$zig" -DCMAKE_C_COMPILER_ARG1=cc `
    -DCMAKE_C_COMPILER_TARGET=x86_64-windows-gnu -DCURL_NO_CURL_CMAKE=ON `
    "-DCURL_INCLUDE_DIR=$curl/include" "-DCURL_LIBRARY=$curl/lib/libcurl.dll.a"
cmake --build "$build" --parallel 3
Copy-Item -LiteralPath "$curl/bin/libcurl-x64.dll" -Destination $build
ctest --test-dir "$build" --output-on-failure --no-tests=error

Copy-Item -LiteralPath "$build/neutrino.exe", "$build/libcurl-x64.dll", 'LICENSE' -Destination $package
Copy-Item -LiteralPath 'third_party/cjson/LICENSE' -Destination (Join-Path $licenses 'cJSON.txt')
Copy-Item -LiteralPath "$curl/COPYING.txt" -Destination (Join-Path $licenses 'libcurl.txt')
foreach ($notice in Get-ChildItem -LiteralPath "$curl/dep" -File -Recurse |
        Where-Object Name -Match '^(LICENSE|LICENCE|COPYING|NOTICE)(\..+)?$') {
    $destination = Join-Path $licenses $notice.Directory.Name
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath $notice.FullName -Destination $destination
}

$exe = Join-Path $package 'neutrino.exe'
if ((& $exe --demo --snapshot | Out-String) -notmatch 'Neutrino') {
    throw 'The packaged executable failed the demo smoke check.'
}
@("Source $(git rev-parse HEAD)", 'Platform windows-x64', "Bundled libcurl $curlRelease", (& $exe --version)) |
    Set-Content -LiteralPath (Join-Path $package 'build-info.txt') -Encoding utf8
Compress-Archive -Path "$package/*" `
    -DestinationPath "$dist/neutrino-$env:PACKAGE_ID-windows-x64.zip" -Force
