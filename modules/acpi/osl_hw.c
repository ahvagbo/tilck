/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/printk.h>

#include <tilck/kernel/hal.h>
#include <tilck/kernel/irq.h>
#include <tilck/kernel/kmalloc.h>

#include <3rd_party/acpi/acpi.h>
#include <3rd_party/acpi/acpiosxf.h>

STATIC_ASSERT(IRQ_HANDLED == ACPI_INTERRUPT_HANDLED);
STATIC_ASSERT(IRQ_NOT_HANDLED == ACPI_INTERRUPT_NOT_HANDLED);

static struct irq_handler_node *osl_irq_handlers;

ACPI_STATUS
osl_init_irqs(void)
{
   osl_irq_handlers = kzalloc_array_obj(struct irq_handler_node, 16);

   if (!osl_irq_handlers)
      panic("ACPI: unable to allocate memory for IRQ handlers");

   return AE_OK;
}

ACPI_STATUS
AcpiOsInstallInterruptHandler(
    UINT32                  InterruptNumber,
    ACPI_OSD_HANDLER        ServiceRoutine,
    void                    *Context)
{
   struct irq_handler_node *n;

   if (!ServiceRoutine)
      return AE_BAD_PARAMETER;

   if (!IN_RANGE((int)InterruptNumber, 0, 16))
      return AE_BAD_PARAMETER;

   n = &osl_irq_handlers[InterruptNumber];

   if (n->handler)
      return AE_ALREADY_EXISTS;

   list_node_init(&n->node);
   n->handler = (irq_handler_t)ServiceRoutine;
   n->context = Context;

   printk("ACPI: install handler for IRQ #%d\n", InterruptNumber);
   irq_install_handler(InterruptNumber, n);
   return AE_OK;
}

ACPI_STATUS
AcpiOsRemoveInterruptHandler(
    UINT32                  InterruptNumber,
    ACPI_OSD_HANDLER        ServiceRoutine)
{
   struct irq_handler_node *n;

   if (!ServiceRoutine)
      return AE_BAD_PARAMETER;

   if (!IN_RANGE((int)InterruptNumber, 0, 16))
      return AE_BAD_PARAMETER;

   n = &osl_irq_handlers[InterruptNumber];

   if (!n->handler)
      return AE_NOT_EXIST;

   if (n->handler != ServiceRoutine)
      return AE_BAD_PARAMETER;

   printk("ACPI: remove handler for IRQ #%d\n", InterruptNumber);
   irq_uninstall_handler(InterruptNumber, n);
   return AE_OK;
}
