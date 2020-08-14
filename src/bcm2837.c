#include <inttypes.h>
#include "board.h"
#include "debug.h"
#include "bcm2837.h"
#include "mm.h"
#include "fifo.h"
#include "peripherals/mini_uart.h"
#include "peripherals/timer.h"
#include "peripherals/irq.h"

struct bcm2837_state {
  struct {
    uint8_t irq_enabled[72]; // IRQ 0-64, ARM Timer, ARM Mailbox, ...
    uint8_t fiq_control;
    uint32_t irqs_1_enabled;
    uint32_t irqs_2_enabled;
    uint8_t  basic_irqs_enabled;
  } intctrl;

  struct {
    struct fifo *mu_tx_fifo;
    struct fifo *mu_rx_fifo;
    int      mu_rx_overrun;
    uint8_t  aux_enables;
    uint8_t  aux_mu_io;
    uint8_t  aux_mu_ier;
    uint8_t  aux_mu_lcr;
    uint8_t  aux_mu_mcr;
    uint8_t  aux_mu_msr;
    uint8_t  aux_mu_scratch;
    uint8_t  aux_mu_cntl;
    uint16_t aux_mu_baud;
  } aux;

  struct {
    uint32_t cs;
    uint64_t counter;
    uint32_t c0;
    uint32_t c1;
    uint32_t c2;
    uint32_t c3;
  } systimer;
};

const struct bcm2837_state initial_state = {
  .intctrl = {
    .fiq_control        = 0x0,
    .irqs_1_enabled     = 0x0,
    .irqs_2_enabled     = 0x0,
    .basic_irqs_enabled = 0x0,
  },
  .aux = {
    .mu_rx_overrun  = 0,
    .aux_enables    = 0x0,
    .aux_mu_io      = 0x0,
    .aux_mu_ier     = 0x0,
    .aux_mu_lcr     = 0x0,
    .aux_mu_mcr     = 0x0,
    .aux_mu_msr     = 0x10,
    .aux_mu_scratch = 0x0,
    .aux_mu_cntl    = 0x3,
    .aux_mu_baud    = 0x0,
  },
  .systimer = {
    .cs  = 0x0,
    .counter = 0x0,
    .c0  = 0x0,
    .c1  = 0x0,
    .c2  = 0x0,
    .c3  = 0x0,
  },
};

#define ADDR_IN_INTCTRL(a) ((a) >= IRQ_BASIC_PENDING && (a) <= DISABLE_BASIC_IRQS)
#define ADDR_IN_AUX(a) ((a) >= AUX_IRQ && (a) <= AUX_MU_BAUD_REG)
#define ADDR_IN_AUX_MU(a) ((a) >= AUX_MU_IO_REG && (a) <= AUX_MU_BAUD_REG)
#define ADDR_IN_SYSTIMER(a) ((a) >= TIMER_CS && (a) <= TIMER_C3)

void bcm2837_initialize(struct task_struct *tsk) {
  struct bcm2837_state *s = (struct bcm2837_state *)allocate_page();
  *s = initial_state;

  s->aux.mu_tx_fifo = create_fifo();
  s->aux.mu_rx_fifo = create_fifo();

  tsk->board_data = s;

  unsigned long begin = DEVICE_BASE;
  unsigned long end = PHYS_MEMORY_SIZE - SECTION_SIZE;
  for (; begin < end; begin += PAGE_SIZE) {
    set_task_page_notaccessable(tsk, begin);
  }
}

unsigned long handle_aux_read(struct task_struct *, unsigned long);

