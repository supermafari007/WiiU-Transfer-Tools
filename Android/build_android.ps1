$ErrorActionPreference = "Stop"

$Root    = $PSScriptRoot
$AppMain = Join-Path $Root "app/src/main"
$Build   = Join-Path $Root "build"
if (Test-Path $Build) { Remove-Item $Build -Recurse -Force }
New-Item -ItemType Directory -Force "$Build/res_compiled","$Build/gen","$Build/classes","$Build/dex" | Out-Null

$SdkBase = "C:/Users/manue/.gemini/antigravity/brain/b39c63c2-fa57-495a-b5bb-73a2881ff1ef/scratch/android_sdk"
$JdkBin  = (Get-ChildItem "$SdkBase/jdk17" -Directory | Select-Object -First 1).FullName + "/bin"
$BtDir   = (Get-ChildItem "$SdkBase/build_tools" -Directory | Select-Object -First 1).FullName
$PlatDir = (Get-ChildItem "$SdkBase/platform34" -Directory | Select-Object -First 1).FullName

$env:JAVA_HOME = (Get-ChildItem "$SdkBase/jdk17" -Directory | Select-Object -First 1).FullName
$env:PATH      = "$JdkBin;$BtDir;$env:PATH"

$Java          = "$JdkBin/java.exe"
$Javac         = "$JdkBin/javac.exe"
$Keytool       = "$JdkBin/keytool.exe"
$Aapt2         = "$BtDir/aapt2.exe"
$ZipAlign      = "$BtDir/zipalign.exe"
$D8Jar         = "$BtDir/lib/d8.jar"
$ApkSignerJar  = "$BtDir/lib/apksigner.jar"
$AndroidJar    = "$PlatDir/android.jar"

Write-Host "[1/6] Compilando recursos con aapt2..."
& $Aapt2 compile --dir "$AppMain/res" -o "$Build/res_compiled/res.zip"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[2/6] Enlazando recursos y generando R.java..."
& $Aapt2 link "$Build/res_compiled/res.zip" `
    -I $AndroidJar `
    --manifest "$AppMain/AndroidManifest.xml" `
    --java "$Build/gen" `
    -o "$Build/unsigned_unaligned.apk"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[3/6] Compilando codigo Java (MainActivity + NfsaTransferServer)..."
$JavaFiles = Get-ChildItem "$AppMain/java","$Build/gen" -Recurse -Filter "*.java" | % { $_.FullName }
& $Javac -source 1.8 -target 1.8 -encoding UTF-8 -bootclasspath $AndroidJar -d "$Build/classes" $JavaFiles
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[4/6] Convirtiendo bytecode a Dalvik DEX (d8, min-api 28 Android 9+)..."
$ClassFiles = Get-ChildItem "$Build/classes" -Recurse -Filter "*.class" | % { $_.FullName }
& $Java -cp $D8Jar com.android.tools.r8.D8 --min-api 28 --lib $AndroidJar --output "$Build/dex" $ClassFiles
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Insertar classes.dex en la raiz del APK
python -c "
import zipfile
with zipfile.ZipFile(r'$Build/unsigned_unaligned.apk', 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(r'$Build/dex/classes.dex', 'classes.dex')
"

Write-Host "[5/6] Alineando APK con zipalign..."
& $ZipAlign -f -p 4 "$Build/unsigned_unaligned.apk" "$Build/aligned.apk"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[6/6] Firmando APK (v1 + v2 + v3) con apksigner..."
$Keystore = Join-Path $Root "release_key.jks"
if (-not (Test-Path $Keystore)) {
    & $Keytool -genkeypair -v -keystore $Keystore -storepass wiiutransfer -keypass wiiutransfer `
        -alias eldiuca -keyalg RSA -keysize 2048 -validity 10000 `
        -dname "CN=El Diuca, OU=Wii U Transfer Tools, O=El Diuca, L=Santiago, C=CL"
}

$OutApk = Join-Path $Root "WiiU_Transfer_Tools_Android.apk"
& $Java -jar $ApkSignerJar sign `
    --v1-signing-enabled true `
    --v2-signing-enabled true `
    --v3-signing-enabled true `
    --ks $Keystore `
    --ks-pass pass:wiiutransfer `
    --key-pass pass:wiiutransfer `
    --ks-key-alias eldiuca `
    --out $OutApk `
    "$Build/aligned.apk"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $Java -jar $ApkSignerJar verify --verbose $OutApk
Write-Host ("APK listo: {0} ({1:N0} bytes)" -f $OutApk, (Get-Item $OutApk).Length)
