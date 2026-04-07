/**
 * Genius (Simon Says) — Raspberry Pi Pico
 *
 * Solo e Dupla. Áudio PWM + ILI9341 landscape.
 * Sem sleep_ms — toda temporização via alarm callbacks.
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

#define AUDIO_PIN     14
#define LCD_BACKLIGHT 15

/* ── Tela (landscape) ──────────────────────────────────────────────────────── */

#define SCREEN_W 320
#define SCREEN_H 240

/* ── Cores LCD ─────────────────────────────────────────────────────────────── */

#define COLOR_BLACK   ILI9341_BLACK
#define COLOR_WHITE   ILI9341_WHITE
#define COLOR_RED     ILI9341_RED
#define COLOR_GREEN   ILI9341_GREEN
#define COLOR_BLUE    ILI9341_BLUE
#define COLOR_YELLOW  ILI9341_YELLOW
#define COLOR_CYAN    ILI9341_CYAN
#define COLOR_ORANGE  ILI9341_ORANGE

/* ── Cores (índices do jogo) ───────────────────────────────────────────────── */

#define YELLOW 0
#define BLUE   1
#define GREEN  2
#define RED    3

/* ── Modos de jogo ─────────────────────────────────────────────────────────── */

#define MODE_SOLO 0
#define MODE_DUO  1

/* ── Estados ───────────────────────────────────────────────────────────────── */

typedef enum {
    ST_IDLE,
    ST_SELECT_MODE,
    ST_SHOW_PRE,
    ST_SHOW_ON,
    ST_SHOW_OFF,
    ST_PLAYER,
    ST_FEEDBACK,
    ST_LEVEL_UP,
    ST_SWITCH_PLAYER,
    ST_ERROR,
    ST_WAIT_START,
    ST_WIN,
} state_t;

/* ── Tempos (ms) ───────────────────────────────────────────────────────────── */

#define PRE_SHOW_MS    1000
#define LED_ON_MS       600
#define LED_OFF_MS      250
#define TIMEOUT_MS     5000
#define FEEDBACK_MS     300
#define LEVEL_UP_MS     800
#define SWITCH_MS      1200
#define ERR_BLINK_MS    150
#define ERR_BLINKS        5
#define WIN_BLINK_MS    200
#define WIN_BLINKS        4

/* ── Sequência ─────────────────────────────────────────────────────────────── */

#define SEQ_LEN 100
static const int led_pins[4] = {LED_YELLOW, LED_BLUE, LED_GREEN, LED_RED};

static void generate_sequence(int *seq, uint32_t seed) {
    if (seed == 0) seed = 1;
    for (int i = 0; i < SEQ_LEN; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        seq[i] = (int)(seed % 4);
    }
}

/* ── Áudio ─────────────────────────────────────────────────────────────────── */

#define AUDIO_NONE     -1
#define AUDIO_AMARELO   0
#define AUDIO_AZUL      1
#define AUDIO_VERDE     2
#define AUDIO_VERMELHO  3
#define AUDIO_FALHA     4
#define AUDIO_INICIO    5

static const unsigned char *audio_data[] = {
    AMARELO_DATA, AZUL_DATA, VERDE_DATA, VERMELHO_DATA, FALHA_DATA, INICIO_DATA,
};
static const unsigned int audio_length[] = {
    AMARELO_DATA_LENGTH, AZUL_DATA_LENGTH, VERDE_DATA_LENGTH,
    VERMELHO_DATA_LENGTH, FALHA_DATA_LENGTH, INICIO_DATA_LENGTH,
};
static const int color_to_audio[] = {
    AUDIO_AMARELO, AUDIO_AZUL, AUDIO_VERDE, AUDIO_VERMELHO,
};

static struct {
    volatile int      id;
    volatile uint32_t position;
} s_audio = { .id = AUDIO_NONE, .position = 0 };

static void audio_play(int audio_id) { s_audio.position = 0; s_audio.id = audio_id; }
static void audio_stop(void) { s_audio.id = AUDIO_NONE; s_audio.position = 0; pwm_set_gpio_level(AUDIO_PIN, 0); }

