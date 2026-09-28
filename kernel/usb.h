/* usb.h - what the host controller and the class drivers agree on.
 *
 * This is deliberately small.  Only one class driver exists (HID boot-protocol
 * keyboards and mice), so the host controller calls it directly rather than
 * going through a registration table that would have exactly one entry.
 */
#ifndef KESTREL_USB_H
#define KESTREL_USB_H

#include "kernel.h"

/* ------------------------------------------------------------- descriptors */

#define USB_DT_DEVICE     1
#define USB_DT_CONFIG     2
#define USB_DT_STRING     3
#define USB_DT_INTERFACE  4
#define USB_DT_ENDPOINT   5
/* A SuperSpeed endpoint is described by two descriptors, not one: the ordinary
 * endpoint descriptor and a companion that follows it immediately.  The
 * companion carries how many packets the device can send back to back - the
 * burst - which on USB 3 is the difference between a device that moves data at
 * its rated speed and one that moves a single packet per service interval.
 *
 * A driver that ignores it still works, slowly, on most devices, and fails
 * outright on the ones that require a burst to be negotiated.  Either way the
 * controller is being told something untrue about the hardware. */
#define USB_DT_SS_EP_COMP 0x30

#define USB_DT_HID        0x21
#define USB_DT_REPORT     0x22

typedef struct {
    u8  length, type;
    u16 usb_version;
    u8  dev_class, dev_subclass, dev_protocol;
    u8  max_packet0;
    u16 vendor, product;
    u16 dev_version;
    u8  manufacturer_str, product_str, serial_str;
    u8  num_configs;
} __attribute__((packed)) usb_device_desc_t;

typedef struct {
    u8  length, type;
    u16 total_length;
    u8  num_interfaces, config_value, config_str;
    u8  attributes, max_power;
} __attribute__((packed)) usb_config_desc_t;

typedef struct {
    u8 length, type;
    u8 interface_num, alternate, num_endpoints;
    u8 dev_class, subclass, protocol;
    u8 interface_str;
} __attribute__((packed)) usb_interface_desc_t;

typedef struct {
    u8  length, type;
    u8  address, attributes;
    u16 max_packet;
    u8  interval;
} __attribute__((packed)) usb_endpoint_desc_t;

/* Request types for the setup packet. */
#define USB_DIR_OUT       0x00
#define USB_DIR_IN        0x80
#define USB_TYPE_STANDARD 0x00
#define USB_TYPE_CLASS    0x20
#define USB_RECIP_DEVICE    0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT  0x02
/* "Other" is how a request aimed at one port of a hub is addressed: the
 * recipient is neither the hub as a device nor one of its interfaces, but a
 * numbered thing belonging to it, named in the index field. */
#define USB_RECIP_OTHER     0x03

#define USB_REQ_GET_STATUS       0
#define USB_REQ_CLEAR_FEATURE    1
#define USB_REQ_SET_FEATURE      3
#define USB_REQ_GET_DESCRIPTOR   6
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_SET_INTERFACE    11

/* HID class. */
#define USB_CLASS_HID       3
#define HID_SUBCLASS_BOOT   1
#define HID_PROTO_KEYBOARD  1
#define HID_PROTO_MOUSE     2
#define HID_REQ_SET_IDLE     0x0A
#define HID_REQ_SET_PROTOCOL 0x0B
#define HID_REQ_SET_REPORT   0x09

/* Endpoint attribute bits. */
#define USB_EP_XFER_MASK   0x03
#define USB_EP_XFER_ISOC    0x01
#define USB_EP_XFER_BULK   0x02
#define USB_EP_XFER_INT    0x03

/* The classes this system has drivers for. */
#define USB_CLASS_STORAGE  0x08

/* ------------------------------------------------------------------ device */

typedef struct usb_device usb_device_t;


/* One interface, reduced to what a class driver actually needs.
 *
 * There are two shapes of interface here and they are used quite differently.
 * A keyboard or a mouse has one interrupt endpoint that the controller polls
 * forever, and reports arrive whether or not anybody asked - so that endpoint
 * is set up once and left running.  A disk has a pair of bulk endpoints and
 * nothing arrives unless it was requested: a command goes out, data comes back
 * or goes out, and a status follows.  Keeping both in the same structure means
 * one place decides what an interface has, rather than each driver parsing the
 * descriptors again for itself.
 */
typedef struct {
    u8  number;
    u8  dev_class, subclass, protocol;

    u8  ep_addr;            /* interrupt IN endpoint, 0 when there is none */
    u16 ep_max_packet;
    u8  ep_interval;
    u16 report_desc_len;    /* from the HID descriptor, 0 when there is none */

    /* Bulk endpoints, for anything that moves data on request. */
    u8  bulk_in, bulk_out;                  /* 0 when there is none        */
    u16 bulk_in_max, bulk_out_max;

    /* How many packets each may send back to back, from the SuperSpeed
     * companion descriptor.  Zero means one packet, which is also what a
     * device slower than USB 3 means. */
    u8  bulk_in_burst, bulk_out_burst;
    u8  int_burst;
} usb_interface_t;
/* Set up the bulk pair on an interface, so the device can be talked to.
 * Returns false if either endpoint could not be configured. */
