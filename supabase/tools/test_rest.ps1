# EcoBio-Matrix · phase 3: test the real Supabase project from outside, with the PUBLIC key only.
#
#   powershell -ExecutionPolicy Bypass -File test_rest.ps1 -Url https://xxxx.supabase.co -Key sb_publishable_xxxx
#
# Sends the same kind of requests the ESP32 sends (POST JSON array, Prefer: return=minimal), reads them back,
# then checks that UPDATE and DELETE are refused. It adds 1 test reading (dated 2000-01-01, so it never shows
# as "latest") and 1 event of type connection_test. Clean-up SQL is in ../README.md.
# Never pass the secret / service_role key or the database password to this script.
param(
  [Parameter(Mandatory = $true)][string]$Url,
  [Parameter(Mandatory = $true)][string]$Key
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$Url = $Url.TrimEnd('/')
if ($Key -like 'sb_secret_*' -or $Key -match '"role"\s*:\s*"service_role"') { Write-Host 'Use the PUBLIC key (publishable / anon), not the secret one.'; exit 2 }

$headers = @{ 'apikey' = $Key; 'Prefer' = 'return=minimal' }
if ($Key.StartsWith('eyJ')) { $headers['Authorization'] = "Bearer $Key" }   # legacy anon JWT only, same rule as sketch.ino

function Call($method, $path, $body) {
  $p = @{ Uri = "$Url/rest/v1/$path"; Method = $method; Headers = $headers; UseBasicParsing = $true; ContentType = 'application/json; charset=utf-8' }
  if ($body) { $p.Body = [Text.Encoding]::UTF8.GetBytes($body) }
  try {
    $r = Invoke-WebRequest @p
    return @{ code = [int]$r.StatusCode; text = [Text.Encoding]::UTF8.GetString($r.RawContentStream.ToArray()) }
  } catch {
    $resp = $_.Exception.Response
    if ($null -eq $resp) { return @{ code = -1; text = $_.Exception.Message } }
    $sr = New-Object IO.StreamReader($resp.GetResponseStream(), [Text.Encoding]::UTF8)
    return @{ code = [int]$resp.StatusCode; text = $sr.ReadToEnd() }
  }
}

$pass = 0; $fail = 0
function Check($name, $cond, $info) {
  if ($cond) { $script:pass++; Write-Host "PASS  $name  $info" } else { $script:fail++; Write-Host "FAIL  $name  $info" }
}

$reading = '[{"ts":"2000-01-01T00:00:00.000Z","ph":7.50,"orp_in_mv":101,"do_mgl":6.00,"temp_c":25.00,"turbidity":2.00,"uv_pct":85.0,"orp_cl_mv":700,"level_pct":49.4,"valve1":"normal","valve2":"normal","air_pump":true,"cl_pump":false,"safe_mode":false}]'
$evt = '[{"type":"connection_test","reason":"connection test from test_rest.ps1","valve":null,"value":null,"decided_by":"rule"}]'

$r = Call 'POST' 'sensor_readings' $reading;  Check 'insert reading (like the ESP32)' ($r.code -eq 201) "HTTP $($r.code) $($r.text)"
$r = Call 'POST' 'events' $evt;             Check 'insert event (like the ESP32)'   ($r.code -eq 201) "HTTP $($r.code) $($r.text)"
$r = Call 'GET' 'events?select=id,ts,type&type=eq.connection_test&order=ts.desc&limit=1' $null
Check 'read events back' (($r.code -eq 200) -and ($r.text -match 'connection_test')) "HTTP $($r.code) $($r.text)"
$r = Call 'GET' 'sensor_readings?select=id,ts,ph,valve1&order=ts.desc&limit=1' $null
Check 'read readings back' (($r.code -eq 200) -and ($r.text -match 'valve1')) "HTTP $($r.code)"
$r = Call 'PATCH' 'events?type=eq.connection_test' '{"reason":"changed"}'
Check 'update refused' (($r.code -eq 401) -or ($r.code -eq 403)) "HTTP $($r.code) $($r.text)"
$r = Call 'DELETE' 'events?type=eq.connection_test' $null
Check 'delete refused' (($r.code -eq 401) -or ($r.code -eq 403)) "HTTP $($r.code) $($r.text)"
$r = Call 'DELETE' 'sensor_readings?ts=lt.2001-01-01' $null
Check 'delete readings refused' (($r.code -eq 401) -or ($r.code -eq 403)) "HTTP $($r.code) $($r.text)"

Write-Host ""
Write-Host "$pass passed, $fail failed"
if ($fail -gt 0) { exit 1 }
