#include <stdarg.h>

#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "user_interface.h"
#include "v7.h"
#include "mem.h"

#include "v7_uart.h"

/* At this moment using uart #0 only */
#define UART_MAIN 0
#define UART_DEBUG 1

#define UART_BASE(i) (0x60000000 + (i) *0xf00)
#define UART_INTR_STATUS(i) (UART_BASE(i) + 0x8)
#define UART_FORMAT_ERROR (BIT(3))
#define UART_RXBUF_FULL (BIT(0))
#define UART_RX_NEW (BIT(8))
#define UART_TXBUF_EMPTY (BIT(1))
#define UART_CTRL_INTR(i) (UART_BASE(i) + 0xC)
#define UART_CLEAR_INTR(i) (UART_BASE(i) + 0x10)
#define UART_DATA_STATUS(i) (UART_BASE(i) + 0x1C)
#define UART_BUF(i) UART_BASE(i)
#define UART_CONF_TX(i) (UART_BASE(i) + 0x20)
#define TASK_PRIORITY 0
#define RXTASK_QUEUE_LEN 10
#define RX_BUFFER_SIZE 0x100

#define UART_SIGINT_CHAR 0x03

uart_process_char_t uart_process_char;
volatile uart_process_char_t uart_interrupt_cb = NULL;
static os_event_t rx_task_queue[RXTASK_QUEUE_LEN];
static char rx_buf[RX_BUFFER_SIZE];
static unsigned s_system_uartno = UART_MAIN;
static unsigned debug_enabled = 0;

FAST static void rx_isr(void *param) {
  /* TODO(alashkin): add errors checking */
  unsigned int peri_reg = READ_PERI_REG(UART_INTR_STATUS(UART_MAIN));
  static volatile int tail = 0;

  if ((peri_reg & UART_RXBUF_FULL) != 0 || (peri_reg & UART_RX_NEW) != 0) {
    int char_count, i;
    CLEAR_PERI_REG_MASK(UART_CTRL_INTR(UART_MAIN),
                        UART_RXBUF_FULL | UART_RX_NEW);
    WRITE_PERI_REG(UART_CLEAR_INTR(UART_MAIN), UART_RXBUF_FULL | UART_RX_NEW);

    char_count = READ_PERI_REG(UART_DATA_STATUS(UART_MAIN)) & 0x000000FF;

    /* TODO(mkm): handle overrun */
    for (i = 0; i < char_count; i++, tail = (tail + 1) % RX_BUFFER_SIZE) {
      rx_buf[tail] = READ_PERI_REG(UART_BUF(UART_MAIN)) & 0xFF;
      if (rx_buf[tail] == UART_SIGINT_CHAR && uart_interrupt_cb) {
        /* swallow the intr byte */
        tail = (tail - 1) % RX_BUFFER_SIZE;
        uart_interrupt_cb(UART_SIGINT_CHAR);
      }
    }

    WRITE_PERI_REG(UART_CLEAR_INTR(UART_MAIN), UART_RXBUF_FULL | UART_RX_NEW);
    SET_PERI_REG_MASK(UART_CTRL_INTR(UART_MAIN), UART_RXBUF_FULL | UART_RX_NEW);

    system_os_post(TASK_PRIORITY, 0, tail);
  }
}

#ifdef V7_ESP_GDB_SERVER

int gdb_read_uart() {
  static char buf[256];
  static char pos = 0;
  if (pos == 0) {
    pos = gdb_read_uart_buf(buf);
  }
  if (pos == 0) {
    return -1;
  }
  return buf[--pos];
}

int gdb_read_uart_buf(char *buf) {
  unsigned int peri_reg = READ_PERI_REG(UART_INTR_STATUS(UART_MAIN));

  if ((peri_reg & UART_RXBUF_FULL) != 0 || (peri_reg & UART_RX_NEW) != 0) {
    int char_count, i;
    CLEAR_PERI_REG_MASK(UART_CTRL_INTR(UART_MAIN),
                        UART_RXBUF_FULL | UART_RX_NEW);
    WRITE_PERI_REG(UART_CLEAR_INTR(UART_MAIN), UART_RXBUF_FULL | UART_RX_NEW);

    char_count = READ_PERI_REG(UART_DATA_STATUS(UART_MAIN)) & 0x000000FF;

    for (i = 0; i < char_count; i++) {
      buf[i] = READ_PERI_REG(UART_BUF(UART_MAIN)) & 0xFF;
    }

    WRITE_PERI_REG(UART_CLEAR_INTR(UART_MAIN), UART_RXBUF_FULL | UART_RX_NEW);
    SET_PERI_REG_MASK(UART_CTRL_INTR(UART_MAIN), UART_RXBUF_FULL | UART_RX_NEW);
    return char_count;
  }
  return 0;
}
#endif