bool usb_bulk_open(usb_device_t *dev, const usb_interface_t *ifc);

/* Move data over one of them and wait for it to finish.  `in` says which
 * direction; the return is how many bytes moved, or negative on failure.
 *
 * This is deliberately synchronous.  Storage is request-response - a command,
 * then data, then a status, each depending on the last - so there is nothing
 * to overlap, and a driver that had to track completions would be more code
 * for no gain. */
int usb_bulk(usb_device_t *dev, bool in, void *data, u32 length, u32 timeout_ms);

/* Storage.  Claims an interface and registers it as a disk. */
bool usbmsc_probe(usb_device_t *dev, const usb_interface_t *ifc);

/* btusb.c - a Bluetooth adapter, whose transport over USB is the same
 * whatever radio is behind it. */
void *btusb_probe(usb_device_t *dev, const usb_interface_t *ifc);
void  btusb_start(void *ctx);
void  btusb_event(void *ctx, const u8 *data, int len);
void  btusb_detach(void *ctx);

/* usbaudio.c - a USB audio device, described from its own descriptors.  It is
 * given the whole configuration because what matters for audio is the
 * alternate settings, which the interface binder skips. */
void *usbaudio_probe(usb_device_t *dev, const usb_interface_t *ifc,
                     const u8 *cfg, int cfg_len);
bool  usbaudio_next_endpoint(void *ctx, u8 *addr, u16 *max_packet,
                             u8 *interval, u8 *burst,
                             u8 *interface, u8 *alternate);
void  usbaudio_endpoint_open(void *ctx, u8 addr, bool ok);
int   usbaudio_ring_selftest(void);
int   usbaudio_write(const void *samples, int bytes);
int   usbaudio_space(void);
u32   usbaudio_next_samples(void *ctx, u8 addr, void *buf, u32 max);
bool  usbaudio_can_play(void);
bool  usbaudio_playback_format(u32 *rate, u32 *channels, u32 *bits);
/* Bluetooth discovery: start listening, stop, and read what was heard. */
int   btusb_selftest(void);
bool  btusb_scan(void *ctx, int seconds);
void  btusb_scan_stop(void *ctx);
bool  btusb_connect(void *ctx, const u8 *addr);
void  btusb_disconnect(void *ctx);
bool  btusb_link(u8 *addr, u16 *handle, u16 *interval_us);
int   btusb_read_attribute(void *ctx, u16 attr_handle, void *out, u32 cap);

int   btusb_found_count(void);
bool  btusb_found_get(int index, u8 *addr, char *name, size_t name_cap,
                      int *rssi, u32 *device_class, bool *low_energy);
const char *btusb_kind_name(u32 device_class);

int   usbaudio_count(void);
bool  usbaudio_get(int index, char *name, size_t name_cap,
                   u32 *out_channels, u32 *out_bits,
                   u32 *in_channels, u32 *in_bits);
void *btusb_adapter(int index);
int   btusb_count(void);
bool  btusb_get(int index, char *maker, size_t maker_cap, u8 *addr, u8 *version_out);

/* Totals across every controller, for the status panel. */
void usb_event_counts(u32 *events, u32 *transfers, u8 *last_code,
                      u8 *worst_ep_state);

/* Synchronous control transfer on endpoint 0.  Returns the number of bytes
 * transferred, or a negative error.  Only safe from the USB thread. */
int usb_control(usb_device_t *dev, u8 request_type, u8 request,
                u16 value, u16 index, void *data, u16 length);

/* Drain pending events on this device's controller without scanning the bus.
 * For bounded class-command waits during enumeration; does not wait for I/O. */
void usb_poll_device_events(usb_device_t *dev);

const char *usb_speed_name(u8 speed);
const char *usb_device_name(const usb_device_t *dev);

/* ------------------------------------------------------------ class driver */

/* Called once per interface during enumeration.  Returning non-NULL claims the
 * interface; the pointer comes back on every report.  Returning NULL leaves the
 * interface alone and its endpoint unconfigured. */
void *usbhid_probe(usb_device_t *dev, const usb_interface_t *ifc);

/* A completed interrupt IN transfer. */
void  usbhid_report(void *ctx, const u8 *data, int len);

/* The interface is going away. */
void  usbhid_detach(void *ctx);

/* USB devices do not repeat a held key the way a PS/2 keyboard's hardware
 * does, so the driver has to synthesise it.  The host controller thread calls
 * this every time round its loop. */
void  usbhid_tick(void);

/* Bookkeeping so the rest of the kernel can tell whether USB found input. */
void  usb_count_hid(int keyboard_delta, int mouse_delta);

/* ------------------------------------------------------------------- entry */

void usb_init(void);
/* The USB 1.1 controller, driven separately - see the note at the top of
 * uhci.c about why it does not share the device model. */
void uhci_init(void);
int  usb_device_count(void);
bool usb_keyboard_present(void);
bool usb_mouse_present(void);

/* For the `usb` tool and the System app: walk what is attached. */
typedef struct {
    u8   port;
    u8   slot;
    u8   speed;
    u16  vendor, product;
    char name[48];
    char driver[24];
} usb_devinfo_t;

int usb_snapshot(usb_devinfo_t *out, int max);

#endif
