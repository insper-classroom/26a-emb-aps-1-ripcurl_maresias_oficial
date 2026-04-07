/**
 * Genius (Simon Says) — Raspberry Pi Pico
 *
 * Máquina de estados sem sleep_ms.
 * Toda temporização via alarm callbacks.
 * Sequência aleatória gerada ao pressionar START.
 * Áudio PWM para cada cor, início e erro.
 *
 * Botão START (pino 27) inicia/reinicia o jogo.
 * Em idle todos os LEDs ficam apagados.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "pico/time.h"

#include "tft_lcd_ili9341/gfx/gfx_ili9341.h"
#include "tft_lcd_ili9341/ili9341/ili9341.h"

#include "img_circle_32.h"

/* ── Áudios ────────────────────────────────────────────────────────────────── */

#include "audio/amarelo.h"
#include "audio/azul.h"
#include "audio/verde.h"
#include "audio/vermelho.h"
#include "audio/falha.h"
#include "audio/inicio.h"

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

#define AUDIO_PIN  14

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

#define SEQ_LEN 100
static const int led_pins[4] = {LED_YELLOW, LED_BLUE, LED_GREEN, LED_RED};

/* ── PRNG simples (xorshift32) ─────────────────────────────────────────────── */

static void generate_sequence(int *seq) {
    uint32_t state = time_us_32();
    if (state == 0) state = 1;
    for (int i = 0; i < SEQ_LEN; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        seq[i] = (int)(state % 4);
    }
}

/* ── Áudio: identificadores ────────────────────────────────────────────────── */

// Índices para selecionar qual áudio tocar
#define AUDIO_NONE     -1
#define AUDIO_AMARELO   0
#define AUDIO_AZUL      1
#define AUDIO_VERDE     2
#define AUDIO_VERMELHO  3
#define AUDIO_FALHA     4
#define AUDIO_INICIO    5

// Tabelas de ponteiros e tamanhos dos áudios
static const unsigned char *audio_data[] = {
    AMARELO_DATA,
    AZUL_DATA,
    VERDE_DATA,
    VERMELHO_DATA,
    FALHA_DATA,
    INICIO_DATA,
};

static const unsigned int audio_length[] = {
    AMARELO_DATA_LENGTH,
    AZUL_DATA_LENGTH,
    VERDE_DATA_LENGTH,
    VERMELHO_DATA_LENGTH,
    FALHA_DATA_LENGTH,
    INICIO_DATA_LENGTH,
};

// Mapeamento cor -> áudio
static const int color_to_audio[] = {
    AUDIO_AMARELO,   // YELLOW = 0
    AUDIO_AZUL,      // BLUE   = 1
    AUDIO_VERDE,     // GREEN  = 2
    AUDIO_VERMELHO,  // RED    = 3
};

/* ── Variáveis de áudio (volatile: acessadas na IRQ PWM) ──────────────────── */

static struct {
    volatile int      id;        // qual áudio está tocando
    volatile uint32_t position;  // posição no áudio (com repetição 8x)
} s_audio = { .id = AUDIO_NONE, .position = 0 };

/* ── Função para iniciar um áudio ──────────────────────────────────────────── */

static void audio_play(int audio_id) {
    s_audio.position = 0;
    s_audio.id       = audio_id;
}

static void audio_stop(void) {
    s_audio.id       = AUDIO_NONE;
    s_audio.position = 0;
    pwm_set_gpio_level(AUDIO_PIN, 0);
}

/* ── Handler PWM: reproduz o áudio selecionado ─────────────────────────────── */

void pwm_interrupt_handler(void) {
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_PIN));

    int id = s_audio.id;
    if (id == AUDIO_NONE) {
        pwm_set_gpio_level(AUDIO_PIN, 0);
        return;
    }

    uint32_t total = audio_length[id] << 3;  // cada amostra repete 8 ciclos
    if (s_audio.position < total - 1) {
        pwm_set_gpio_level(AUDIO_PIN, audio_data[id][s_audio.position >> 3]);
        s_audio.position++;
    } else {
        // áudio terminou: silencia (não faz loop)
        s_audio.id = AUDIO_NONE;
        pwm_set_gpio_level(AUDIO_PIN, 0);
    }
}

/* ── Variáveis de estado (volatile: compartilhadas com IRQ/callbacks) ──────── */

static volatile state_t    state          = ST_IDLE;
static volatile int        current_level  = 1;
static volatile int        show_idx       = 0;
static volatile int        player_idx     = 0;
static volatile int        pressed_color  = -1;
static volatile int        feedback_color = -1;
static volatile int        blink_count    = 0;
static volatile bool       start_pressed  = false;
static volatile alarm_id_t timeout_id     = 0;
static volatile uint32_t   last_btn_us    = 0;

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void all_leds(bool on) {
    for (int i = 0; i < 4; i++)
        gpio_put(led_pins[i], on);
}

/* ── Callback de alarme — apenas sinaliza o main ───────────────────────────── */

static int64_t cb_alarm(alarm_id_t id, void *data) {
    *(volatile bool *)data = true;
    return 0;
}

