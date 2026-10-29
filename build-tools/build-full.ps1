$ErrorActionPreference = "Stop"

$project = Split-Path -Parent $PSScriptRoot
$environment = "mk-signal-div-v2-1-stable"
$build = Join-Path $project ".pio\build\$environment"
$dist = Join-Path $project "dist"
$output = Join-Path $dist "ESP32-DIV-1.7.2-stable-by_MK_SIGNAL_v1.67-DIV-V2.1-full.bin"
$platformio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"
$python = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
$esptool = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py"
$bootApp = Join-Path $env:USERPROFILE ".platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"

# The ESP32-S3 framework has a large include list.  Windows can otherwise hit
# its command-line length limit when this package is unpacked below a long
# customer directory, so compile through temporary short drive aliases.
$availableLetters = @("P", "Q", "R", "S", "T", "U")
$occupiedLetters = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object { $_.Name.ToUpperInvariant() })
$freeLetters = @($availableLetters | Where-Object { $occupiedLetters -notcontains $_ })
if ($freeLetters.Count -lt 2) {
  throw "Two free temporary drive letters are required to build this package"
}

$projectLetter = $freeLetters[0]
$coreLetter = $freeLetters[1]
$projectDrive = "${projectLetter}:"
$coreDrive = "${coreLetter}:"
$oldCoreDir = [Environment]::GetEnvironmentVariable("PLATFORMIO_CORE_DIR", "Process")

try {
  & subst.exe $projectDrive $project
  if ($LASTEXITCODE -ne 0) { throw "Could not create the temporary project drive" }
  & subst.exe $coreDrive (Join-Path $env:USERPROFILE ".platformio")
  if ($LASTEXITCODE -ne 0) { throw "Could not create the temporary PlatformIO drive" }

  $env:PLATFORMIO_CORE_DIR = "$coreDrive\"
  $shortProject = "$projectDrive\"
  $shortPlatformio = "$coreDrive\penv\Scripts\platformio.exe"

  & $shortPlatformio run --project-dir $shortProject --target clean
  if ($LASTEXITCODE -ne 0) { throw "PlatformIO clean failed" }

  # Serial compilation also avoids intermittent CreateProcess failures when
  # several large C++ translation units start simultaneously.
  & $shortPlatformio run --project-dir $shortProject -j 1
  if ($LASTEXITCODE -ne 0) { throw "PlatformIO build failed" }
}
finally {
  if ($null -eq $oldCoreDir) {
    Remove-Item Env:PLATFORMIO_CORE_DIR -ErrorAction SilentlyContinue
  }
  else {
    $env:PLATFORMIO_CORE_DIR = $oldCoreDir
  }
  & subst.exe $coreDrive /D 2>$null
  & subst.exe $projectDrive /D 2>$null
}

New-Item -ItemType Directory -Force -Path $dist | Out-Null
& $python $esptool --chip esp32s3 merge_bin `
  -o $output `
  --flash_mode keep --flash_freq keep --flash_size 16MB `
  0x0 (Join-Path $build "bootloader.bin") `
  0x8000 (Join-Path $build "partitions.bin") `
  0xe000 $bootApp `
  0x10000 (Join-Path $build "firmware.bin")
if ($LASTEXITCODE -ne 0) { throw "Full image merge failed" }

$file = Get-Item -LiteralPath $output
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $output
Write-Host "FULL_BIN=$($file.FullName)"
Write-Host "SIZE=$($file.Length)"
Write-Host "SHA256=$($hash.Hash)"