static void uart_tx_char(unsigned uartno, char ch) {
  while (true) {
    uint32 fifo_cnt =
        (READ_PERI_REG(UART_DATA_STATUS(uartno)) & 0x00FF0000) >> 16;
    if (fifo_cnt < 126) {
      break;
    }
  }
  WRITE_PERI_REG(UART_BUF(uartno), ch);
}

static int c_uvprintf(int uart, const char *format, va_list args) {
  static char buf[512];
  int size, i;
  /* TODO(mkm): add a callback to some c_xxxprintf */
  size = c_vsnprintf(buf, sizeof(buf), format, args);
  if (size <= 0) {
    return size;
  }
  if (size > sizeof(buf)) {
    size = sizeof(buf) - 1;
  }
  for (i = 0; i < size; i++) {
    if (buf[i] == '\n') uart_tx_char(uart, '\r');
    uart_tx_char(uart, buf[i]);
  }

  return size;
}

int c_printf(const char *format, ...) {
  int size;
  va_list arglist;
  va_start(arglist, format);
  size = c_uvprintf(UART_MAIN, format, arglist);
  va_end(arglist);
  return size;
}

/* This is a workaround for #576. It only supports stdout/stderr */
void fprint_str(FILE *fp, const char *str) {
  int uart = (fp == stderr) ? s_system_uartno : UART_MAIN;
  if (fp == stderr && !debug_enabled) return;

  while (*str != '\0') {
    if (*str == '\n') uart_tx_char(uart, '\r');
    uart_tx_char(uart, *str);
    str++;
  }
}

/* This is a workaround for #576 */
void print_str(const char *str) {
  fprint_str(stdout, str);
}

/* shim for fprintf. Handles only (pseudo) stderr */
int c_ufprintf(FILE *fp, const char *format, ...) {
  int size = -1;
  va_list arglist;
  va_start(arglist, format);

  if (fp == stderr) {
    if (debug_enabled) {
      size = c_uvprintf(s_system_uartno, format, arglist);
    } else {
      size = 0;
    }
  }
  va_end(arglist);
  return size;
}

void rx_task(os_event_t *events) {
  static int head = 0;
  int tail = events->par;
  int i;

  if (events->sig != 0) {
    return;
  }

  for (i = head; i != tail; i = (i + 1) % RX_BUFFER_SIZE) {
    uart_process_char(rx_buf[i]);
  }
  head = tail;
}

void uart_main_init(int baud_rate) {
  system_os_task(rx_task, TASK_PRIORITY, rx_task_queue, RXTASK_QUEUE_LEN);

  if (baud_rate != 0) {
    uart_div_modify(0, UART_CLK_FREQ / baud_rate);
  }

  ETS_UART_INTR_ATTACH(rx_isr, 0);
  ETS_UART_INTR_ENABLE();
}

static void uart_system_tx_char(char ch) {
  if (ch == '\n') {
    uart_tx_char(s_system_uartno, '\r');
    uart_tx_char(s_system_uartno, '\n');
  } else if (ch != '\r') {
    uart_tx_char(s_system_uartno, ch);
  }
}

int uart_redirect_debug(int mode) {
  switch (mode) {
    case 0:
      debug_enabled = 0;
      break;
    case 1:
    case 2:
      debug_enabled = 1;
      s_system_uartno = mode - 1;
      break;
    default:
      return -1;
  }
  system_set_os_print(debug_enabled);

  return 0;
}

void uart_debug_init(unsigned periph, unsigned baud_rate) {
  if (periph == 0) {
    periph = PERIPHS_IO_MUX_GPIO2_U;
  }

  if (baud_rate == 0) {
    baud_rate = 115200;
  }

  PIN_FUNC_SELECT(periph, FUNC_U1TXD_BK);
  uart_div_modify(UART_DEBUG, UART_CLK_FREQ / baud_rate);

  /* Magic: set 8-N-1 mode */
  WRITE_PERI_REG(UART_CONF_TX(UART_DEBUG), 0xC);

  os_install_putc1((void *) uart_system_tx_char);
}
