/**
 * Genius (Simon Says) — Raspberry Pi Pico
 *
 * Máquina de estados sem sleep_ms.
 * Toda temporização via alarm callbacks.
 * Sequência aleatória gerada ao pressionar START.
 *
 * Botão START (pino 27) inicia/reinicia o jogo.
 * Em idle todos os LEDs ficam apagados.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/time.h"

/* ── Pinos ─────────────────────────────────────────────────────────────────── */

#define BTN_YELLOW 10
#define BTN_BLUE   11
#define BTN_GREEN  12
#define BTN_RED    13
#define BTN_START  27

#define LED_YELLOW 2
#define LED_BLUE   3
#define LED_GREEN  4
#define LED_RED    5

/* ── Cores (índices) ───────────────────────────────────────────────────────── */

#define YELLOW 0
#define BLUE   1
#define GREEN  2
#define RED    3

/* ── Estados ───────────────────────────────────────────────────────────────── */

typedef enum {
    ST_IDLE,       // aguardando botão START
    ST_SHOW_PRE,   // pausa antes de exibir a sequência
    ST_SHOW_ON,    // LED da sequência aceso
    ST_SHOW_OFF,   // intervalo entre LEDs da sequência
    ST_PLAYER,     // aguardando botão do jogador
    ST_FEEDBACK,   // LED de feedback aceso após botão
    ST_LEVEL_UP,   // pausa antes do próximo nível
    ST_ERROR,      // animação de erro (todos piscam)
    ST_WAIT_START, // pós-erro/vitória: espera START
    ST_WIN,        // animação de vitória
} state_t;

/* ── Tempos (ms) ───────────────────────────────────────────────────────────── */

#define PRE_SHOW_MS   1000
#define LED_ON_MS      600
#define LED_OFF_MS     250
#define TIMEOUT_MS    5000
#define FEEDBACK_MS    300
#define LEVEL_UP_MS    800
#define ERR_BLINK_MS   150
#define ERR_BLINKS       5
#define WIN_BLINK_MS   200
#define WIN_BLINKS       4

/* ── Sequência ─────────────────────────────────────────────────────────────── */

#define SEQ_LEN 6
static int       sequence[SEQ_LEN];
static const int led_pins[4] = {LED_YELLOW, LED_BLUE, LED_GREEN, LED_RED};

/* ── PRNG simples (xorshift32) — sem stdlib ────────────────────────────────── */

static uint32_t rng_state = 0;

