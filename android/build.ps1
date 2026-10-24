# Build the Android widget APK without Gradle: aapt2 + javac + d8 + zipalign + apksigner
# Requires: JDK 17, Android SDK build-tools 34.0.0, platforms/android-34
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1

$ErrorActionPreference = 'Stop'
$base = $PSScriptRoot
$sdk  = Join-Path $base 'sdk'
$app  = Join-Path $base 'app'
$bt   = Join-Path $sdk 'build-tools\34.0.0'
$jar  = Join-Path $sdk 'platforms\android-34\android.jar'
$jdk  = (Get-ChildItem (Join-Path $base 'jdk') -Directory | Select-Object -First 1).FullName
$out  = Join-Path $base 'out'

$env:JAVA_HOME = $jdk
$env:PATH = "$($jdk)\bin;$bt;$env:PATH"
$env:TEMP = Join-Path $base 'tmp'
$env:TMP  = $env:TEMP

Remove-Item (Join-Path $out 'classes'), (Join-Path $out 'dex'), (Join-Path $out 'gen') -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $out 'classes'), (Join-Path $out 'dex'), (Join-Path $out 'gen') | Out-Null

Write-Host '0. guard: no forbidden views in widget layout'
$wxml = Get-Content (Join-Path $app 'res\layout\widget.xml') -Raw
foreach ($bad in @('<View', '<include', '<merge', '<fragment', '<WebView', '<ScrollView')) {
    if ($wxml.Contains($bad)) { Write-Host ('ERROR: forbidden element ' + $bad + ' in widget.xml'); exit 3 }
}
Write-Host '   ok'
Write-Host '1. aapt2 compile'
& (Join-Path $bt 'aapt2.exe') compile --dir (Join-Path $app 'res') -o (Join-Path $out 'res.zip')

Write-Host '2. aapt2 link'
& (Join-Path $bt 'aapt2.exe') link -o (Join-Path $out 'base.apk') -I $jar `
    --manifest (Join-Path $app 'AndroidManifest.xml') -R (Join-Path $out 'res.zip') `
    --java (Join-Path $out 'gen') --min-sdk-version 23 --target-sdk-version 34 `
    --version-code 4 --version-name '1.0.3' --auto-add-overlay

Write-Host '3. javac (UTF-8!)'
$srcs = Get-ChildItem (Join-Path $app 'src') -Recurse -Filter *.java | ForEach-Object { $_.FullName }
$rjava = Get-ChildItem (Join-Path $out 'gen') -Recurse -Filter R.java | ForEach-Object { $_.FullName }
& (Join-Path $jdk 'bin\javac.exe') --release 8 -encoding UTF-8 -classpath $jar -d (Join-Path $out 'classes') $srcs $rjava

Write-Host '4. d8'
$cls = Get-ChildItem (Join-Path $out 'classes') -Recurse -Filter *.class | ForEach-Object { $_.FullName }
& (Join-Path $bt 'd8.bat') --min-api 23 --lib $jar --output (Join-Path $out 'dex') $cls

Write-Host '5. pack dex into apk'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open((Join-Path $out 'base.apk'), 'Update')
[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $out 'dex\classes.dex'), 'classes.dex', [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
$zip.Dispose()

Write-Host '6. zipalign'
& (Join-Path $bt 'zipalign.exe') -f -p 4 (Join-Path $out 'base.apk') (Join-Path $out 'aligned.apk')

Write-Host '7. sign'
$ks = Join-Path $base 'widget.keystore'
if (-not (Test-Path $ks)) {
    & (Join-Path $jdk 'bin\keytool.exe') -genkeypair -keystore $ks -alias aqua -keyalg RSA -keysize 2048 `
        -validity 12000 -storepass aquarium -keypass aquarium -dname 'CN=Aquarium Widget, O=Aquarium, C=RU'
}
& (Join-Path $bt 'apksigner.bat') sign --ks $ks --ks-pass pass:aquarium --key-pass pass:aquarium `
    --out (Join-Path $out 'aquarium-widget.apk') (Join-Path $out 'aligned.apk')

$apk = Join-Path $out 'aquarium-widget.apk'
Write-Host ('APK size: ' + (Get-Item $apk).Length + ' bytes')

# --- verify that Russian strings inside the dex are real UTF-8 (not cp1251 mojibake) ---
$dex = [System.IO.File]::ReadAllBytes((Join-Path $out 'dex\classes.dex'))
function Find-Bytes([byte[]]$hay, [byte[]]$needle) {
    $limit = $hay.Length - $needle.Length
    for ($i = 0; $i -le $limit; $i++) {
        $ok = $true
        for ($j = 0; $j -lt $needle.Length; $j++) {
            if ($hay[$i + $j] -ne $needle[$j]) { $ok = $false; break }
        }
        if ($ok) { return $true }
    }
    return $false
}
function HexToBytes([string]$hex) {
    $arr = New-Object System.Collections.Generic.List[byte]
    for ($i = 0; $i -lt $hex.Length; $i += 2) { $arr.Add([Convert]::ToByte($hex.Substring($i, 2), 16)) }
    return $arr.ToArray()
}
# UTF-8 hex of: "Akvarium" in Russian (Аквариум) and "Dobavit vidzhet" (Добавить виджет)
$needle1 = HexToBytes 'D090D0BAD0B2D0B0D180D0B8D183D0BC'
$needle2 = HexToBytes 'D094D0BED0B1D0B0D0B2D0B8D182D18C'
# cp1251 mojibake would contain the pair for "РІ" style garbage: D0A0 D0B2 (Р + І) is normal,
# so instead check that the cp1251-read-at-UTF8 sequence "Ð" (D0 90 in cp1251 = 'Р') is absent
$bad = HexToBytes 'D0A0D0B5D0B0'
$ok1 = Find-Bytes $dex $needle1
$ok2 = Find-Bytes $dex $needle2
Write-Host ('UTF-8 string "Akvarium" in dex: ' + $ok1)
Write-Host ('UTF-8 string "Dobavit vidzhet" in dex: ' + $ok2)
if (-not $ok1 -or -not $ok2) { Write-Host 'WARNING: Cyrillic strings are not UTF-8 encoded!'; exit 2 }
Write-Host 'OK: strings are proper UTF-8'
