$port = New-Object System.IO.Ports.SerialPort 'COM6',115200,'None',8,'One'
$opened = $false
for ($i = 1; $i -le 8; $i++) {
  try { $port.Open(); $opened = $true; break } catch { Write-Output ('retry ' + $i); Start-Sleep 3 }
}
if (-not $opened) { Write-Output 'FATAL: COM6 busy'; exit }
$deadline = (Get-Date).AddSeconds(70)
$sb = New-Object System.Text.StringBuilder
while ((Get-Date) -lt $deadline) {
  if ($port.BytesToRead -gt 0) {
    $n = $port.BytesToRead
    $buf = New-Object byte[] $n
    [void]$port.Read($buf, 0, $n)
    [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf))
  } else { Start-Sleep -Milliseconds 100 }
}
$port.Close()
$path = 'Join-Path $PSScriptRoot 'serial_log.txt''
$sb.ToString() | Out-File -Encoding utf8 $path
Write-Output ('SAVED ' + $sb.Length + ' chars to ' + $path)
