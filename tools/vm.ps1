# vm.ps1 - create and drive a VMware Workstation VM for testing KestrelOS.
#
#   .\tools\vm.ps1 create  [-Boot iso|disk] [-Controller nvme|sata]
#   .\tools\vm.ps1 start   [-Gui]
#   .\tools\vm.ps1 stop
#   .\tools\vm.ps1 shot    -Out shot.png
#   .\tools\vm.ps1 serial            # print the guest's serial output
#   .\tools\vm.ps1 reset
#
# The VM is configured with EFI firmware (Secure Boot off, since the loader is
# unsigned), a serial port redirected to a file - which is the primary
# diagnostic channel before the framebuffer console is up - and a VNC server so
# input can be driven without VMware Tools in the guest.
param(
    [Parameter(Position = 0)][string]$Action = "status",
    [string]$Boot = "iso",
    [string]$Controller = "nvme",
    [string]$Name = "kestrel",
    [string]$Out = "",
    [int]$MemoryMB = 2048,
    [int]$Cpus = 2,
    [int]$VncPort = 5940,
    [int]$Width = 1920,
    [int]$Height = 1080,
    # How many virtual displays to give the VM.  One unless asked otherwise;
    # more is for exercising multi-display (extend/mirror/only-other, #7).
    [int]$Displays = 1,
    [switch]$Gui,
    # The adapter's own drawing engine.  Off unless asked for: it needs a
    # window on the host, and enabling it without one makes the remote
    # display unusably slow.
    [switch]$Enable3D,
    [switch]$Fresh
)
$ErrorActionPreference = "Stop"

$root    = Split-Path -Parent $PSScriptRoot
$vmDir   = Join-Path $root "build\vm\$Name"
$vmx     = Join-Path $vmDir "$Name.vmx"
$serial  = Join-Path $vmDir "serial.log"
$vmrun   = "C:\Program Files\VMware\VMware Workstation\vmrun.exe"
$vdiskmgr= "C:\Program Files\VMware\VMware Workstation\vmware-vdiskmanager.exe"

if (-not (Test-Path $vmrun)) { throw "vmrun not found at $vmrun" }

