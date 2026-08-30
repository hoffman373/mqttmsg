/**
 * @file keymanager.c
 * @brief Nonce generation from the RP2350 ring oscillator.
 *
 * Device-only: it reads a hardware register, so there is nothing here a
 * host test could exercise. mqttCheckKey() is in message_auth.c, where
 * there is.
 *
 * @see keymanager.h
 */

#include "hardware/regs/addressmap.h"
#include "hardware/regs/rosc.h"
#include <mqttmsg/keymanager.h>

/**
 * @brief Assembles a 32-bit random word from the ring oscillator.
 * @return One random word, built one bit per read of the ROSC random bit.
 */
static uint32_t rnd(void) {
    int k, random = 0;
    volatile uint32_t *rnd_reg = (uint32_t *)(ROSC_BASE + ROSC_RANDOMBIT_OFFSET);

    for (k = 0; k < 32; k++) {
        random = random << 1;
        random = random + (0x00000001 & (*rnd_reg));
    }

    return random;
}

void makeNonce(uint8_t *dest) {
    for (int i = 0; i < MQTTMSG_NONCE_BYTES; i++) {
        dest[i] = (rnd() % 256);
    }
}
