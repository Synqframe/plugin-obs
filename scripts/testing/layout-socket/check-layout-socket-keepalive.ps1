<#
Regression check for the plugin's layout websocket dropping every ~20 seconds.

The control API pings idle websockets every 20 s. libcurl answers with an
automatic PONG inside curl_ws_recv, and over TLS that send fails once a
CURLOPT_TIMEOUT_MS deadline set for the connect phase has passed, so every
connection died at the first server ping ("Failed sending data to the peer").
The plugin now bounds the connect phase with a progress callback instead.

This runs a local wss server that pings after the old deadline and checks that:
  1. the pre-fix behaviour still loses the connection (so the check can fail), and
  2. the current behaviour survives several pings, and
  3. the connect phase is still bounded against a server that never answers.

The first three checks exercise libcurl with a copy of the plugin's option set,
so they can demonstrate the pre-fix behaviour. The last two run the plugin's real
WebSocket client (ws-client.cpp, shared by the layout socket and the config
channel), without OBS or Qt: it must stay connected across server pings, and
stop() must return promptly even against a server that never answers. Needs cmake, a Visual Studio generator, python, and openssl (Git's copy
is fine). Uses the libcurl in .tools/obs-sdk.
#>
param([string]$ObsSdkRoot)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $here "..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($ObsSdkRoot)) { $ObsSdkRoot = Join-Path $repoRoot ".tools\obs-sdk\32.2.1" }
$deps = Join-Path $ObsSdkRoot ".deps\obs-deps-2026-07-15-x64"
$work = Join-Path $repoRoot "build_layout_socket_check"   # build*/ is git-ignored
New-Item -ItemType Directory -Force -Path $work | Out-Null

$openssl = (Get-Command openssl -ErrorAction SilentlyContinue).Source
if (!$openssl) { $openssl = "C:\Program Files\Git\usr\bin\openssl.exe" }
if (!(Test-Path $openssl)) { throw "openssl not found; install Git for Windows or add openssl to PATH." }

& cmake -S $here -B $work -G "Visual Studio 18 2026" -A x64 "-DCMAKE_PREFIX_PATH=$deps" "-DOBS_SOURCE_DIR=$ObsSdkRoot" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& cmake --build $work --config Release | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Copy-Item (Join-Path $deps "bin\libcurl.dll") (Join-Path $work "Release") -Force
$client = Join-Path $work "Release\client.exe"
$wsCheck = Join-Path $work "Release\wsclient-check.exe"

if (!(Test-Path (Join-Path $work "cert.pem"))) {
  # openssl reports key generation progress on stderr, which -ErrorAction Stop
  # would treat as a failure; judge it by exit code and output files instead.
  $env:MSYS_NO_PATHCONV = "1"
  $ErrorActionPreference = "Continue"
  & $openssl req -x509 -newkey rsa:2048 -nodes -keyout (Join-Path $work "key.pem") -out (Join-Path $work "cert.pem") -days 2 -subj "/CN=localhost" 2>&1 | Out-Null
  $opensslExit = $LASTEXITCODE
  $ErrorActionPreference = "Stop"
  Remove-Item Env:MSYS_NO_PATHCONV
  if ($opensslExit -ne 0 -or !(Test-Path (Join-Path $work "cert.pem"))) { throw "could not generate the test certificate" }
}

$failures = @()
function Run-Variant($variant, $seconds) {
  $server = Start-Process python -ArgumentList "`"$here\server.py`"", "8", "18767", "`"$work`"" -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $work "server-$variant.out") -RedirectStandardError (Join-Path $work "server-$variant.err")
  Start-Sleep -Seconds 2
  $env:SYNQ_WS_INSECURE = "1"
  try { $output = & $client "wss://localhost:18767/" $variant $seconds; $code = $LASTEXITCODE }
  finally { Remove-Item Env:SYNQ_WS_INSECURE; Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
  Write-Host "[$variant] $($output -join ' | ')"
  return $code
}

# The server pings at 8 s and 16 s; the connect-phase deadline is 5 s.
if ((Run-Variant "timeout" 12) -ne 1) { $failures += "pre-fix behaviour no longer loses the connection: this check cannot detect the bug" }
if ((Run-Variant "progress" 20) -ne 0) { $failures += "current behaviour lost the connection across server pings" }

$hang = Start-Process python -ArgumentList "`"$here\hang.py`"" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 1
$watch = [Diagnostics.Stopwatch]::StartNew()
& $client "ws://127.0.0.1:18766/" progress 30 | Out-Null
$code = $LASTEXITCODE; $watch.Stop()
Stop-Process -Id $hang.Id -Force -ErrorAction SilentlyContinue
Write-Host ("[handshake bound] exit {0} after {1:N1}s" -f $code, $watch.Elapsed.TotalSeconds)
if ($code -ne 2 -or $watch.Elapsed.TotalSeconds -gt 8) { $failures += "connect phase is not bounded against a server that never answers" }

# The real client: several server pings (8 s and 16 s) without a reconnect.
$server = Start-Process python -ArgumentList "`"$here\server.py`"", "8", "18767", "`"$work`"" -PassThru -WindowStyle Hidden `
  -RedirectStandardOutput (Join-Path $work "server-wsclient.out") -RedirectStandardError (Join-Path $work "server-wsclient.err")
Start-Sleep -Seconds 2
$env:SYNQ_WS_INSECURE = "1"
try { $output = & $wsCheck "wss://localhost:18767/" keepalive 20; $code = $LASTEXITCODE }
finally { Remove-Item Env:SYNQ_WS_INSECURE; Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
Write-Host "[wsclient keepalive] $($output -join ' | ')"
if ($code -ne 0) { $failures += "the plugin's WebSocket client lost the connection across server pings" }

# The real client: stop() interrupts a handshake that never completes.
$hang = Start-Process python -ArgumentList "`"$here\hang.py`"" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 1
$output = & $wsCheck "ws://127.0.0.1:18766/" stop
$code = $LASTEXITCODE
Stop-Process -Id $hang.Id -Force -ErrorAction SilentlyContinue
Write-Host "[wsclient stop] $($output -join ' | ')"
if ($code -ne 0) { $failures += "stop() waited on a server that never answers the upgrade" }

if ($failures.Count) { $failures | ForEach-Object { Write-Host "FAIL: $_" -ForegroundColor Red }; exit 1 }
Write-Host "PASS" -ForegroundColor Green
exit 0
