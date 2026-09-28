/* usbdesc.c - read a USB device's descriptors from Windows, and run the
 * kernel's own interface walk over them.
 *
 * The boot stick is a textbook mass storage device and this system's driver
 * never claims it.  Everything checkable from the outside says it should: the
 * class, subclass and protocol are the three numbers the driver looks for.  So
 * the fault is in what the driver makes of the device's own description of
 * itself, and the way to find that is to get that description from a machine
 * that reads it correctly and put it through the same walk.
 *
 * Windows will hand over the raw configuration descriptor to any program that
 * opens the hub the device is plugged into.  No driver, no privilege beyond
 * the usual, and the bytes are the device's own.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbiodef.h>
#include <usbioctl.h>
#include <usbspec.h>

#pragma comment(lib, "setupapi.lib")

/* ---------------------------------------------------- the kernel's own walk
 *
 * These are the same shapes and the same loop as bind_interfaces() in xhci.c,
 * kept deliberately close so that what happens here is what happens there.
 */
#define USB_DT_INTERFACE   0x04
#define USB_DT_ENDPOINT    0x05
#define USB_DT_SS_EP_COMP  0x30
#define USB_EP_XFER_MASK   0x03
#define USB_EP_XFER_BULK   0x02
#define USB_EP_XFER_INT    0x03
#define USB_DIR_IN         0x80

typedef struct {
    uint8_t number, dev_class, subclass, protocol;
    uint8_t ep_addr, bulk_in, bulk_out;
    uint16_t ep_max_packet, bulk_in_max, bulk_out_max;
    int alternate;
} iface_t;

/* The same decision offer() makes in xhci.c, and for the same reasons.
 *
 * What this reports is what the system would actually do with the device in
 * front of it, decided by that device's own bytes rather than by anybody's
 * recollection of what the driver checks.
 */
static int report(const iface_t *i) {
    const char *v;
    int claimed = 0;

    if (i->dev_class == 0x03 && i->ep_addr) {
        v = "input device - claimed"; claimed = 1;
    } else if (i->dev_class == 0x08 && i->bulk_in && i->bulk_out) {
        v = "storage - claimed"; claimed = 1;
    } else if (i->dev_class == 0x08) {
        v = "storage, but its bulk endpoints were not found";
    } else if (i->dev_class == 0xE0 && i->subclass == 0x01 &&
               i->protocol == 0x01 && i->ep_addr) {
        v = "Bluetooth - claimed"; claimed = 1;
    } else if (i->dev_class == 0xE0) {
        v = "Bluetooth, without the endpoint its events arrive on";
    } else if (i->dev_class == 0x09) {
        v = "a hub - driven through endpoint zero, not through this";
    } else if (i->dev_class == 0x01 && i->subclass == 0x01) {
        v = "audio control - claimed, and its streaming settings read"; claimed = 1;
    } else if (i->dev_class == 0x01) {
        v = "audio streaming - read through the control interface";
    } else if (i->dev_class == 0x0E) {
        v = "video - NO DRIVER";
    } else if (i->dev_class == 0xFF) {
        v = "vendor-specific - NO DRIVER";
    } else {
        v = "NO DRIVER";
    }

    printf("  interface %u alt %d: class %02x/%02x/%02x  int %02x  "
           "bulk %02x/%02x  -> %s\n",
           i->number, i->alternate, i->dev_class, i->subclass, i->protocol,
           i->ep_addr, i->bulk_in, i->bulk_out, v);
    return claimed;
}