unsigned long handle_intctrl_read(struct task_struct *tsk, unsigned long addr) {
#define BIT(v, n) ((v) & (1 << (n)))
  struct bcm2837_state *s = (struct bcm2837_state *)tsk->board_data;
  switch (addr) {
  case IRQ_BASIC_PENDING:
    {
      int pending1 = handle_intctrl_read(tsk, IRQ_PENDING_1) != 0;
      int pending2 = handle_intctrl_read(tsk, IRQ_PENDING_2) != 0;
      return (pending1 << 8) | (pending2 << 9);
    }
  case IRQ_PENDING_1:
    {
      unsigned long systimer_match1 =
        BIT(s->intctrl.irqs_1_enabled, 1) && (s->systimer.cs & 0x2);
      unsigned long systimer_match3 =
        BIT(s->intctrl.irqs_1_enabled, 3) && (s->systimer.cs & 0x8);
      unsigned long int uart_int =
        BIT(s->intctrl.irqs_1_enabled, (57-32)) && (handle_aux_read(tsk, AUX_IRQ) & 0x1);
      return (systimer_match1 << 1) | (systimer_match3 << 3) |
        (uart_int << 57);
    }
  case IRQ_PENDING_2:
    return 0;
  case FIQ_CONTROL:
    return s->intctrl.fiq_control;
  case ENABLE_IRQS_1:
    return s->intctrl.irqs_1_enabled;
  case ENABLE_IRQS_2:
    return s->intctrl.irqs_2_enabled;
  case ENABLE_BASIC_IRQS:
    return s->intctrl.basic_irqs_enabled;
  case DISABLE_IRQS_1:
    return ~s->intctrl.irqs_1_enabled;
  case DISABLE_IRQS_2:
    return ~s->intctrl.irqs_2_enabled;
  case DISABLE_BASIC_IRQS:
    return ~s->intctrl.basic_irqs_enabled;
  }
  return 0;
}

void handle_intctrl_write(struct task_struct *tsk, unsigned long addr, unsigned long val) {
  struct bcm2837_state *s = (struct bcm2837_state *)tsk->board_data;
  switch (addr) {
  case FIQ_CONTROL:
    s->intctrl.fiq_control = val;
  case ENABLE_IRQS_1:
    s->intctrl.irqs_1_enabled |= val;
  case ENABLE_IRQS_2:
    s->intctrl.irqs_2_enabled |= val;
  case ENABLE_BASIC_IRQS:
    s->intctrl.basic_irqs_enabled |= val;
  case DISABLE_IRQS_1:
    s->intctrl.irqs_1_enabled &= ~val;
  case DISABLE_IRQS_2:
    s->intctrl.irqs_2_enabled &= ~val;
  case DISABLE_BASIC_IRQS:
    s->intctrl.basic_irqs_enabled &= ~val;
  }
}

#define LCR_DLAB 0x80