/* ── IRQ: botões — apenas seta variáveis de ativação ───────────────────────── */

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

/* ── Setup ─────────────────────────────────────────────────────────────────── */

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

    /* ── Configura PWM para saída de áudio no AUDIO_PIN ── */
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
    int audio_pin_slice = pwm_gpio_to_slice_num(AUDIO_PIN);

    pwm_clear_irq(audio_pin_slice);
    pwm_set_irq_enabled(audio_pin_slice, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 8.0f);   // 176MHz / 250 / 8 = 88kHz / 8 = 11kHz
    pwm_config_set_wrap(&config, 250);
    pwm_init(audio_pin_slice, &config, true);

    pwm_set_gpio_level(AUDIO_PIN, 0);
}

/* ── Main — máquina de estados completa ────────────────────────────────────── */

int main(void) {
    stdio_init_all();
    set_sys_clock_khz(176000, true);  // clock base para o PWM de áudio
    setup();

    int           sequence[SEQ_LEN];
    volatile bool alarm_fired = false;

    while (true) {

        /* ── START pressionado ── */
        if (start_pressed) {
            start_pressed = false;
            generate_sequence(sequence);
            current_level = 1;
            all_leds(false);
            audio_play(AUDIO_INICIO);   // toca som de início
            show_idx = 0;
            state    = ST_SHOW_PRE;
            add_alarm_in_ms(PRE_SHOW_MS, cb_alarm, (void*)&alarm_fired, false);
        }

        /* ── Alarme disparou: processa estado atual ── */
        if (alarm_fired) {
            alarm_fired = false;

            switch (state) {

                case ST_SHOW_PRE:
                    state = ST_SHOW_ON;
                    gpio_put(led_pins[sequence[show_idx]], 1);
                    audio_play(color_to_audio[sequence[show_idx]]);  // som da cor
                    add_alarm_in_ms(LED_ON_MS, cb_alarm, (void*)&alarm_fired, false);
                    break;

                case ST_SHOW_ON:
                    gpio_put(led_pins[sequence[show_idx]], 0);
                    audio_stop();
                    show_idx++;
                    if (show_idx >= current_level) {
                        player_idx    = 0;
                        pressed_color = -1;
                        state         = ST_PLAYER;
                        timeout_id    = add_alarm_in_ms(TIMEOUT_MS, cb_alarm, NULL, false);
                    } else {
                        state = ST_SHOW_OFF;
                        add_alarm_in_ms(LED_OFF_MS, cb_alarm, (void*)&alarm_fired, false);
                    }
                    break;

                case ST_SHOW_OFF:
                    state = ST_SHOW_ON;
                    gpio_put(led_pins[sequence[show_idx]], 1);
                    audio_play(color_to_audio[sequence[show_idx]]);  // som da cor
                    add_alarm_in_ms(LED_ON_MS, cb_alarm, (void*)&alarm_fired, false);
                    break;

                case ST_PLAYER:
                    /* timeout esgotado */
                    audio_play(AUDIO_FALHA);
                    state       = ST_ERROR;
                    blink_count = 0;
                    all_leds(true);
                    add_alarm_in_ms(ERR_BLINK_MS, cb_alarm, (void*)&alarm_fired, false);
                    break;

                case ST_FEEDBACK:
                    gpio_put(led_pins[feedback_color], 0);
                    audio_stop();
                    if (feedback_color != sequence[player_idx]) {
                        /* ── Cor errada ── */
                        audio_play(AUDIO_FALHA);
                        state       = ST_ERROR;
                        blink_count = 0;
                        all_leds(true);
                        add_alarm_in_ms(ERR_BLINK_MS, cb_alarm, (void*)&alarm_fired, false);
                    } else {
                        player_idx++;
                        if (player_idx >= current_level) {
                            if (current_level >= SEQ_LEN) {
                                /* ── Vitória ── */
                                audio_play(AUDIO_INICIO);  // som de vitória (reutiliza inicio)
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
                            timeout_id = add_alarm_in_ms(TIMEOUT_MS, cb_alarm, (void*)&alarm_fired, false);
                        }
                    }
                    break;

                case ST_LEVEL_UP:
                    all_leds(false);
                    show_idx = 0;
                    state    = ST_SHOW_PRE;
                    add_alarm_in_ms(PRE_SHOW_MS, cb_alarm, (void*)&alarm_fired, false);
                    break;

                case ST_ERROR:
                    blink_count++;
                    if (blink_count >= ERR_BLINKS * 2) {
                        all_leds(false);
                        audio_stop();
                        current_level = 1;
                        state         = ST_WAIT_START;
                    } else {
                        all_leds(blink_count % 2 == 0);
                        add_alarm_in_ms(ERR_BLINK_MS, cb_alarm, (void*)&alarm_fired, false);
                    }
                    break;

                case ST_WIN:
                    blink_count++;
                    if (blink_count >= WIN_BLINKS * 2) {
                        all_leds(false);
                        audio_stop();
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
                audio_play(color_to_audio[feedback_color]);  // som da cor pressionada
                add_alarm_in_ms(FEEDBACK_MS, cb_alarm, NULL, false);
            }
        }

        __wfi();
    }
}