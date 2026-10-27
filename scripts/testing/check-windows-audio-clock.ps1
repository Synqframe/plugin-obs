param(
  # A DirectShow audio device name, as ffmpeg -list_devices true -f dshow -i dummy shows it.
  [Parameter(Mandatory = $true)]
  [string]$Device,
  [ValidateRange(2, 30)]
  [int]$DurationSeconds = 5,
  [ValidateSet(44100, 48000)]
  [int]$SampleRate = 48000
)

$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
$arguments = @(
  "-hide_banner", "-nostats",
  "-f", "dshow",
  "-audio_buffer_size", "20",
  "-sample_rate", $SampleRate,
  "-channels", "2",
  "-i", "audio=$Device",
  "-t", $DurationSeconds,
  "-map", "0:a:0",
  "-af", "ashowinfo",
  "-f", "null", "NUL"
)

$lines = & $ffmpeg @arguments 2>&1
$frames = @()
foreach ($line in $lines) {
  if ($line -match 'n:(\d+) pts:(\d+) pts_time:([0-9.]+).*rate:(\d+) nb_samples:(\d+)') {
    $frames += [pscustomobject]@{
      Number = [int]$matches[1]
      Pts = [double]$matches[3]
      Rate = [int]$matches[4]
      Samples = [int]$matches[5]
    }
  }
}

if ($frames.Count -lt 2) {
  Write-Error "Could not sample '$Device'. It may be unavailable or exclusively owned."
  exit 2
}

$gaps = @()
for ($index = 1; $index -lt $frames.Count; $index++) {
  $gaps += $frames[$index].Pts - $frames[$index - 1].Pts
}
$capturedSeconds = ($frames | Measure-Object Samples -Sum).Sum / $SampleRate
$timelineSeconds = $frames[-1].Pts - $frames[0].Pts + ($frames[-1].Samples / $SampleRate)
$coverage = if ($timelineSeconds -gt 0) { $capturedSeconds / $timelineSeconds } else { 0 }
$maximumGap = ($gaps | Measure-Object -Maximum).Maximum

[pscustomobject]@{
  Device = $Device
  RequestedRate = $SampleRate
  Frames = $frames.Count
  TimelineSeconds = [math]::Round($timelineSeconds, 3)
  CapturedSeconds = [math]::Round($capturedSeconds, 3)
  CoveragePercent = [math]::Round($coverage * 100, 1)
  MaximumGapMilliseconds = [math]::Round($maximumGap * 1000, 1)
} | Format-List

if ($coverage -lt 0.95 -or $maximumGap -gt 0.05) {
  Write-Error "Audio endpoint clock is unhealthy. Reconnect the USB device (or restart Windows Audio) before streaming."
  exit 1
}

Write-Host "Audio endpoint clock is healthy." -ForegroundColor Green
