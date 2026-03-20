#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "tft_lcd_ili9341/gfx/gfx_ili9341.h"
#include "tft_lcd_ili9341/ili9341/ili9341.h"
#include "tft_lcd_ili9341/touch_resistive/touch_resistive.h"

#include "../image_bitmap2.h"

// Propriedades do LCD
#define SCREEN_ROTATION 1
#define SCREEN_W 320
#define SCREEN_H 240

// Pinos do motor de passos
#define FASE1_PIN 2
#define FASE2_PIN 3
#define FASE3_PIN 4
#define FASE4_PIN 5

// Posições dos botões na tela
#define CCW_BTN_X  20
#define CCW_BTN_Y  70
#define CCW_BTN_W  105
#define CCW_BTN_H  99

#define CW_BTN_X   200
#define CW_BTN_Y   65
#define CW_BTN_W   103
#define CW_BTN_H   110

// Centro da animação (entre os dois botões)
#define ANIM_X     155
#define ANIM_Y     120
#define ANIM_SIZE  20
#define ANIM_THICK 6

// Desenha os dois botões de seta na tela
void drawButtons(void) {
    gfx_drawBitmap(CW_BTN_X,  CW_BTN_Y,  image_horario_bits,       CW_BTN_W,  CW_BTN_H,  0x07E0);
    gfx_drawBitmap(CCW_BTN_X, CCW_BTN_Y, image_anti_horario_bits, CCW_BTN_W, CCW_BTN_H, 0x07E0);
}

// Limpa a área da animação
void clearAnim(void) {
    gfx_fillRect(ANIM_X - ANIM_SIZE - 2, ANIM_Y - ANIM_SIZE - 2,
                 (ANIM_SIZE + 2) * 2, (ANIM_SIZE + 2) * 2, 0x0000);
}

// Desenha um frame da animação (ponteiro girando)
// frame 0=cima, 1=direita, 2=baixo, 3=esquerda
void drawAnimFrame(int frame, uint16_t color) {
    clearAnim();
    int f = frame % 4;
    switch (f) {
        case 0:
            gfx_fillRect(ANIM_X - ANIM_THICK/2, ANIM_Y - ANIM_SIZE,
                         ANIM_THICK, ANIM_SIZE, color);
            break;
        case 1:
            gfx_fillRect(ANIM_X, ANIM_Y - ANIM_THICK/2,
                         ANIM_SIZE, ANIM_THICK, color);
            break;
        case 2:
            gfx_fillRect(ANIM_X - ANIM_THICK/2, ANIM_Y,
                         ANIM_THICK, ANIM_SIZE, color);
            break;
        case 3:
            gfx_fillRect(ANIM_X - ANIM_SIZE, ANIM_Y - ANIM_THICK/2,
                         ANIM_SIZE, ANIM_THICK, color);
            break;
    }
}

// Um passo do motor sentido horário
void stepMotorCW(void) {
    gpio_put(FASE1_PIN, 1);
    sleep_ms(2);
    gpio_put(FASE1_PIN, 0);
    gpio_put(FASE2_PIN, 1);
    sleep_ms(2);
    gpio_put(FASE2_PIN, 0);
    gpio_put(FASE3_PIN, 1);
    sleep_ms(2);
    gpio_put(FASE3_PIN, 0);
    gpio_put(FASE4_PIN, 1);
    sleep_ms(2);
    gpio_put(FASE4_PIN, 0);
}

// Um passo do motor sentido anti-horário
void stepMotorCCW(void) {
    gpio_put(FASE4_PIN, 1);
    sleep_ms(2);
    gpio_put(FASE4_PIN, 0);
    gpio_put(FASE3_PIN, 1);
    sleep_ms(2);
    gpio_put(FASE3_PIN, 0);
    gpio_put(FASE2_PIN, 1);
    sleep_ms(2);
    gpio_put(FASE2_PIN, 0);
    gpio_put(FASE1_PIN, 1);
    sleep_ms(2);
    gpio_put(FASE1_PIN, 0);
}

void stopMotor(void) {
    gpio_put(FASE1_PIN, 0);
    gpio_put(FASE2_PIN, 0);
    gpio_put(FASE3_PIN, 0);
    gpio_put(FASE4_PIN, 0);
}

// Roda o motor uma volta completa (512 passos) com animação
// direction: 1 = horário, -1 = anti-horário
void runMotorOneRevolution(int direction) {
    uint16_t color = (direction == 1) ? 0x07E0 : 0xF800;
    int animFrame = 0;

    for (int i = 0; i < 512; i++) {
        if (direction == 1)
            stepMotorCW();
        else
            stepMotorCCW();

        if (i % 32 == 0) {
            if (direction == 1)
                drawAnimFrame(animFrame, color);
            else
                drawAnimFrame(3 - animFrame, color);

            animFrame = (animFrame + 1) % 4;
        }
    }

    stopMotor();
    clearAnim();
}

int main() {
    stdio_init_all();

    //### LCD
    LCD_initDisplay();
    LCD_setRotation(SCREEN_ROTATION);

    //### TOUCH
    configure_touch();

    //### GFX
    gfx_init();
    gfx_clear();

    //### MOTOR
    gpio_init(FASE1_PIN);
    gpio_set_dir(FASE1_PIN, GPIO_OUT);
    gpio_init(FASE2_PIN);
    gpio_set_dir(FASE2_PIN, GPIO_OUT);
    gpio_init(FASE3_PIN);
    gpio_set_dir(FASE3_PIN, GPIO_OUT);
    gpio_init(FASE4_PIN);
    gpio_set_dir(FASE4_PIN, GPIO_OUT);

    //### TELA
    gfx_setTextSize(2);
    gfx_setTextColor(0x07E0);
    gfx_drawText(40, 10, "Motor de Passos");
    drawButtons();

    while (true) {
        int touchRawX, touchRawY;
        int screenTouchX = 0;
        int screenTouchY = 0;

        int touchDetected = readPoint(&touchRawX, &touchRawY);

        if (touchDetected) {
            gfx_touchTransform(SCREEN_ROTATION,
                               touchRawX, touchRawY,
                               &screenTouchX, &screenTouchY);

            // Checa se tocou no botão CW (seta da direita)
            if (screenTouchX >= CW_BTN_X && screenTouchX <= CW_BTN_X + CW_BTN_W &&
                screenTouchY >= CW_BTN_Y && screenTouchY <= CW_BTN_Y + CW_BTN_H) {
                runMotorOneRevolution(1);
            }

            // Checa se tocou no botão CCW (seta da esquerda)
            if (screenTouchX >= CCW_BTN_X && screenTouchX <= CCW_BTN_X + CCW_BTN_W &&
                screenTouchY >= CCW_BTN_Y && screenTouchY <= CCW_BTN_Y + CCW_BTN_H) {
                runMotorOneRevolution(-1);
            }
        }

        sleep_ms(10);
    }

    return 0;
}