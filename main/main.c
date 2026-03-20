/**
 * Genius (Simon Says) - Raspberry Pi Pico
 * 4 botões + 4 LEDs, sequência fixa de 6 passos para teste
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/time.h"

const int BTN_YELLOW_PIN = 12;
const int BTN_BLUE_PIN   = 13;
const int BTN_GREEN_PIN  = 14;
const int BTN_RED_PIN    = 15;

const int LED_YELLOW_PIN = 2;
const int LED_BLUE_PIN   = 3;
const int LED_GREEN_PIN  = 4;
const int LED_RED_PIN    = 5;

#define YELLOW  0
#define BLUE    1
#define GREEN   2
#define RED     3

#define STATE_SHOWING  0
#define STATE_PLAYER   1
#define STATE_WIN      2
#define STATE_LOSE     3

#define LED_ON_MS    600
#define LED_OFF_MS   250
#define PRE_SHOW_MS  1000

const int SEQUENCE_LEN   = 6;
const int sequence[6]    = {RED, GREEN, BLUE, YELLOW, RED, BLUE};
const int led_pins[4]    = {LED_YELLOW_PIN, LED_BLUE_PIN, LED_GREEN_PIN, LED_RED_PIN};

volatile int game_state    = STATE_SHOWING;
volatile int current_level = 1;
volatile int show_step     = 0;

volatile int player_step   = 0;
volatile int pressed_color = -1;

volatile uint32_t last_button_time = 0;

// ─── Alarm chain: exibe a sequência de LEDs ──────────────────────────────────

int64_t led_on_callback(alarm_id_t id, void *user_data);   // forward declaration

int64_t led_off_callback(alarm_id_t id, void *user_data) {
    gpio_put(led_pins[sequence[show_step]], 0);
    show_step++;

    if (show_step >= current_level) {
        // Sequência exibida — vez do jogador
        show_step   = 0;
        player_step = 0;
        game_state  = STATE_PLAYER;
    } else {
        add_alarm_in_ms(LED_OFF_MS, led_on_callback, NULL, false);
    }
    return 0;
}

int64_t led_on_callback(alarm_id_t id, void *user_data) {
    gpio_put(led_pins[sequence[show_step]], 1);
    add_alarm_in_ms(LED_ON_MS, led_off_callback, NULL, false);
    return 0;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

void all_leds_off() {
    for (int i = 0; i < 4; i++) gpio_put(led_pins[i], 0);
}

void start_showing() {
    show_step  = 0;
    game_state = STATE_SHOWING;
    add_alarm_in_ms(PRE_SHOW_MS, led_on_callback, NULL, false);
}

// ─── IRQ: botões ─────────────────────────────────────────────────────────────

void btn_callback(uint gpio, uint32_t events) {
    uint32_t now = time_us_32();
    if (now - last_button_time < 200000) {   // 200 ms debounce
        return;
    }
    last_button_time = now;

    if (events == 0x4 && game_state == STATE_PLAYER) {   // fall edge, vez do jogador
        if      (gpio == BTN_YELLOW_PIN) pressed_color = YELLOW;
        else if (gpio == BTN_BLUE_PIN)   pressed_color = BLUE;
        else if (gpio == BTN_GREEN_PIN)  pressed_color = GREEN;
        else if (gpio == BTN_RED_PIN)    pressed_color = RED;
    }
}

// ─── Setup ───────────────────────────────────────────────────────────────────

void setup() {
    gpio_init(BTN_YELLOW_PIN);
    gpio_set_dir(BTN_YELLOW_PIN, GPIO_IN);
    gpio_pull_up(BTN_YELLOW_PIN);

    gpio_init(BTN_BLUE_PIN);
    gpio_set_dir(BTN_BLUE_PIN, GPIO_IN);
    gpio_pull_up(BTN_BLUE_PIN);

    gpio_init(BTN_GREEN_PIN);
    gpio_set_dir(BTN_GREEN_PIN, GPIO_IN);
    gpio_pull_up(BTN_GREEN_PIN);

    gpio_init(BTN_RED_PIN);
    gpio_set_dir(BTN_RED_PIN, GPIO_IN);
    gpio_pull_up(BTN_RED_PIN);

    // Callback registrado no primeiro, habilitado nos demais (igual ao original)
    gpio_set_irq_enabled_with_callback(BTN_YELLOW_PIN, GPIO_IRQ_EDGE_FALL, true, &btn_callback);
    gpio_set_irq_enabled(BTN_BLUE_PIN,  GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_GREEN_PIN, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_RED_PIN,   GPIO_IRQ_EDGE_FALL, true);

    for (int i = 0; i < 4; i++) {
        gpio_init(led_pins[i]);
        gpio_set_dir(led_pins[i], GPIO_OUT);
        gpio_put(led_pins[i], 0);
    }
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
    stdio_init_all();
    setup();
    start_showing();

    while (true) {
        if (game_state == STATE_PLAYER && pressed_color >= 0) {
            int color  = pressed_color;
            pressed_color = -1;

            // Feedback visual do botão pressionado
            // sleep_ms aqui é intencional: feedback rápido de 300 ms,
            // não há operações sensíveis a tempo pendentes neste momento
            gpio_put(led_pins[color], 1);
            sleep_ms(300);
            gpio_put(led_pins[color], 0);
            sleep_ms(100);

            if (color == sequence[player_step]) {
                player_step++;

                if (player_step >= current_level) {
                    if (current_level >= SEQUENCE_LEN) {
                        // ── Vitória ──
                        game_state = STATE_WIN;
                        for (int i = 0; i < 4; i++) {
                            for (int j = 0; j < 4; j++) gpio_put(led_pins[j], 1);
                            sleep_ms(200);
                            all_leds_off();
                            sleep_ms(200);
                        }
                        current_level = 1;
                        sleep_ms(500);
                        start_showing();
                    } else {
                        // ── Próximo nível ──
                        current_level++;
                        sleep_ms(600);
                        start_showing();
                    }
                }
            } else {
                // ── Erro: pisca o LED errado ──
                game_state = STATE_LOSE;
                all_leds_off();
                for (int i = 0; i < 5; i++) {
                    gpio_put(led_pins[color], 1);
                    sleep_ms(150);
                    gpio_put(led_pins[color], 0);
                    sleep_ms(150);
                }
                current_level = 1;
                sleep_ms(500);
                start_showing();
            }
        }
        __wfi();
    }
}