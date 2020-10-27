/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include <tilck/common/basic_defs.h>
#include <tilck/kernel/list.h>

struct pci_vendor {
   u16 vendor_id;
   const char *name;
};

struct pci_device_class {
   u8 class_id;
   u8 subclass_id;
   u8 progif_id;
   const char *class_name;
   const char *subclass_name;
   const char *progif_name;
};

struct pci_device_loc {

   union {

      struct {
         u16 seg;       /* PCI Segment Group Number */
         u8 bus;        /* PCI Bus */
         u8 dev  : 5;   /* PCI Device Number */
         u8 func : 3;   /* PCI Function Number */
      };

      u32 raw;
   };
};

struct pci_device_basic_info {

   union {

      struct {
         u16 vendor_id;
         u16 device_id;
      };

      u32 __dev_vendr;
   };

   union {

      struct {
         u8 rev_id;
         u8 progif_id;
         u8 subclass_id;
         u8 class_id;
      };

      u32 __class_info;
   };

   union {

      struct {
         u8 header_type : 7;
         u8 multi_func  : 1;
      };

      u8 __header_type;
   };
};

/*
 * PCI leaf node: a PCI device function.
 *
 * It's called just `device` and not `device_function` because it represents
 * a logical device, not a physical one. Also, `pci_device` is just shorter
 * and feels more natural than `pci_device_function` or `pci_device_node`.
 */
struct pci_device {

   struct list_node node;
   struct pci_device_loc loc;
   struct pci_device_basic_info nfo;
   void *ext_config;
};

static ALWAYS_INLINE struct pci_device_loc
pci_make_loc(u16 seg, u8 bus, u8 dev, u8 func)
{
   return (struct pci_device_loc) {
      .seg = seg,
      .bus = bus,
      .dev = dev,
      .func = func
   };
}

const char *
pci_find_vendor_name(u16 id);

void
pci_find_device_class_name(struct pci_device_class *dev_class);

static inline int
pci_config_read(struct pci_device_loc loc, u32 off, u32 width, u32 *val)
{
   extern int (*__pci_config_read_func)(struct pci_device_loc, u32, u32, u32 *);
   return __pci_config_read_func(loc, off, width, val);
}

static inline int
pci_config_write(struct pci_device_loc loc, u32 off, u32 width, u32 val)
{
   extern int (*__pci_config_write_func)(struct pci_device_loc, u32, u32, u32);
   return __pci_config_write_func(loc, off, width, val);
}

struct pci_device *
pci_get_object(struct pci_device_loc loc);
