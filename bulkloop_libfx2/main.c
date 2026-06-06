// Bulk loopback example: receives data on EP2 OUT and sends it back on EP6 IN.
// Uses quad-buffering (4x) on both endpoints to keep the USB pipeline full
// while the 8051 copies between FIFOs via autopointers.

#include <fx2lib.h>
#include <fx2delay.h>
#include <fx2usb.h>

// Device descriptor
usb_desc_device_c usb_device = {
  .bLength              = sizeof(struct usb_desc_device),
  .bDescriptorType      = USB_DESC_DEVICE,
  .bcdUSB               = 0x0200,
  .bDeviceClass         = USB_DEV_CLASS_VENDOR,
  .bDeviceSubClass      = 0,
  .bDeviceProtocol      = 0,
  .bMaxPacketSize0      = 64,
  .idVendor             = 0x04b4,
  .idProduct            = 0x1004,
  .bcdDevice            = 0x0000,
  .iManufacturer        = 1,
  .iProduct             = 2,
  .iSerialNumber        = 0,
  .bNumConfigurations   = 1,
};

// Interface 0: Bulk loopback
usb_desc_interface_c usb_iface_0 = {
  .bLength              = sizeof(struct usb_desc_interface),
  .bDescriptorType      = USB_DESC_INTERFACE,
  .bInterfaceNumber     = 0,
  .bAlternateSetting    = 0,
  .bNumEndpoints        = 2,
  .bInterfaceClass      = USB_IFACE_CLASS_VENDOR,
  .bInterfaceSubClass   = 0,
  .bInterfaceProtocol   = 0,
  .iInterface           = 0,
};

// EP2 OUT: Bulk endpoint
usb_desc_endpoint_c usb_endpoint_ep2_out = {
  .bLength              = sizeof(struct usb_desc_endpoint),
  .bDescriptorType      = USB_DESC_ENDPOINT,
  .bEndpointAddress     = 2,
  .bmAttributes         = USB_XFER_BULK,
  .wMaxPacketSize       = 512,
  .bInterval            = 0,
};

// EP6 IN: Bulk endpoint
usb_desc_endpoint_c usb_endpoint_ep6_in = {
  .bLength              = sizeof(struct usb_desc_endpoint),
  .bDescriptorType      = USB_DESC_ENDPOINT,
  .bEndpointAddress     = 6|USB_DIR_IN,
  .bmAttributes         = USB_XFER_BULK,
  .wMaxPacketSize       = 512,
  .bInterval            = 0,
};

// Configuration descriptor
usb_configuration_c usb_config = {
  {
    .bLength              = sizeof(struct usb_desc_configuration),
    .bDescriptorType      = USB_DESC_CONFIGURATION,
    .bNumInterfaces       = 1,
    .bConfigurationValue  = 1,
    .iConfiguration       = 0,
    .bmAttributes         = USB_ATTR_RESERVED_1,
    .bMaxPower            = 50,
  },
  {
    { .interface = &usb_iface_0 },
    { .endpoint  = &usb_endpoint_ep2_out },
    { .endpoint  = &usb_endpoint_ep6_in },
    { 0 }
  }
};

usb_configuration_set_c usb_configs[] = {
  &usb_config,
};

usb_ascii_string_c usb_strings[] = {
  [0] = "FX2 Bulk Loopback",           // iManufacturer = 1
  [1] = "FX2 Bulk Loopback Example",   // iProduct = 2
};

usb_descriptor_set_c usb_descriptor_set = {
  .device           = &usb_device,
  .config_count     = ARRAYSIZE(usb_configs),
  .configs          = usb_configs,
  .string_count     = ARRAYSIZE(usb_strings),
  .strings          = usb_strings,
};

void handle_usb_setup(__xdata struct usb_req_setup *req) {
  (void) req;
  // Standard requests are handled by the libfx2 framework before this callback.
  // Stall all remaining (vendor/class) requests we don't handle.
  STALL_EP0();
}

int main(void) {
  // Run core at 48 MHz fCLK.
  CPUCS = _CLKSPD1;

  // Use newest chip features.
  REVCTL = _ENH_PKT|_DYN_OUT;

  // NAK all transfers.
  SYNCDELAY;
  FIFORESET = _NAKALL;
  SYNCDELAY;

  // EP2: 512-byte QUAD-buffered (4x) BULK OUT.
  // EP6: 512-byte QUAD-buffered (4x) BULK IN.
  // 4x buffering: neither _BUF0 nor _BUF1 set (bits[1:0] = 00).
  // With 4 buffers the host can keep 4 packets in flight on each endpoint,
  // hiding the latency of the 8051 copy loop.
  EP2CFG = _VALID|_TYPE1;           // DIR=OUT, TYPE=BULK, SIZE=512, BUF=4x
  SYNCDELAY;
  EP6CFG = _VALID|_DIR|_TYPE1;      // DIR=IN,  TYPE=BULK, SIZE=512, BUF=4x
  SYNCDELAY;
  EP2CS  = 0;
  EP6CS  = 0;

  // Disable unused endpoints.
  EP1INCFG  &= ~_VALID; SYNCDELAY;
  EP1OUTCFG &= ~_VALID; SYNCDELAY;
  EP4CFG    &= ~_VALID; SYNCDELAY;
  EP8CFG    &= ~_VALID; SYNCDELAY;

  // Reset EP2 and EP6 FIFOs and prime all four EP2 OUT slots.
  FIFORESET = _NAKALL|2; SYNCDELAY;
  OUTPKTEND = _SKIP|2;   SYNCDELAY;  // prime slot 1
  OUTPKTEND = _SKIP|2;   SYNCDELAY;  // prime slot 2
  OUTPKTEND = _SKIP|2;   SYNCDELAY;  // prime slot 3
  OUTPKTEND = _SKIP|2;   SYNCDELAY;  // prime slot 4
  FIFORESET = _NAKALL|6; SYNCDELAY;
  FIFORESET = 0;         SYNCDELAY;

  // Re-enumerate, to make sure our descriptors are picked up correctly.
  // usb_init() enables global interrupts (EA = 1) internally.
  usb_init(/*disconnect=*/true);

  // Main loopback loop.
  // Use the FX2 autopointers for the FIFO copy: after setup, the loop body
  // is just two movx instructions per byte with no address recalculation.
  while(1) {
    if(!(EP2468STAT & _EP2E) && !(EP2468STAT & _EP6F)) {
      uint16_t bytes = (EP2BCH << 8) | EP2BCL;

      AUTOPTRSETUP = _APTREN|_APTR1INC|_APTR2INC;
      AUTOPTRH1 = (uint16_t)EP2FIFOBUF >> 8;
      AUTOPTRL1 = (uint16_t)EP2FIFOBUF & 0xff;
      AUTOPTRH2 = (uint16_t)EP6FIFOBUF >> 8;
      AUTOPTRL2 = (uint16_t)EP6FIFOBUF & 0xff;
      {
        uint16_t i;
        for(i = 0; i < bytes; i++)
          XAUTODAT2 = XAUTODAT1;
      }

      // Commit EP6 IN packet.
      EP6BCH = bytes >> 8;
      SYNCDELAY;
      EP6BCL = bytes & 0xff;
      SYNCDELAY;

      // Re-arm EP2 OUT for the next packet.
      EP2BCL = 0;
    }
  }
}