static void walk_config(const uint8_t *cfg, int total) {
    iface_t ifc;
    memset(&ifc, 0, sizeof ifc);
    int have = 0, claimed = 0, seen = 0;

    printf("\nthe kernel's interface walk over these bytes:\n");

    for (int off = 0; off + 2 <= total; ) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len < 2 || off + len > total) break;

        if (type == USB_DT_INTERFACE && len >= 9) {
            if (have) {
                seen++;
                if (report(&ifc)) claimed++;
            }
            memset(&ifc, 0, sizeof ifc);
            ifc.number = cfg[off + 2];
            ifc.alternate = cfg[off + 3];
            ifc.dev_class = cfg[off + 5];
            ifc.subclass = cfg[off + 6];
            ifc.protocol = cfg[off + 7];
            /* The kernel only takes alternate setting zero. */
            have = (cfg[off + 3] == 0);
        } else if (type == USB_DT_ENDPOINT && have && len >= 7) {
            uint8_t addr = cfg[off + 2];
            uint8_t attrs = cfg[off + 3];
            uint16_t maxp = (uint16_t)(cfg[off + 4] | (cfg[off + 5] << 8));
            uint8_t kind = attrs & USB_EP_XFER_MASK;
            int in = (addr & USB_DIR_IN) != 0;

            if (kind == USB_EP_XFER_INT && in && !ifc.ep_addr) {
                ifc.ep_addr = addr;
                ifc.ep_max_packet = maxp & 0x7FF;
            } else if (kind == USB_EP_XFER_BULK && in && !ifc.bulk_in) {
                ifc.bulk_in = addr;
                ifc.bulk_in_max = maxp & 0x7FF;
            } else if (kind == USB_EP_XFER_BULK && !in && !ifc.bulk_out) {
                ifc.bulk_out = addr;
                ifc.bulk_out_max = maxp & 0x7FF;
            }
        }
        off += len;
    }

    if (have) {
        seen++;
        if (report(&ifc)) claimed++;
    }

    printf("  %d interface(s) offered, %d claimed\n", seen, claimed);
}

static void hexdump(const uint8_t *p, int n) {
    printf("\nthe configuration descriptor, %d bytes:\n", n);
    for (int i = 0; i < n; i += 16) {
        printf("  %04x  ", i);
        for (int j = 0; j < 16 && i + j < n; j++) printf("%02x ", p[i + j]);
        printf("\n");
    }
}

/* Walk the descriptors as a list of typed blocks, which is how they are meant
 * to be read and how a mistake in the length of one is made obvious. */
static void describe(const uint8_t *p, int n) {
    printf("\nwhat those bytes say:\n");
    for (int off = 0; off + 2 <= n; ) {
        uint8_t len = p[off], type = p[off + 1];
        if (len < 2) { printf("  a descriptor of length %u, which cannot be\n", len); break; }

        const char *name = type == 0x02 ? "configuration" :
                           type == 0x04 ? "interface" :
                           type == 0x05 ? "endpoint" :
                           type == 0x0B ? "interface association" :
                           type == 0x21 ? "HID" :
                           type == 0x30 ? "SuperSpeed endpoint companion" : "?";
        printf("  +%03d  type %02x %-30s %u bytes", off, type, name, len);

        if (type == 0x04 && len >= 9)
            printf("   number %u alt %u, %u endpoint(s), class %02x/%02x/%02x",
                   p[off + 2], p[off + 3], p[off + 4], p[off + 5], p[off + 6], p[off + 7]);
        else if (type == 0x05 && len >= 7) {
            uint8_t a = p[off + 2];
            const char *k = (p[off + 3] & 3) == 2 ? "bulk" :
                            (p[off + 3] & 3) == 3 ? "interrupt" :
                            (p[off + 3] & 3) == 1 ? "isochronous" : "control";
            printf("   %02x %s %s, max packet %u", a, (a & 0x80) ? "IN " : "OUT", k,
                   (unsigned)(p[off + 4] | (p[off + 5] << 8)));
        } else if (type == 0x30 && len >= 6) {
            printf("   burst %u", p[off + 2]);
        }
        printf("\n");
        off += len;
    }
}

