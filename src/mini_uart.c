#include "peripherals/mini_uart.h"
#include "peripherals/gpio.h"
#include "utils.h"
#include "printf.h"

static void _uart_send(char c) {
  while (1) {
    if (get32(AUX_MU_LSR_REG) & 0x20)
      break;
  }
  put32(AUX_MU_IO_REG, c);
}

void uart_send(char c) {
  if (c == '\n') {
    _uart_send('\r');
    _uart_send('\n');
  } else {
    _uart_send(c);
  }
}

char uart_recv(void) {
  while (1) {
    if (get32(AUX_MU_LSR_REG) & 0x01)
      break;
  }

  char c = get32(AUX_MU_IO_REG) & 0xFF;
  if (c == '\r')
    c = '\n';

  return c;
}

void handle_uart_irq(void) {
  printf("receive %c\n", get32(AUX_MU_IO_REG) & 0xff);
  put32(AUX_MU_IIR_REG, 0x2); // clear interrupt
}

void uart_init(void) {
  unsigned int selector;

  selector = get32(GPFSEL1);
  selector &= ~(7 << 12); // clean gpio14
  selector |= 2 << 12;    // set alt5 for gpio14
  selector &= ~(7 << 15); // clean gpio15
  selector |= 2 << 15;    // set alt5 for gpio15
  put32(GPFSEL1, selector);

  put32(GPPUD, 0);
  delay(150);
  put32(GPPUDCLK0, (1 << 14) | (1 << 15));
  delay(150);
  put32(GPPUDCLK0, 0);

  put32(AUX_ENABLES, 1); // Enable mini uart (this also enables access to it registers)
  put32(AUX_MU_CNTL_REG, 0); // Disable auto flow control and disable receiver
                             // and transmitter (for now)
  put32(AUX_MU_IER_REG, 1);    // Enable receive interrupt
  put32(AUX_MU_LCR_REG, 3);    // Enable 8 bit mode
  put32(AUX_MU_MCR_REG, 0);    // Set RTS line to be always high
  put32(AUX_MU_BAUD_REG, 270); // Set baud rate to 115200

  put32(AUX_MU_CNTL_REG, 3); // Finally, enable transmitter and receiver
}

// This function is required by printf function
void putc(void *p, char c) { uart_send(c); }
