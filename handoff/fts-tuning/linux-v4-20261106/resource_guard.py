from pathlib import Path
import json,os,subprocess
r=Path(__file__).resolve().parent
mem={x.split(':')[0]:int(x.split()[1])*1024 for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')}
s=os.statvfs(r);linux_free=s.f_bavail*s.f_frsize
p=subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-Command',"(Get-CimInstance Win32_LogicalDisk -Filter \"DeviceID='D:'\").FreeSpace"],capture_output=True,text=True,timeout=20,check=True)
host_free=int(p.stdout.strip())
print('RESOURCE_GUARD',json.dumps({'host_D_free_GiB':round(host_free/2**30,2),'linux_free_GiB':round(linux_free/2**30,2),'memory_available_GiB':round(mem['MemAvailable']/2**30,2)}),flush=True)
assert host_free>=30*2**30, 'Windows D: free space below 30 GiB'
assert linux_free>=15*2**30, 'Linux free space below 15 GiB'
assert mem['MemAvailable']>=3*2**30, 'Available RAM below 3 GiB'