function New-Vm {
    if ($Fresh -and (Test-Path $vmDir)) { Remove-Item -Recurse -Force $vmDir }
    New-Item -ItemType Directory -Force $vmDir | Out-Null

    $iso = Join-Path $root "out\kestrelos.iso"
    $img = Join-Path $root "out\kestrelos.img"

    # VMware will not boot from a descriptor that points outside the VM folder
    # on some versions, so keep a private copy of the disk here.
    $diskRaw = Join-Path $vmDir "kestrel-flat.vmdk"
    $diskVmdk = Join-Path $vmDir "kestrel.vmdk"
    if (-not (Test-Path $diskRaw) -or (Test-Path $img -NewerThan (Get-Item $diskRaw).LastWriteTime)) {
        Copy-Item $img $diskRaw -Force
    }
    $sectors = [int64]((Get-Item $diskRaw).Length / 512)
    $desc = @"
# Disk DescriptorFile
version=1
encoding="UTF-8"
CID=fffffffe
parentCID=ffffffff
isNativeSnapshot="no"
createType="monolithicFlat"

# Extent description
RW $sectors FLAT "kestrel-flat.vmdk" 0

# The Disk Data Base
#DDB

ddb.adapterType = "lsilogic"
ddb.geometry.cylinders = "$([math]::Floor($sectors / (255*63)))"
ddb.geometry.heads = "255"
ddb.geometry.sectors = "63"
ddb.virtualHWVersion = "21"
"@
    [System.IO.File]::WriteAllText($diskVmdk, $desc, (New-Object System.Text.UTF8Encoding($false)))

    # A second, blank disk gives the installer somewhere to install to that is
    # not the disk it booted from.
    $target = Join-Path $vmDir "target.vmdk"
    if (-not (Test-Path $target)) {
        & $vdiskmgr -c -s 8GB -a lsilogic -t 0 $target | Out-Null
    }

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('.encoding = "UTF-8"')
    $lines.Add('config.version = "8"')
    $lines.Add('virtualHW.version = "21"')
    $lines.Add('displayName = "KestrelOS test"')
    $lines.Add('guestOS = "other-64"')
    $lines.Add('firmware = "efi"')
    $lines.Add('uefi.secureBoot.enabled = "FALSE"')
    $lines.Add('bios.bootDelay = "1000"')
    $lines.Add("memsize = `"$MemoryMB`"")
    $lines.Add("numvcpus = `"$Cpus`"")
    $lines.Add('cpuid.coresPerSocket = "1"')
    $lines.Add('vmci0.present = "FALSE"')
    $lines.Add('tools.syncTime = "FALSE"')
    $lines.Add('msg.autoAnswer = "TRUE"')
    # A hand-written VMX has to declare the PCI bridges itself.  Without them
    # there are no PCIe slots to put controllers in, and VMware reports
    # "No PCIe slot available" for whichever device it reaches first.
    $lines.Add('pciBridge0.present = "TRUE"')
    $lines.Add('pciBridge4.present = "TRUE"')
    $lines.Add('pciBridge4.virtualDev = "pcieRootPort"')
    $lines.Add('pciBridge4.functions = "8"')
    $lines.Add('pciBridge5.present = "TRUE"')
    $lines.Add('pciBridge5.virtualDev = "pcieRootPort"')
    $lines.Add('pciBridge5.functions = "8"')
    $lines.Add('pciBridge6.present = "TRUE"')
    $lines.Add('pciBridge6.virtualDev = "pcieRootPort"')
    $lines.Add('pciBridge6.functions = "8"')
    $lines.Add('pciBridge7.present = "TRUE"')
    $lines.Add('pciBridge7.virtualDev = "pcieRootPort"')
    $lines.Add('pciBridge7.functions = "8"')

    # Devices the OS does not drive are left out to keep the machine simple.
    $lines.Add('floppy0.present = "FALSE"')
    # An HD Audio controller, so the audio driver has something to bring up.
    # VMware's is the standard Intel part with its own codec behind it.
    $lines.Add('sound.present = "TRUE"')
    $lines.Add('sound.virtualDev = "hdaudio"')
    $lines.Add('sound.autodetect = "TRUE"')
    $lines.Add('sound.startConnected = "TRUE"')
    # The adapter's own three-dimensional engine, which forwards drawing to
    # whatever real graphics card the host has.  It also needs the machine to
    # be started with a window: without somewhere on the host to render to,
    # VMware loads its software renderer and the guest is told there is no 3D
    # engine at all.
    if ($Enable3D) {
        $lines.Add('mks.enable3d = "TRUE"')
        $lines.Add('vmotion.checkpoint3DSupported = "TRUE"')
        # VMware keeps a list of host graphics drivers it will not use, and a
        # card newer than the VMware build is on it by default - which is how a
        # machine with a perfectly good GPU ends up with the software renderer.
        # This says to use it anyway.
        $lines.Add('mks.gl.allowBlacklistedDrivers = "TRUE"')
        # Memory for the objects the card owns.  Without a real amount the
        # adapter leaves its older three-dimensional capability bit clear, and
        # what it reports then does not match a working guest on this host.
        $lines.Add('svga.graphicsMemoryKB = "8388608"')
        # And the guest saying it understands surfaces that live in memory it
        # provided, which is the whole model the drawing contexts sit on.
        $lines.Add('svga.guestBackedPrimaryAware = "TRUE"')

    } else {
        # Off by default.  With it on and no window to render into, VMware's
        # renderer thread keeps trying and the remote display becomes unusably
        # slow - which matters because every other stage is driven over it.
        $lines.Add('mks.enable3d = "FALSE"')
    }

    # An xHCI controller, so the USB driver has something to bring up.
    $lines.Add('usb.present = "TRUE"')
    $lines.Add('usb_xhci.present = "TRUE"')
    $lines.Add('ehci.present = "FALSE"')
    $lines.Add('usb.generic.autoconnect = "FALSE"')

    # And the console's own mouse and keyboard presented as USB devices on it,
    # rather than over PS/2.
    #
    # This file used to say that VMware drives them over PS/2 whatever is set
    # here, and that the HID path could therefore only be tested on real
    # hardware.  That was an assumption written down once and then trusted:
    # with these set, the console's input arrives as interrupt transfers on
    # the xHCI controller, which is the exact path that fails on a machine
    # with no PS/2 port.  Being unable to reproduce a fault is a good reason
    # to check the claim that it cannot be reproduced.
    $lines.Add('mouse.vusb.enable = "TRUE"')
    $lines.Add('mouse.vusb.useBasicMouse = "FALSE"')
    $lines.Add('keyboard.vusb.enable = "TRUE"')

    # An Intel gigabit card behind NAT: the guest gets an address from VMware's
    # DHCP server and can reach the host and the network beyond it.
    $lines.Add('ethernet0.present = "TRUE"')
    $lines.Add('ethernet0.virtualDev = "e1000"')
    $lines.Add('ethernet0.connectionType = "nat"')
    $lines.Add('ethernet0.startConnected = "TRUE"')
    $lines.Add('ethernet0.addressType = "generated"')
    # Enough video memory and headroom for a 4K mode, so the mode list the
    # firmware offers is worth something to test against.  The guest picks its
    # own mode out of that list.
    $lines.Add('svga.autodetect = "FALSE"')
    $lines.Add('svga.vramSize = "268435456"')
    $lines.Add("svga.maxWidth = `"$Width`"")
    $lines.Add("svga.maxHeight = `"$Height`"")
    # Ask the adapter for more than one independent display when requested, to
    # exercise multi-display (#7).  numDisplays is the count the SVGA device
    # exposes to the guest; the guest still has to define a screen per display.
    if ($Displays -gt 1) {
        $lines.Add("svga.numDisplays = `"$Displays`"")
    }

    # Serial to a file: everything klog emits lands here, even after a panic.
    $lines.Add('serial0.present = "TRUE"')
    $lines.Add('serial0.fileType = "file"')
    $lines.Add("serial0.fileName = `"serial.log`"")
    $lines.Add('serial0.tryNoRxLoss = "FALSE"')
    $lines.Add('serial0.yieldOnMsrRead = "TRUE"')

    # VNC, so a test can send keystrokes without VMware Tools in the guest.
    $lines.Add('RemoteDisplay.vnc.enabled = "TRUE"')
    $lines.Add("RemoteDisplay.vnc.port = `"$VncPort`"")
    $lines.Add('RemoteDisplay.vnc.key = ""')


    # The system disk, on whichever controller the caller asked for.
    if ($Controller -eq "nvme") {
        $lines.Add('nvme0.present = "TRUE"')
        $lines.Add('nvme0:0.present = "TRUE"')
        $lines.Add('nvme0:0.fileName = "kestrel.vmdk"')
        $lines.Add('nvme0:0.deviceType = "disk"')
        $lines.Add('nvme0:1.present = "TRUE"')
        $lines.Add('nvme0:1.fileName = "target.vmdk"')
        $lines.Add('nvme0:1.deviceType = "disk"')

        # A third disk carrying an NTFS volume that Windows formatted, so the
        # NTFS driver is exercised against a filesystem nothing here created.
        #
        # Built here rather than left in the folder, because creating the
        # machine clears the folder first - so anything put there beforehand is
        # gone by the time this runs.
        $vhd = Join-Path (Join-Path $root "build") "ntfs-test.vhd"
        if (Test-Path $vhd) {
            $raw = Join-Path $vmDir "ntfs.img"

            # A fixed VHD is the raw volume followed by a 512-byte footer, so
            # dropping the footer leaves an image VMware can use directly.
            $len = (Get-Item $vhd).Length - 512
            $in = [System.IO.File]::OpenRead($vhd)
            $out = [System.IO.File]::Create($raw)
            $buf = New-Object byte[] (1MB)
            $left = $len
            while ($left -gt 0) {
                $n = $in.Read($buf, 0, [Math]::Min($buf.Length, $left))
                if ($n -le 0) { break }
                $out.Write($buf, 0, $n)
                $left -= $n
            }
            $out.Close(); $in.Close()

            $sectors = [int]((Get-Item $raw).Length / 512)
            $cyl = [int]($sectors / (16 * 63))
            @"
# Disk DescriptorFile
version=1
CID=fffffffe
parentCID=ffffffff
createType="monolithicFlat"

RW $sectors FLAT "ntfs.img" 0

ddb.adapterType = "lsilogic"
ddb.geometry.cylinders = "$cyl"
ddb.geometry.heads = "16"
ddb.geometry.sectors = "63"
ddb.virtualHWVersion = "14"
"@ | Set-Content -Path (Join-Path $vmDir "ntfs.vmdk") -Encoding ASCII

            $lines.Add('nvme0:2.present = "TRUE"')
            $lines.Add('nvme0:2.fileName = "ntfs.vmdk"')
            $lines.Add('nvme0:2.deviceType = "disk"')
        }
    } else {
        $lines.Add('sata0.present = "TRUE"')
        $lines.Add('sata0:0.present = "TRUE"')
        $lines.Add('sata0:0.fileName = "kestrel.vmdk"')
        $lines.Add('sata0:0.deviceType = "disk"')
        $lines.Add('sata0:1.present = "TRUE"')
        $lines.Add('sata0:1.fileName = "target.vmdk"')
        $lines.Add('sata0:1.deviceType = "disk"')
    }

    # A small disk on the legacy IDE controller, so the ATA driver has real
    # hardware to run against.
    #
    # VMware presents an Intel PIIX4 IDE controller on every machine whether
    # or not anything is attached to it, and the driver-coverage report named
    # it as a device nothing drove.  Attaching a disk here is what turns that
    # driver from written into tested: IDENTIFY, the byte-swapped strings, the
    # per-sector DRQ handshake and a read/write round trip all run for real.
    $ideRaw = Join-Path $vmDir "ide.img"
    $ideSectors = 32768                                    # 16 MiB
    $fs = [System.IO.File]::Create($ideRaw)
    $fs.SetLength($ideSectors * 512)

    # A known pattern at known sectors, so that reading it back proves the
    # driver ADDRESSED the disk rather than merely returned something.  A
    # zero-filled image cannot tell a correct read from a read that did
    # nothing at all, and that is the failure worth catching.
    #
    # Each marked sector begins with its own number, little-endian.  Sector
    # 32767 is the last one, so it also proves the capacity is right.
    foreach ($lba in @(0, 1, 1000, 32767)) {
        $fs.Seek([int64]$lba * 512, 'Begin') | Out-Null
        $fs.Write([System.BitConverter]::GetBytes([uint32]$lba), 0, 4)
        $fs.Write([System.Text.Encoding]::ASCII.GetBytes("KESTREL-ATA"), 0, 11)
    }
    $fs.Close()
    $ideCyl = [int]($ideSectors / (16 * 63))
    @"
# Disk DescriptorFile
version=1
CID=fffffffd
parentCID=ffffffff
createType="monolithicFlat"

RW $ideSectors FLAT "ide.img" 0

ddb.adapterType = "ide"
ddb.geometry.cylinders = "$ideCyl"
ddb.geometry.heads = "16"
ddb.geometry.sectors = "63"
ddb.virtualHWVersion = "14"
"@ | Set-Content -Path (Join-Path $vmDir "ide.vmdk") -Encoding ASCII

    $lines.Add('ide0:0.present = "TRUE"')
    $lines.Add('ide0:0.fileName = "ide.vmdk"')
    $lines.Add('ide0:0.deviceType = "disk"')

    # The installer image, always on SATA as a CD.
    $lines.Add('sata1.present = "TRUE"')
    $lines.Add('sata1:0.present = "TRUE"')
    $lines.Add("sata1:0.fileName = `"$iso`"")
    $lines.Add('sata1:0.deviceType = "cdrom-image"')

    if ($Boot -eq "iso") {
        $lines.Add('bios.forceSetupOnce = "FALSE"')
        $lines.Add('bootOrder = "cdrom,hdd"')
    } else {
        $lines.Add('bootOrder = "hdd,cdrom"')
    }

    [System.IO.File]::WriteAllLines($vmx, $lines, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "created $vmx (boot=$Boot controller=$Controller vnc=$VncPort)"
}

function Start-Vm {
    if (-not (Test-Path $vmx)) { throw "no VM at $vmx; run 'create' first" }
    if (Test-Path $serial) { Remove-Item -Force $serial }
    $mode = if ($Gui) { "gui" } else { "nogui" }
    & $vmrun -T ws start $vmx $mode
    Write-Host "started ($mode)"
}

function Stop-Vm {
    if (-not (Test-Path $vmx)) { return }
    & $vmrun -T ws stop $vmx hard 2>$null
    Write-Host "stopped"
}

function Get-Shot {
    if (-not $Out) { $Out = Join-Path $vmDir ("shot-" + (Get-Random) + ".png") }
    & $vmrun -T ws captureScreen $vmx $Out
    Write-Host $Out
}

function Show-Serial {
    if (-not (Test-Path $serial)) { Write-Host "(no serial output yet)"; return }
    Get-Content -Raw -Encoding UTF8 $serial
}

function Show-Status {
    $running = & $vmrun -T ws list
    if ($running -match [regex]::Escape($vmx)) { Write-Host "running" } else { Write-Host "stopped" }
}

switch ($Action.ToLower()) {
    "create" { New-Vm }
    "start"  { Start-Vm }
    "stop"   { Stop-Vm }
    "shot"   { Get-Shot }
    "serial" { Show-Serial }
    "status" { Show-Status }
    "reset"  { Stop-Vm; Start-Sleep -Milliseconds 500; Start-Vm }
    default  { Write-Host "unknown action '$Action'" }
}