unsigned long handle_aux_read(struct task_struct *tsk, unsigned long addr) {
  struct bcm2837_state *s = (struct bcm2837_state *)tsk->board_data;

  if ((s->aux.aux_enables & 1) == 0 && ADDR_IN_AUX_MU(addr)) {
    return 0;
  }

  switch (addr) {
  case AUX_IRQ:
    {
      int mu_pending = (s->aux.aux_enables & 0x1) &&
        ~(handle_aux_read(tsk, AUX_MU_IIR_REG) & 0x1);
      return mu_pending;
    }
  case AUX_ENABLES:
    return s->aux.aux_enables;
  case AUX_MU_IO_REG:
    if (s->aux.aux_mu_lcr & LCR_DLAB) {
      s->aux.aux_mu_lcr &= ~LCR_DLAB;
      return s->aux.aux_mu_baud & 0xff;
    } else {
      unsigned long data;
      dequeue_fifo(s->aux.mu_rx_fifo, &data);
      return data;
    }
  case AUX_MU_IER_REG:
    if (s->aux.aux_mu_lcr & LCR_DLAB) {
      return s->aux.aux_mu_baud >> 8;
    } else {
      return s->aux.aux_mu_ier;
    }
  case AUX_MU_IIR_REG:
    {
      int tx_int = (s->aux.aux_mu_ier & 0x2) && is_empty_fifo(s->aux.mu_tx_fifo);
      int rx_int = (s->aux.aux_mu_ier & 0x1) && !is_empty_fifo(s->aux.mu_rx_fifo);
      int int_id = tx_int | (rx_int << 1);
      if (int_id == 0x3)
        int_id = 0x1;
      return (!int_id) | (int_id << 1) | (0x3 << 6);
    }
  case AUX_MU_LCR_REG:
    return s->aux.aux_mu_lcr;
  case AUX_MU_MCR_REG:
    return s->aux.aux_mu_mcr;
  case AUX_MU_LSR_REG:
    {
      int dready = !is_empty_fifo(s->aux.mu_rx_fifo);
      int rx_overrun = s->aux.mu_rx_overrun;
      int tx_empty = !is_full_fifo(s->aux.mu_tx_fifo);
      int tx_idle = is_empty_fifo(s->aux.mu_tx_fifo);
      s->aux.mu_rx_overrun = 0;
      return dready | (rx_overrun << 1) | (tx_empty << 5) | (tx_idle << 6);
    }
  case AUX_MU_MSR_REG:
    return s->aux.aux_mu_msr;
  case AUX_MU_SCRATCH:
    return s->aux.aux_mu_scratch;
  case AUX_MU_CNTL_REG:
    return s->aux.aux_mu_cntl;
  case AUX_MU_STAT_REG:
    {
#define MIN(a,b) ((a)<(b)?(a):(b))
      int sym_avail = !is_empty_fifo(s->aux.mu_rx_fifo);
      int space_avail = !is_full_fifo(s->aux.mu_tx_fifo);
      int rx_idle = is_empty_fifo(s->aux.mu_rx_fifo);
      int tx_idle = !is_empty_fifo(s->aux.mu_tx_fifo);
      int rx_overrun = s->aux.mu_rx_overrun;
      int tx_full = !space_avail;
      int tx_empty = is_empty_fifo(s->aux.mu_tx_fifo);
      int tx_done = rx_idle & tx_empty;
      int rx_fifo_level = MIN(used_of_fifo(s->aux.mu_rx_fifo), 8);
      int tx_fifo_level = MIN(used_of_fifo(s->aux.mu_tx_fifo), 8);
      return sym_avail | (space_avail << 1) | (rx_idle << 2) |
        (tx_idle << 3) | (rx_overrun << 4) | (tx_full << 5) |
        (tx_empty << 8) | (tx_done << 9) | (rx_fifo_level << 16) |
        (tx_fifo_level << 24);
    }
  case AUX_MU_BAUD_REG:
    return s->aux.aux_mu_baud;
  }
  return 0;
}

void handle_aux_write(struct task_struct *tsk, unsigned long addr, unsigned long val) {
  struct bcm2837_state *s = (struct bcm2837_state *)tsk->board_data;

  if ((s->aux.aux_enables & 1) == 0 && ADDR_IN_AUX_MU(addr)) {
    return;
  }

  switch (addr) {
  case AUX_ENABLES:
    s->aux.aux_enables = val;
    break;
  case AUX_MU_IO_REG:
    if (s->aux.aux_mu_lcr & LCR_DLAB) {
      s->aux.aux_mu_lcr &= ~LCR_DLAB;
      s->aux.aux_mu_baud =
        (s->aux.aux_mu_baud & 0xff00) | (val & 0xff);
    } else {
      enqueue_fifo(s->aux.mu_tx_fifo, val & 0xff);
    }
    break;
  case AUX_MU_IER_REG:
    if (s->aux.aux_mu_lcr & LCR_DLAB) {
      s->aux.aux_mu_baud =
        (s->aux.aux_mu_baud & 0xff) | ((val & 0xff) << 8);
    } else {
      s->aux.aux_mu_ier = val;
    }
    break;
  case AUX_MU_IIR_REG:
    if (val & 0x2)
      clear_fifo(s->aux.mu_rx_fifo);
    if (val & 0x4)
      clear_fifo(s->aux.mu_tx_fifo);
    break;
  case AUX_MU_LCR_REG:
    s->aux.aux_mu_lcr = val;
    break;
  case AUX_MU_MCR_REG:
    s->aux.aux_mu_mcr = val;
    break;
  case AUX_MU_SCRATCH:
    s->aux.aux_mu_scratch = val;
    break;
  case AUX_MU_CNTL_REG:
    s->aux.aux_mu_cntl = val;
    break;
  case AUX_MU_BAUD_REG:
    s->aux.aux_mu_baud = val;
    break;
  }
}