// IRQ PWM — reproduz áudio
void pwm_interrupt_handler(void) {
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_PIN));
    int id = s_audio.id;
    if (id == AUDIO_NONE) { pwm_set_gpio_level(AUDIO_PIN, 0); return; }
    uint32_t total = audio_length[id] << 3;
    if (s_audio.position < total - 1) {
        pwm_set_gpio_level(AUDIO_PIN, audio_data[id][s_audio.position >> 3]);
        s_audio.position++;
    } else { s_audio.id = AUDIO_NONE; pwm_set_gpio_level(AUDIO_PIN, 0); }
}

/* ── Variáveis globais — somente as compartilhadas com IRQ/callbacks ───────── */

// Botão pressionado (gravado pela IRQ, consumido pelo main)
volatile int       btn_event      = -1;
volatile uint32_t  last_btn_us    = 0;

// Alarme (gravado pelo callback, consumido pelo main)
volatile bool      alarm_fired    = false;

// Feedback e timeout (usados entre main e IRQ/alarm)
volatile int       feedback_color = -1;
volatile alarm_id_t timeout_id    = 0;

/* ── Variáveis de jogo (globais, compartilhadas com IRQ indiretamente) ───── */

static int  game_mode       = MODE_SOLO;
static bool player_alive[2] = {true, true};
static int  player_score[2] = {0, 0};
static int  player_idx      = 0;
static int  sequence[2][SEQ_LEN];

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void all_leds(bool on) {
    for (int i = 0; i < 4; i++) gpio_put(led_pins[i], on);
}

static int64_t cb_alarm(alarm_id_t id, void *data) {
    *(volatile bool *)data = true;
    return 0;
}

static int find_next_untried_alive(const bool *tried) {
    for (int i = 0; i < 2; i++)
        if (player_alive[i] && !tried[i]) return i;
    return -1;
}

static bool any_alive(void) {
    return player_alive[0] || player_alive[1];
}

/* ── IRQ: botão — apenas grava qual GPIO foi pressionado ───────────────────── */

static void btn_callback(uint gpio, uint32_t events) {
    uint32_t now = time_us_32();
    if (now - last_btn_us < 200000) return;
    last_btn_us = now;
    if (!(events & GPIO_IRQ_EDGE_FALL)) return;
    btn_event = (int)gpio;
}

/* ── LCD helpers (320×240 landscape) ───────────────────────────────────────── */

static void lcd_clear(void) {
    gfx_fillRect(0, 0, SCREEN_W, SCREEN_H, COLOR_BLACK);
}

static void lcd_show_idle(void) {
    lcd_clear();
    gfx_setTextSize(4);
    gfx_setTextColor(COLOR_CYAN);
    gfx_setCursor(88, 60);
    gfx_print("GENIUS");
    gfx_setTextSize(1);
    gfx_setTextColor(COLOR_WHITE);
    gfx_setCursor(60, 140);
    gfx_print("Aperte o botao preto");
    gfx_setCursor(85, 160);
    gfx_print("para comecar!");
}

static void lcd_show_select_mode(void) {
    lcd_clear();
    gfx_setTextSize(2);
    gfx_setTextColor(COLOR_WHITE);
    gfx_setCursor(40, 30);
    gfx_print("Escolha o modo:");
    gfx_setTextColor(COLOR_RED);
    gfx_setCursor(40, 90);
    gfx_print("Vermelho: SOLO");
    gfx_setTextColor(COLOR_BLUE);
    gfx_setCursor(40, 140);
    gfx_print("Azul: DUPLA");
}

static void lcd_show_score_solo(int level) {
    lcd_clear();
    gfx_setTextSize(2);
    gfx_setTextColor(COLOR_WHITE);
    gfx_setCursor(90, 40);
    gfx_print("Pontuacao:");
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", level - 1);
    gfx_setTextSize(5);
    gfx_setTextColor(COLOR_GREEN);
    gfx_setCursor(140, 100);
    gfx_print(buf);
}

