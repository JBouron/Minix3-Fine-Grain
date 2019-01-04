/*
 *   The Minix hardware interrupt system.
 *   
 *   This file contains routines for managing the interrupt
 *   controller.
 *  
 *   put_irq_handler: register an interrupt handler.
 *   rm_irq_handler:  deregister an interrupt handler.
 *   irq_handle:     handle a hardware interrupt.
 *                    called by the system dependent part when an
 *                    external interrupt occurs.
 *   enable_irq:      enable hook for IRQ.
 *   disable_irq:     disable hook for IRQ.
 */

#include <assert.h>

#include "kernel/kernel.h"
#include "hw_intr.h"


SPINLOCK_DEFINE(irq_lock);
/* number of lists of IRQ hooks, one list per supported line. */
static irq_hook_t* irq_handlers[NR_IRQ_VECTORS] = {0};

/*===========================================================================*
 *				put_irq_handler				     *
 *===========================================================================*/
/* Register an interrupt handler.  */
void put_irq_handler_no_lock( irq_hook_t* hook, int irq,
  const irq_handler_t handler)
{
  int id;
  irq_hook_t **line;
  unsigned long bitmap;

  if( irq < 0 || irq >= NR_IRQ_VECTORS )
	panic("invalid call to put_irq_handler: %d",  irq);

  line = &irq_handlers[irq];

  bitmap = 0;
  while ( *line != NULL ) {
	if(hook == *line) return; /* extra initialization */
	bitmap |= (*line)->id;	/* mark ids in use */
	line = &(*line)->next;
  }

  /* find the lowest id not in use */
  for (id = 1; id != 0; id <<= 1)
  	if (!(bitmap & id)) break;

  if(id == 0)
  	panic("Too many handlers for irq: %d",  irq);
  
  hook->next = NULL;
  hook->handler = handler;
  hook->irq = irq;
  hook->id = id;
  *line = hook;

  /* And as last enable the irq at the hardware.
   *
   * Internal this activates the line or source of the given interrupt.
   */
  if((irq_actids[hook->irq] &= ~hook->id) == 0) {
	  hw_intr_used(irq);
	  hw_intr_unmask(hook->irq);
  }
}

void put_irq_handler( irq_hook_t* hook, int irq,
  const irq_handler_t handler)
{
	lock_irq();
	put_irq_handler_no_lock(hook,irq,handler);
	unlock_irq();
}

/*===========================================================================*
 *				rm_irq_handler				     *
 *===========================================================================*/
/* Unregister an interrupt handler.  */
void rm_irq_handler_no_lock( const irq_hook_t* hook ) {
  const int irq = hook->irq; 
  const int id = hook->id;
  irq_hook_t **line;

  if( irq < 0 || irq >= NR_IRQ_VECTORS ) 
	panic("invalid call to rm_irq_handler: %d",  irq);

  /* remove the hook  */
  line = &irq_handlers[irq];
  while( (*line) != NULL ) {
	if((*line)->id == id) {
		(*line) = (*line)->next;
		if (irq_actids[irq] & id)
			irq_actids[irq] &= ~id;
    	}
    	else {
		line = &(*line)->next;
    	}
  }

  /* Disable the irq if there are no other handlers registered.
   * If the irq is shared, reenable it if there is no active handler.
   */
  if (irq_handlers[irq] == NULL) {
	hw_intr_mask(irq);
	hw_intr_not_used(irq);
  }
  else if (irq_actids[irq] == 0) {
	hw_intr_unmask(irq);
  }
}

void rm_irq_handler( const irq_hook_t* hook )
{
	lock_irq();
	rm_irq_handler_no_lock(hook);
	unlock_irq();
}

/*===========================================================================*
 *				irq_handle				     *
 *===========================================================================*/
/*
 * The function first disables interrupt is need be and restores the state at
 * the end. Before returning, it unmasks the IRQ if and only if all active ID
 * bits are cleared, and restart a process.
 */
static int n_irqs = 0;
void irq_handle(int irq)
{
  irq_hook_t * hook;

  lock_irq();
  n_irqs++;

  /* here we need not to get this IRQ until all the handlers had a say */
  assert(irq >= 0 && irq < NR_IRQ_VECTORS);
  hw_intr_mask(irq);
  hook = irq_handlers[irq];

  /* Check for spurious interrupts. */
  if(hook == NULL) {
      static int nspurious[NR_IRQ_VECTORS], report_interval = 100;
      nspurious[irq]++;
      if(nspurious[irq] == 1 || !(nspurious[irq] % report_interval)) {
      	printf("irq_handle: spurious irq %d (count: %d); keeping masked\n",
		irq, nspurious[irq]);
	if(report_interval < INT_MAX/2)
		report_interval *= 2;
      }
      unlock_irq();
      return;
  }

  /* Compute how many hooks we need to store. */
  int n_hooks = 0;
  while(hook!=NULL) {
	  n_hooks++;
	  hook = hook->next;
  }

  irq_hook_t to_be_called[n_hooks]; // The hooks that will be called.

  /* Call list of handlers for an IRQ. */
  hook = irq_handlers[irq];
  int hook_idx = 0;
  while( hook != NULL ) {
    /* Add the hook. */
    to_be_called[hook_idx++] = *hook;
    /* Next hooked function. */
    hook = hook->next;
  }
  assert(hook_idx==n_hooks);

  /* Call the hooks without holding the irq lock. */
  unlock_irq();
  for(hook_idx=0;hook_idx<n_hooks;++hook_idx) {
	irq_hook_t h = to_be_called[hook_idx];
	lock_irq();
	irq_actids[irq] |= h.id;
	unlock_irq();
	/* Call the hooked function. */
	if((h.handler)(&h)) {
		lock_irq();
		irq_actids[h.irq] &= ~h.id;
		unlock_irq();
	}
  }

  lock_irq();
    
  /* reenable the IRQ only if there is no active handler */
  if (irq_actids[irq] == 0)
	  hw_intr_unmask(irq);

  hw_intr_ack(irq);
  unlock_irq();
}

/* Enable/Disable a interrupt line.  */
void enable_irq_no_lock(const irq_hook_t *hook)
{
  if((irq_actids[hook->irq] &= ~hook->id) == 0) {
    hw_intr_unmask(hook->irq);
  }
}

void enable_irq(const irq_hook_t *hook)
{
	lock_irq();
	enable_irq_no_lock(hook);
	unlock_irq();
}

/* Return true if the interrupt was enabled before call.  */
int disable_irq_no_lock(const irq_hook_t *hook)
{
  int res;
  if(irq_actids[hook->irq] & hook->id) {  /* already disabled */
    res = 0;
  } else {
	  irq_actids[hook->irq] |= hook->id;
	  hw_intr_mask(hook->irq);
	  res = TRUE;
  }
  return res;
}

int disable_irq(const irq_hook_t *hook)
{
	int res;
	lock_irq();
	res = disable_irq_no_lock(hook);
	unlock_irq();
	return res;
}