unsigned long handle_systimer_read(struct task_struct *tsk, unsigned long addr) {
  struct bcm2837_state *s = (struct bcm2837_state *)tsk->board_data;
  switch (addr) {
  case TIMER_CS:
    return s->systimer.cs;
  case TIMER_CLO:
    return s->systimer.counter & 0xffffffff;
  case TIMER_CHI:
    return s->systimer.counter >> 32;
  case TIMER_C0:
    return s->systimer.c0;
  case TIMER_C1:
    return s->systimer.c1;
  case TIMER_C2:
    return s->systimer.c2;
  case TIMER_C3:
    return s->systimer.c3;
  }
  return 0;
}

void handle_systimer_write(struct task_struct *tsk, unsigned long addr, unsigned long val) {
  struct bcm2837_state *s = (struct bcm2837_state *)tsk->board_data;
  switch (addr) {
  case TIMER_CS:
    s->systimer.cs &= ~val;
  case TIMER_C0:
    s->systimer.c0 = val;
  case TIMER_C1:
    s->systimer.c1 = val;
  case TIMER_C2:
    s->systimer.c2 = val;
  case TIMER_C3:
    s->systimer.c3 = val;
  }
}

unsigned long bcm2837_mmio_read(struct task_struct *tsk, unsigned long addr) {
  if (ADDR_IN_INTCTRL(addr)) {
    return handle_intctrl_read(tsk, addr);
  } else if (ADDR_IN_AUX(addr)) {
    return handle_aux_read(tsk, addr);
  } else if (ADDR_IN_SYSTIMER(addr)) {
    return handle_systimer_read(tsk, addr);
  }
  return 0;
}

void bcm2837_mmio_write(struct task_struct *tsk, unsigned long addr, unsigned long val) {
  if (ADDR_IN_INTCTRL(addr)) {
    handle_intctrl_write(tsk, addr, val);
  } else if (ADDR_IN_AUX(addr)) {
    handle_aux_write(tsk, addr, val);
  } else if (ADDR_IN_SYSTIMER(addr)) {
    handle_systimer_write(tsk, addr, val);
  }
}

void bcm2837_timer_tick(struct task_struct *tsk) {
  struct bcm2837_state *s = (struct bcm2837_state *)tsk->board_data;

  s->systimer.counter++;

  uint32_t clo = s->systimer.counter & 0xffff;
  int matched = (clo == s->systimer.c0) |
    ((clo == s->systimer.c1) << 1) |
    ((clo == s->systimer.c2) << 2) |
    ((clo == s->systimer.c3) << 3);

  int fired = (~s->systimer.cs) & matched;
  s->systimer.cs |= fired;
}

int bcm2837_is_irq_asserted(struct task_struct *tsk) {
  return handle_intctrl_read(tsk, IRQ_BASIC_PENDING) != 0;
}

int bcm2837_is_fiq_asserted(struct task_struct *tsk) {
  return 0; // TODO
}

const struct board_ops bcm2837_board_ops = {
  .initialize = bcm2837_initialize,
  .mmio_read  = bcm2837_mmio_read,
  .mmio_write = bcm2837_mmio_write,
  .timer_tick = bcm2837_timer_tick,
  .is_irq_asserted = bcm2837_is_irq_asserted,
  .is_fiq_asserted = bcm2837_is_fiq_asserted,
};