static void lcd_show_score_duo(int level, int player) {
    lcd_clear();
    uint16_t color = (player == 0) ? COLOR_CYAN : COLOR_ORANGE;
    gfx_setTextSize(2);
    gfx_setTextColor(color);
    char header[24];
    snprintf(header, sizeof(header), "Vez: Jogador %d", player + 1);
    gfx_setCursor(50, 20);
    gfx_print(header);
    gfx_setTextColor(COLOR_WHITE);
    gfx_setCursor(90, 70);
    gfx_print("Pontuacao:");
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", level - 1);
    gfx_setTextSize(4);
    gfx_setTextColor(COLOR_GREEN);
    gfx_setCursor(135, 110);
    gfx_print(buf);
    gfx_setTextSize(1);
    gfx_setTextColor(player_alive[0] ? COLOR_CYAN : COLOR_RED);
    gfx_setCursor(30, 200);
    gfx_print(player_alive[0] ? "J1: Vivo" : "J1: Eliminado");
    gfx_setTextColor(player_alive[1] ? COLOR_ORANGE : COLOR_RED);
    gfx_setCursor(200, 200);
    gfx_print(player_alive[1] ? "J2: Vivo" : "J2: Eliminado");
}

static void lcd_show_turn(int player) {
    lcd_clear();
    uint16_t color = (player == 0) ? COLOR_CYAN : COLOR_ORANGE;
    gfx_setTextSize(3);
    gfx_setTextColor(color);
    char buf[24];
    snprintf(buf, sizeof(buf), "Jogador %d", player + 1);
    gfx_setCursor(55, 70);
    gfx_print(buf);
    gfx_setTextSize(2);
    gfx_setTextColor(COLOR_WHITE);
    gfx_setCursor(85, 130);
    gfx_print("Sua vez!");
}

static void lcd_show_error_solo(int final_score) {
    lcd_clear();
    gfx_setTextSize(3);
    gfx_setTextColor(COLOR_RED);
    gfx_setCursor(100, 40);
    gfx_print("ERROU!");
    char buf[16];
    snprintf(buf, sizeof(buf), "Score: %d", final_score);
    gfx_setTextSize(2);
    gfx_setTextColor(COLOR_WHITE);
    gfx_setCursor(80, 100);
    gfx_print(buf);
    gfx_setTextSize(1);
    gfx_setCursor(70, 170);
    gfx_print("Aperte START para");
    gfx_setCursor(80, 190);
    gfx_print("jogar novamente");
}

static void lcd_show_error_duo_player(int player, int score) {
    lcd_clear();
    uint16_t color = (player == 0) ? COLOR_CYAN : COLOR_ORANGE;
    gfx_setTextSize(2);
    gfx_setTextColor(color);
    char buf[24];
    snprintf(buf, sizeof(buf), "Jogador %d", player + 1);
    gfx_setCursor(70, 40);
    gfx_print(buf);
    gfx_setTextSize(3);
    gfx_setTextColor(COLOR_RED);
    gfx_setCursor(85, 80);
    gfx_print("ERROU!");
    snprintf(buf, sizeof(buf), "Score: %d", score);
    gfx_setTextSize(2);
    gfx_setTextColor(COLOR_WHITE);
    gfx_setCursor(80, 140);
    gfx_print(buf);
}

static void lcd_show_gameover_duo(void) {
    lcd_clear();
    gfx_setTextSize(2);
    gfx_setTextColor(COLOR_WHITE);
    gfx_setCursor(60, 20);
    gfx_print("Fim de Jogo!");
    char buf[24];
    gfx_setTextColor(COLOR_CYAN);
    snprintf(buf, sizeof(buf), "J1: %d pts", player_score[0]);
    gfx_setCursor(70, 70);
    gfx_print(buf);
    gfx_setTextColor(COLOR_ORANGE);
    snprintf(buf, sizeof(buf), "J2: %d pts", player_score[1]);
    gfx_setCursor(70, 110);
    gfx_print(buf);
    gfx_setTextSize(2);
    gfx_setCursor(40, 160);
    if (player_score[0] > player_score[1]) {
        gfx_setTextColor(COLOR_CYAN);
        gfx_print("Jogador 1 venceu!");
    } else if (player_score[1] > player_score[0]) {
        gfx_setTextColor(COLOR_ORANGE);
        gfx_print("Jogador 2 venceu!");
    } else {
        gfx_setTextColor(COLOR_YELLOW);
        gfx_print("  EMPATE!");
    }
    gfx_setTextSize(1);
    gfx_setTextColor(COLOR_WHITE);
    gfx_setCursor(70, 210);
    gfx_print("Aperte START para");
    gfx_setCursor(80, 225);
    gfx_print("jogar novamente");
}

