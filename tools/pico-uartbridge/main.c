// USB <-> UART0 bridge for the head unit debug console (576000 8N1).
// GP1 = UART0 RX  <- head unit PB06 (TX).  GP0 = UART0 TX -> PB07 (RX), optional.
#include "pico/stdlib.h"
#include "hardware/uart.h"
int main(void) {
    stdio_init_all();
    uart_init(uart0, 576000);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    gpio_pull_up(1);                         // idle-high when the probe is lifted
    gpio_init(PICO_DEFAULT_LED_PIN); gpio_set_dir(PICO_DEFAULT_LED_PIN, 1);
    uint32_t led_off = 0;
    for (;;) {
        while (uart_is_readable(uart0)) {
            putchar_raw(uart_getc(uart0));
            gpio_put(PICO_DEFAULT_LED_PIN, 1); led_off = time_us_32() + 50000;
        }
        int c = getchar_timeout_us(0);
        if (c >= 0) uart_putc_raw(uart0, (char)c);
        if (led_off && (int32_t)(time_us_32() - led_off) > 0) { gpio_put(PICO_DEFAULT_LED_PIN, 0); led_off = 0; }
    }
}