static uint32_t xorshift32(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void generate_sequence(void) {
    rng_state = time_us_32();
    if (rng_state == 0) rng_state = 1;
    for (int i = 0; i < SEQ_LEN; i++)
        sequence[i] = xorshift32() % 4;
}

/* ── Variáveis de ativação (volatile: compartilhadas com IRQ/callbacks) ────── */

static volatile state_t    state          = ST_IDLE;
static volatile int        current_level  = 1;
static volatile int        show_idx       = 0;
static volatile int        player_idx     = 0;
static volatile int        pressed_color  = -1;
static volatile int        feedback_color = -1;
static volatile int        blink_count    = 0;
static volatile bool       start_pressed  = false;
static volatile bool       alarm_fired    = false;
static volatile alarm_id_t timeout_id     = 0;
static volatile uint32_t   last_btn_us    = 0;

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void all_leds(bool on) {
    for (int i = 0; i < 4; i++)
        gpio_put(led_pins[i], on);
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  Callback de alarme — apenas sinaliza o main                              *
 * ═══════════════════════════════════════════════════════════════════════════ */

static int64_t cb_alarm(alarm_id_t id, void *data) {
    alarm_fired = true;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  IRQ: botões — apenas seta variáveis de ativação                          *
 * ═══════════════════════════════════════════════════════════════════════════ */

static void btn_callback(uint gpio, uint32_t events) {
    uint32_t now = time_us_32();
    if (now - last_btn_us < 200000)
        return;
    last_btn_us = now;

    if (!(events & GPIO_IRQ_EDGE_FALL))
        return;

    if (gpio == BTN_START) {
        if (state == ST_IDLE || state == ST_WAIT_START)
            start_pressed = true;
        return;
    }

    if (state != ST_PLAYER)
        return;

    switch (gpio) {
        case BTN_YELLOW: pressed_color = YELLOW; break;
        case BTN_BLUE:   pressed_color = BLUE;   break;
        case BTN_GREEN:  pressed_color = GREEN;  break;
        case BTN_RED:    pressed_color = RED;     break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  Setup                                                                     *
 * ═══════════════════════════════════════════════════════════════════════════ */

static void setup(void) {
    const int btns[] = {BTN_YELLOW, BTN_BLUE, BTN_GREEN, BTN_RED, BTN_START};

    for (int i = 0; i < 5; i++) {
        gpio_init(btns[i]);
        gpio_set_dir(btns[i], GPIO_IN);
        gpio_pull_up(btns[i]);
    }

    gpio_set_irq_enabled_with_callback(BTN_YELLOW, GPIO_IRQ_EDGE_FALL, true, &btn_callback);
    gpio_set_irq_enabled(BTN_BLUE,   GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_GREEN,  GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_RED,    GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_START,  GPIO_IRQ_EDGE_FALL, true);

    for (int i = 0; i < 4; i++) {
        gpio_init(led_pins[i]);
        gpio_set_dir(led_pins[i], GPIO_OUT);
        gpio_put(led_pins[i], 0);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  Main — máquina de estados completa                                        *
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    stdio_init_all();
    setup();

    while (true) {

        /* ── START pressionado ── */
        if (start_pressed) {
            start_pressed = false;
            generate_sequence();
            current_level = 1;
            all_leds(false);
            show_idx = 0;
            state    = ST_SHOW_PRE;
            add_alarm_in_ms(PRE_SHOW_MS, cb_alarm, NULL, false);
        }

        /* ── Alarme disparou: processa estado atual ── */
        if (alarm_fired) {
            alarm_fired = false;

            switch (state) {

                case ST_SHOW_PRE:
                    state = ST_SHOW_ON;
                    gpio_put(led_pins[sequence[show_idx]], 1);
                    add_alarm_in_ms(LED_ON_MS, cb_alarm, NULL, false);
                    break;

                case ST_SHOW_ON:
                    gpio_put(led_pins[sequence[show_idx]], 0);
                    show_idx++;
                    if (show_idx >= current_level) {
                        player_idx    = 0;
                        pressed_color = -1;
                        state         = ST_PLAYER;
                        timeout_id    = add_alarm_in_ms(TIMEOUT_MS, cb_alarm, NULL, false);
                    } else {
                        state = ST_SHOW_OFF;
                        add_alarm_in_ms(LED_OFF_MS, cb_alarm, NULL, false);
                    }
                    break;

                case ST_SHOW_OFF:
                    state = ST_SHOW_ON;
                    gpio_put(led_pins[sequence[show_idx]], 1);
                    add_alarm_in_ms(LED_ON_MS, cb_alarm, NULL, false);
                    break;

                case ST_PLAYER:
                    /* alarme no ST_PLAYER = timeout esgotado */
                    state       = ST_ERROR;
                    blink_count = 0;
                    all_leds(true);
                    add_alarm_in_ms(ERR_BLINK_MS, cb_alarm, NULL, false);
                    break;

                case ST_FEEDBACK:
                    gpio_put(led_pins[feedback_color], 0);
                    if (feedback_color != sequence[player_idx]) {
                        /* ── Cor errada ── */
                        state       = ST_ERROR;
                        blink_count = 0;
                        all_leds(true);
                        add_alarm_in_ms(ERR_BLINK_MS, cb_alarm, NULL, false);
                    } else {
                        player_idx++;
                        if (player_idx >= current_level) {
                            if (current_level >= SEQ_LEN) {
                                /* ── Vitória ── */
                                state       = ST_WIN;
                                blink_count = 0;
                                all_leds(true);
                                add_alarm_in_ms(WIN_BLINK_MS, cb_alarm, NULL, false);
                            } else {
                                /* ── Próximo nível ── */
                                current_level++;
                                state = ST_LEVEL_UP;
                                add_alarm_in_ms(LEVEL_UP_MS, cb_alarm, NULL, false);
                            }
                        } else {
                            /* ── Aguarda próxima cor ── */
                            state      = ST_PLAYER;
                            timeout_id = add_alarm_in_ms(TIMEOUT_MS, cb_alarm, NULL, false);
                        }
                    }
                    break;

                case ST_LEVEL_UP:
                    all_leds(false);
                    show_idx = 0;
                    state    = ST_SHOW_PRE;
                    add_alarm_in_ms(PRE_SHOW_MS, cb_alarm, NULL, false);
                    break;

                case ST_ERROR:
                    blink_count++;
                    if (blink_count >= ERR_BLINKS * 2) {
                        all_leds(false);
                        current_level = 1;
                        state         = ST_WAIT_START;
                    } else {
                        all_leds(blink_count % 2 == 0);
                        add_alarm_in_ms(ERR_BLINK_MS, cb_alarm, NULL, false);
                    }
                    break;

                case ST_WIN:
                    blink_count++;
                    if (blink_count >= WIN_BLINKS * 2) {
                        all_leds(false);
                        current_level = 1;
                        state         = ST_WAIT_START;
                    } else {
                        all_leds(blink_count % 2 == 0);
                        add_alarm_in_ms(WIN_BLINK_MS, cb_alarm, NULL, false);
                    }
                    break;

                default:
                    break;
            }
        }

        /* ── Botão de cor durante ST_PLAYER ── */
        if (state == ST_PLAYER && pressed_color >= 0) {
            uint32_t ints = save_and_disable_interrupts();

            if (state != ST_PLAYER) {
                pressed_color = -1;
                restore_interrupts(ints);
            } else {
                feedback_color = pressed_color;
                pressed_color  = -1;
                state          = ST_FEEDBACK;
                cancel_alarm(timeout_id);
                restore_interrupts(ints);

                gpio_put(led_pins[feedback_color], 1);
                add_alarm_in_ms(FEEDBACK_MS, cb_alarm, NULL, false);
            }
        }

        __wfi();
    }
}
