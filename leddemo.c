/*
 * neopixel_demo.c
 *
 * mmap’s physical address 0x18183070 to control 5 NeoPixels via PCIe.
 * Patterns:
 *  1) Color-wipe fade (red → green → blue)
 *  2) Rainbow chase (“walking” rainbow)
 *  3) Fade between two user colors
 *
 * Compile with:
 *   gcc -o neopixel_demo neopixel_demo.c
 *
 * Run as root (needs /dev/mem access).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <timer.h>
#include <uart.h>
#include <prism.h>

#define LED_COUNT      7

// Brightness in 8.8 fixed point: 256 = 100%, 128 = 50%, etc.
#define BRIGHTNESS_FULL 256
uint16_t gBrightness = 64;

static volatile uint32_t led_regs[7] = {0, };
static volatile int frozen = 1;

void timer_callback(void*) {
   frozen = 0;
}

void flush_leds(int time_ms)
{
    int  i;

    // Ensure interrupts are clear
    prism_clear_interrupt();

    // Load LED color 0
    prism_set_count1_preload(led_regs[0]);
    prism_host_toggle();

    for (i = 1; i < 7; i++)
    {
        // Wait for the interrupt
        while ((prism_read32(PRISM_REG_CTRL) & PRISM_CTRL_INTERRUPT) == 0)
            ;

        // Load next LED color
        prism_set_count1_preload(led_regs[i]);
        prism_host_toggle();
    }

    // Now set an alarm to freeze us for specified ms
    frozen = 1;
    set_alarm(time_ms, timer_callback, NULL);
    while (frozen)
       ;
}

// Convert HSV (h: 0–359°, s,v: 0–255) to 8-bit RGB
void hsv2rgb(uint16_t h, uint8_t s, uint8_t v,
             uint8_t *out_r, uint8_t *out_g, uint8_t *out_b)
{
    uint8_t r, g, b;
    if (s == 0) {
        r = g = b = v;
    } else {
        h %= 360;
        uint32_t i = h / 60;
        uint32_t f = ((h % 60) * 255) / 60;   // fractional part, 0–255
        uint8_t p = (v * (255 - s)) / 255;
        uint8_t q = (v * (255 - (s * f) / 255)) / 255;
        uint8_t t = (v * (255 - (s * (255 - f)) / 255)) / 255;
        switch (i) {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
        }
    }
    *out_r = r;
    *out_g = g;
    *out_b = b;
}

// Write LED color scaled by gBrightness (8.8 fixed point)
static inline void set_color(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t rb = (uint8_t)((g * gBrightness) >> 8);
    uint8_t gb = (uint8_t)((r * gBrightness) >> 8);
    uint8_t bb = (uint8_t)((b * gBrightness) >> 8);
    uint32_t val = ((uint32_t)rb <<  8) |
                   ((uint32_t)gb << 16) |
                   ((uint32_t)bb <<  0);
    led_regs[idx] = val;
}

// Pattern 1: color-wipe fade through R→G→B
void pattern_wipe()
{
    const uint8_t colors[][3] = {
        {255,   0,   0},  // red
        {  0, 255,   0},  // green
        {  0,   0, 255},  // blue
    };
    for (int stage = 0; stage < 3; stage++) {
        uint8_t *c1 = (uint8_t *)colors[stage];
        uint8_t *c2 = (uint8_t *)colors[(stage+1)%3];
        for (int t = 0; t <= 100; t++) {
            uint8_t r = ((100-t)*c1[0] + t*c2[0]) / 100;
            uint8_t g = ((100-t)*c1[1] + t*c2[1]) / 100;
            uint8_t b = ((100-t)*c1[2] + t*c2[2]) / 100;
            for (int i = 0; i < LED_COUNT; i++)
                set_color(i, r, g, b);
            flush_leds(20);
        }
    }
}

// Pattern 2: rainbow chase (“walking” rainbow)
void pattern_rainbow()
{
    for (int cycle = 0; cycle < 360; cycle += 2) {
        for (int i = 0; i < LED_COUNT; i++) {
            uint16_t hue = (cycle + (360 * i) / LED_COUNT) % 360;
            uint8_t r, g, b;
            hsv2rgb(hue, 255, 128, &r, &g, &b);
            set_color(i, r, g, b);
        }
        flush_leds(30);
    }
}

// Pattern 3: fade between two arbitrary colors
void pattern_fade_two(uint8_t c1[3], uint8_t c2[3])
{
    for (int t = 0; t <= 200; t++) {
        uint8_t r = ((200-t)*c1[0] + t*c2[0]) / 200;
        uint8_t g = ((200-t)*c1[1] + t*c2[1]) / 200;
        uint8_t b = ((200-t)*c1[2] + t*c2[2]) / 200;
        for (int i = 0; i < LED_COUNT; i++)
            set_color(i, r, g, b);
        flush_leds(15);
    }
}

// Pattern 4: circle the moon
void pattern_circle(int direction)
{
    const uint8_t colors[][3] = {
        {255,   0,   0},  // red
        {255, 128,   0},  // yellow
        {  0,   0, 255},  // blue
        {  0, 255,   0},  // green
        {255,   0, 128},  // purple
    };
    // Make all LEDs RED
    for (int color = 0; color < 5; color++) {
        for (int l = 0; l < LED_COUNT; l++)
        {
            set_color(l, 0, 0, 0);
        }
        for (int x = 0; x < 3; x++)
        {
            for (int i = 1; i < LED_COUNT; i++) {
                int led = direction ? LED_COUNT-i : i;
                int led_prev = direction ? led+1 : led-1;
                if (led_prev < 1)
                    led_prev = 6;
                if (led_prev > 6)
                    led_prev = 1;
                int led_prev2 = direction ? led_prev+1 : led_prev-1;
                if (led_prev2 < 1)
                    led_prev2 = 6;
                if (led_prev2 > 6)
                    led_prev2 = 1;
                set_color(led, colors[color][0], colors[color][1], colors[color][2]);
                flush_leds(80);
                set_color(led, colors[color][0]/3, colors[color][1]/3, colors[color][2]/3);
                set_color(led_prev, colors[color][0]/6, colors[color][1]/6, colors[color][2]/6);
                set_color(led_prev2, 000,0,0);
            }
        }
    }
}

// Pattern 5: star crossed
void pattern_star_crossed(void)
{
    const uint8_t colors[][3] = {
        {255,   0,   0},  // red
        {255, 128,   0},  // yellow
        {  0,   0, 255},  // blue
        {  0, 255,   0},  // green
        {255,   0, 128},  // purple
    };
    for (int l = 0; l < LED_COUNT; l++)
    {
        set_color(l, 0, 0, 0);
    }
    int led = 0;
    // Make all LEDs RED
    for (int color = 0; color < 5; color++) {
        for (int x = 0; x < 1; x++)
        {
            for (int i = 0; i < LED_COUNT+2; i++) {
                // Do the star across action
                set_color(led%6+1, 0, 0, 0);
                led += 3;
                set_color(led%6+1, colors[color][0], colors[color][1], colors[color][2]);
                flush_leds(100);

                set_color(led%6+1, 0, 0, 0);
                led -= 2;
                set_color(led%6+1, colors[color][0], colors[color][1], colors[color][2]);
                flush_leds(100);
            }
        }
    }
}

// Pattern 6: circle the moon
void pattern_zip()
{
    const uint8_t colors[][3] = {
        {255,   0,   0},  // red
        {255, 128,   0},  // yellow
        {  0,   0, 255},  // blue
        {  0, 255,   0},  // green
        {255,   0, 128},  // purple
        {  0,   0,   0},  // black
    };
    for (int l = 0; l < LED_COUNT; l++)
    {
        set_color(l, 0, 0, 0);
    }

    // Make all LEDs RED
    uint16_t save = gBrightness;
    for (int color = 0; color < 6; color++) {
        for (int i = 1; i < LED_COUNT; i++) {
            set_color(i, colors[color][0], colors[color][1], colors[color][2]);
            flush_leds(120);
        }
        gBrightness = BRIGHTNESS_FULL;
        set_color(1, colors[color][0], colors[color][1], colors[color][2]);
        flush_leds(40);
        gBrightness = save;
    }
}

void flashy(int argc, char *argv[])
{
    // Brightness argument in 8.8 fixed point: 256 = 100%, 128 = 50%
    if (argc > 1)
        gBrightness = atoi(argv[1]);

    // Run patterns forever
    while (1) {
        pattern_zip();
        if (uart_getc() != -1)
           return;
        pattern_star_crossed();
        if (uart_getc() != -1)
           return;
        pattern_wipe();
        if (uart_getc() != -1)
           return;
        pattern_circle(1);
        if (uart_getc() != -1)
           return;
        pattern_rainbow();
        if (uart_getc() != -1)
           return;

        uint8_t magenta[3] = {255, 0, 255};
        uint8_t cyan   [3] = {0, 255, 255};
        pattern_fade_two(magenta, cyan);
        if (uart_getc() != -1)
           return;

        pattern_circle(0);
        if (uart_getc() != -1)
           return;

        if (uart_getc() != -1)
           break;
    }
}