static void lcd_show_win_solo(void) {
    lcd_clear();
    gfx_setTextSize(3);
    gfx_setTextColor(COLOR_YELLOW);
    gfx_setCursor(70, 60);
    gfx_print("VITORIA!");
    gfx_setTextSize(2);
    gfx_setTextColor(COLOR_WHITE);
    gfx_setCursor(80, 120);
    gfx_print("Parabens!");
    gfx_setTextSize(1);
    gfx_setCursor(70, 180);
    gfx_print("Aperte START para");
    gfx_setCursor(80, 200);
    gfx_print("jogar novamente");
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

    /* PWM áudio */
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
    int slice = pwm_gpio_to_slice_num(AUDIO_PIN);
    pwm_clear_irq(slice);
    pwm_set_irq_enabled(slice, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 8.0f);
    pwm_config_set_wrap(&config, 250);
    pwm_init(slice, &config, true);
    pwm_set_gpio_level(AUDIO_PIN, 0);

    /* LCD */
    LCD_setPins(22, 17, 16, 18, 19);
    LCD_setSPIperiph(spi0);
    LCD_initDisplay();
    LCD_setRotation(1);
    gpio_init(LCD_BACKLIGHT);
    gpio_set_dir(LCD_BACKLIGHT, GPIO_OUT);
    gpio_put(LCD_BACKLIGHT, 1);
    gfx_init();
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void) {
    stdio_init_all();
    set_sys_clock_khz(176000, true);
    setup();
    lcd_show_idle();

    // Variáveis locais do jogo (não precisam ser globais)
    state_t state              = ST_IDLE;
    int     current_level      = 1;
    int     show_idx           = 0;
    int     blink_count        = 0;
    int     current_player     = 0;
    bool    player_tried[2]    = {false, false};
    int     last_displayed_level = -1;

    while (true) {

        /* ══════════════════════════════════════════════════════════════════ *
         *  1. Processa alarmes (prioridade sobre botões)                    *
         * ══════════════════════════════════════════════════════════════════ */
        if (alarm_fired) {
            alarm_fired = false;

            switch (state) {

                case ST_SWITCH_PLAYER:
                    // Inicia sequência para o jogador atual
                    player_tried[current_player] = true;
                    show_idx = 0;
                    state    = ST_SHOW_PRE;
                    if (game_mode == MODE_DUO)
                        lcd_show_score_duo(current_level, current_player);
                    else
                        lcd_show_score_solo(current_level);
                    last_displayed_level = current_level;
                    add_alarm_in_ms(PRE_SHOW_MS, cb_alarm, (void*)&alarm_fired, false);
                    break;

                case ST_SHOW_PRE:
                    state = ST_SHOW_ON;
                    gpio_put(led_pins[sequence[current_player][show_idx]], 1);
                    audio_play(color_to_audio[sequence[current_player][show_idx]]);
                    add_alarm_in_ms(LED_ON_MS, cb_alarm, (void*)&alarm_fired, false);
                    break;

                case ST_SHOW_ON:
                    gpio_put(led_pins[sequence[current_player][show_idx]], 0);
                    audio_stop();
                    show_idx++;
                    if (show_idx >= current_level) {
                        player_idx = 0;
                        state      = ST_PLAYER;
                        timeout_id = add_alarm_in_ms(TIMEOUT_MS, cb_alarm, (void*)&alarm_fired, false);
                    } else {
                        state = ST_SHOW_OFF;
                        add_alarm_in_ms(LED_OFF_MS, cb_alarm, (void*)&alarm_fired, false);
                    }
                    break;

                case ST_SHOW_OFF:
                    state = ST_SHOW_ON;
                    gpio_put(led_pins[sequence[current_player][show_idx]], 1);
                    audio_play(color_to_audio[sequence[current_player][show_idx]]);
                    add_alarm_in_ms(LED_ON_MS, cb_alarm, (void*)&alarm_fired, false);
                    break;

                case ST_PLAYER:
                    /* timeout = erro */
                    audio_play(AUDIO_FALHA);
                    player_alive[current_player] = false;
                    player_score[current_player] = current_level - 1;
                    if (game_mode == MODE_DUO)
                        lcd_show_error_duo_player(current_player, player_score[current_player]);
                    else
                        lcd_show_error_solo(current_level - 1);
                    state = ST_ERROR; blink_count = 0; all_leds(true);
                    add_alarm_in_ms(ERR_BLINK_MS, cb_alarm, (void*)&alarm_fired, false);
                    break;

                case ST_FEEDBACK:
                    gpio_put(led_pins[feedback_color], 0);
                    audio_stop();
                    if (feedback_color != sequence[current_player][player_idx]) {
                        /* ── Cor errada ── */
                        audio_play(AUDIO_FALHA);
                        player_alive[current_player] = false;
                        player_score[current_player] = current_level - 1;
                        if (game_mode == MODE_DUO)
                            lcd_show_error_duo_player(current_player, player_score[current_player]);
                        else
                            lcd_show_error_solo(current_level - 1);
                        state = ST_ERROR; blink_count = 0; all_leds(true);
                        add_alarm_in_ms(ERR_BLINK_MS, cb_alarm, (void*)&alarm_fired, false);
                    } else {
                        player_idx++;
                        if (player_idx >= current_level) {
                            /* Jogador completou o nível */
                            player_score[current_player] = current_level;

                            if (game_mode == MODE_SOLO) {
                                if (current_level >= SEQ_LEN) {
                                    audio_play(AUDIO_INICIO);
                                    lcd_show_win_solo();
                                    state = ST_WIN; blink_count = 0; all_leds(true);
                                    add_alarm_in_ms(WIN_BLINK_MS, cb_alarm, (void*)&alarm_fired, false);
                                } else {
                                    current_level++;
                                    state = ST_LEVEL_UP;
                                    add_alarm_in_ms(LEVEL_UP_MS, cb_alarm, (void*)&alarm_fired, false);
                                }
                            } else {
                                /* Duo: verifica se outro jogador precisa jogar */
                                int next = find_next_untried_alive(player_tried);
                                if (next >= 0) {
                                    current_player = next;
                                    lcd_show_turn(current_player);
                                    state = ST_SWITCH_PLAYER;
                                    add_alarm_in_ms(SWITCH_MS, cb_alarm, (void*)&alarm_fired, false);
                                } else if (current_level >= SEQ_LEN) {
                                    audio_play(AUDIO_INICIO);
                                    lcd_show_win_solo();
                                    state = ST_WIN; blink_count = 0; all_leds(true);
                                    add_alarm_in_ms(WIN_BLINK_MS, cb_alarm, (void*)&alarm_fired, false);
                                } else {
                                    current_level++;
                                    state = ST_LEVEL_UP;
                                    add_alarm_in_ms(LEVEL_UP_MS, cb_alarm, (void*)&alarm_fired, false);
                                }
                            }
                        } else {
                            state      = ST_PLAYER;
                            timeout_id = add_alarm_in_ms(TIMEOUT_MS, cb_alarm, (void*)&alarm_fired, false);
                        }
                    }
                    break;

                case ST_LEVEL_UP:
                    all_leds(false);
                    player_tried[0] = false;
                    player_tried[1] = false;

                    if (game_mode == MODE_DUO) {
                        int first = find_next_untried_alive(player_tried);
                        if (first >= 0) {
                            current_player = first;
                            lcd_show_turn(current_player);
                            state = ST_SWITCH_PLAYER;
                            add_alarm_in_ms(SWITCH_MS, cb_alarm, (void*)&alarm_fired, false);
                        }
                    } else {
                        // Solo: inicia sequência direto
                        player_tried[current_player] = true;
                        show_idx = 0;
                        state    = ST_SHOW_PRE;
                        lcd_show_score_solo(current_level);
                        last_displayed_level = current_level;
                        add_alarm_in_ms(PRE_SHOW_MS, cb_alarm, (void*)&alarm_fired, false);
                    }
                    break;

                case ST_ERROR:
                    blink_count++;
                    if (blink_count >= ERR_BLINKS * 2) {
                        all_leds(false);
                        audio_stop();

                        if (game_mode == MODE_SOLO) {
                            current_level = 1;
                            state = ST_WAIT_START;
                        } else {
                            int next = find_next_untried_alive(player_tried);
                            if (next >= 0) {
                                current_player = next;
                                lcd_show_turn(current_player);
                                state = ST_SWITCH_PLAYER;
                                add_alarm_in_ms(SWITCH_MS, cb_alarm, (void*)&alarm_fired, false);
                            } else if (any_alive()) {
                                current_level++;
                                state = ST_LEVEL_UP;
                                add_alarm_in_ms(LEVEL_UP_MS, cb_alarm, (void*)&alarm_fired, false);
                            } else {
                                lcd_show_gameover_duo();
                                state = ST_WAIT_START;
                            }
                        }
                    } else {
                        all_leds(blink_count % 2 == 0);
                        add_alarm_in_ms(ERR_BLINK_MS, cb_alarm, (void*)&alarm_fired, false);
                    }
                    break;

                case ST_WIN:
                    blink_count++;
                    if (blink_count >= WIN_BLINKS * 2) {
                        all_leds(false); audio_stop();
                        current_level = 1;
                        state = ST_WAIT_START;
                    } else {
                        all_leds(blink_count % 2 == 0);
                        add_alarm_in_ms(WIN_BLINK_MS, cb_alarm, (void*)&alarm_fired, false);
                    }
                    break;

                default:
                    break;
            }
        }

        /* ══════════════════════════════════════════════════════════════════ *
         *  2. Processa botão (após alarmes)                                 *
         * ══════════════════════════════════════════════════════════════════ */
        int btn = btn_event;
        if (btn >= 0) {
            btn_event = -1;

            /* START → vai para seleção de modo */
            if (btn == BTN_START && (state == ST_IDLE || state == ST_WAIT_START)) {
                state = ST_SELECT_MODE;
                lcd_show_select_mode();
            }

            /* Seleção de modo */
            else if (state == ST_SELECT_MODE &&
                     (btn == BTN_RED || btn == BTN_BLUE)) {
                game_mode = (btn == BTN_RED) ? MODE_SOLO : MODE_DUO;
                uint32_t seed = time_us_32();
                generate_sequence(sequence[0], seed);
                generate_sequence(sequence[1], seed ^ 0xDEADBEEF);

                current_level  = 1;
                current_player = 0;
                player_alive[0] = true;
                player_alive[1] = true;
                player_score[0] = 0;
                player_score[1] = 0;
                player_tried[0] = false;
                player_tried[1] = false;
                all_leds(false);
                audio_play(AUDIO_INICIO);
                last_displayed_level = -1;

                if (game_mode == MODE_DUO) {
                    lcd_show_turn(0);
                    state = ST_SWITCH_PLAYER;
                    add_alarm_in_ms(SWITCH_MS, cb_alarm, (void*)&alarm_fired, false);
                } else {
                    player_tried[0] = true;
                    show_idx = 0;
                    state    = ST_SHOW_PRE;
                    lcd_show_score_solo(current_level);
                    last_displayed_level = current_level;
                    add_alarm_in_ms(PRE_SHOW_MS, cb_alarm, (void*)&alarm_fired, false);
                }
            }

            /* Cor durante ST_PLAYER */
            else if (state == ST_PLAYER) {
                int color = -1;
                switch (btn) {
                    case BTN_YELLOW: color = YELLOW; break;
                    case BTN_BLUE:   color = BLUE;   break;
                    case BTN_GREEN:  color = GREEN;  break;
                    case BTN_RED:    color = RED;     break;
                }
                if (color >= 0) {
                    uint32_t ints = save_and_disable_interrupts();
                    feedback_color = color;
                    state          = ST_FEEDBACK;
                    cancel_alarm(timeout_id);
                    restore_interrupts(ints);

                    gpio_put(led_pins[feedback_color], 1);
                    audio_play(color_to_audio[feedback_color]);
                    add_alarm_in_ms(FEEDBACK_MS, cb_alarm, (void*)&alarm_fired, false);
                }
            }
        }

        /* ══════════════════════════════════════════════════════════════════ *
         *  3. Atualiza LCD quando nível muda (solo)                         *
         * ══════════════════════════════════════════════════════════════════ */
        if (game_mode == MODE_SOLO &&
            state >= ST_SHOW_PRE && state <= ST_LEVEL_UP &&
            current_level != last_displayed_level) {
            lcd_show_score_solo(current_level);
            last_displayed_level = current_level;
        }

        __wfi();
    }
}