int main(int argc, char **argv) {
    unsigned want_vid = 0, want_pid = 0;
    int all = (argc < 3);
    if (!all) {
        sscanf(argv[1], "%x", &want_vid);
        sscanf(argv[2], "%x", &want_pid);
        printf("looking for %04x:%04x\n", want_vid, want_pid);
    } else {
        printf("every USB device, and what this system would make of it\n");
    }

    HDEVINFO set = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USB_HUB, NULL, NULL,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) { printf("no hubs\n"); return 1; }

    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof ifd;
    int found = 0;

    for (DWORD i = 0; (all || !found) &&
         SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_USB_HUB, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetail(set, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;

        static unsigned char detbuf[1024];
        if (need > sizeof detbuf) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA det = (PSP_DEVICE_INTERFACE_DETAIL_DATA)detbuf;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        if (!SetupDiGetDeviceInterfaceDetail(set, &ifd, det, need, NULL, NULL)) continue;

        HANDLE hub = CreateFile(det->DevicePath, GENERIC_WRITE, FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
        if (hub == INVALID_HANDLE_VALUE) continue;

        USB_NODE_INFORMATION info;
        DWORD got = 0;
        memset(&info, 0, sizeof info);
        info.NodeType = UsbHub;
        if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_INFORMATION, &info, sizeof info,
                             &info, sizeof info, &got, NULL)) { CloseHandle(hub); continue; }

        ULONG ports = info.u.HubInformation.HubDescriptor.bNumberOfPorts;
        for (ULONG port = 1; port <= ports; port++) {
            if (found && !all) break;
            static unsigned char conbuf[2048];
            PUSB_NODE_CONNECTION_INFORMATION_EX con =
                (PUSB_NODE_CONNECTION_INFORMATION_EX)conbuf;
            memset(conbuf, 0, sizeof conbuf);
            con->ConnectionIndex = port;

            if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                                 con, sizeof conbuf, con, sizeof conbuf, &got, NULL))
                continue;
            if (con->ConnectionStatus != DeviceConnected) continue;
            if (!all && (con->DeviceDescriptor.idVendor != want_vid ||
                         con->DeviceDescriptor.idProduct != want_pid)) continue;

            found = 1;
            printf("\n%04x:%04x  port %lu, speed %u",
                   con->DeviceDescriptor.idVendor,
                   con->DeviceDescriptor.idProduct, port, con->Speed);
            printf("\nfound on port %lu\n", port);
            printf("  USB %x.%02x, device class %02x/%02x/%02x, max packet %u\n",
                   con->DeviceDescriptor.bcdUSB >> 8,
                   con->DeviceDescriptor.bcdUSB & 0xFF,
                   con->DeviceDescriptor.bDeviceClass,
                   con->DeviceDescriptor.bDeviceSubClass,
                   con->DeviceDescriptor.bDeviceProtocol,
                   con->DeviceDescriptor.bMaxPacketSize0);
            printf("  speed %u (0 low, 1 full, 2 high, 3 super), %u configuration(s)\n",
                   con->Speed, con->DeviceDescriptor.bNumConfigurations);

            /* And the configuration descriptor, which is the thing in
             * question: the interfaces and their endpoints. */
            static unsigned char req[sizeof(USB_DESCRIPTOR_REQUEST) + 4096];
            PUSB_DESCRIPTOR_REQUEST dr = (PUSB_DESCRIPTOR_REQUEST)req;
            memset(req, 0, sizeof req);
            dr->ConnectionIndex = port;
            dr->SetupPacket.wValue = (USB_CONFIGURATION_DESCRIPTOR_TYPE << 8);
            dr->SetupPacket.wLength = 4096;

            if (DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
                                req, sizeof req, req, sizeof req, &got, NULL)) {
                const uint8_t *cfg = req + sizeof(USB_DESCRIPTOR_REQUEST);
                int total = cfg[2] | (cfg[3] << 8);
                if (total > (int)(got - sizeof(USB_DESCRIPTOR_REQUEST)))
                    total = (int)(got - sizeof(USB_DESCRIPTOR_REQUEST));

                if (!all) { hexdump(cfg, total); describe(cfg, total); }
                walk_config(cfg, total);
            } else {
                printf("  the configuration descriptor could not be read\n");
            }
        }
        CloseHandle(hub);
    }

    SetupDiDestroyDeviceInfoList(set);
    if (!found) printf("\nnot found on any hub\n");
    return found ? 0 : 1;
}
