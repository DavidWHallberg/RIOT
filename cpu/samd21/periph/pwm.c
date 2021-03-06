/*
 * Copyright (C) 2014 Hamburg University of Applied Sciences
 *               2015 Freie Universität Berlin
 *
 * This file is subject to the terms and conditions of the GNU Lesser General
 * Public License v2.1. See the file LICENSE in the top level directory for more
 * details.
 */

/**
 * @ingroup     cpu_samd21
 * @ingroup     drivers_periph_pwm
 * @{
 *
 * @file
 * @brief       Low-level PWM driver implementation
 *
 * @author      Peter Kietzmann <peter.kietzmann@haw-hamburg.de>
 * @author      Hauke Petersen <hauke.petersen@fu-berlin.de>
 *
 * @}
 */

#include <stdint.h>
#include <string.h>

#include "log.h"
#include "cpu.h"
#include "board.h"
#include "periph/gpio.h"
#include "periph/pwm.h"

static inline Tcc *_tcc(pwm_t dev)
{
    return pwm_config[dev].dev;
}

static inline uint8_t _chan(pwm_t dev, int chan)
{
    return pwm_config[dev].chan[chan].chan;
}

static int _clk_id(pwm_t dev)
{
    Tcc *tcc = _tcc(dev);

    if (tcc == TCC0) {
        return TCC0_GCLK_ID;
    }

    if (tcc == TCC1) {
        return TCC1_GCLK_ID;
    }

    if (tcc == TCC2) {
        return TCC2_GCLK_ID;
    }
#ifdef TCC3
    if (tcc == TCC3) {
        return TCC3_GCLK_ID;
    }
#endif

    assert(0);
    return 0;
}

static uint32_t _apbcmask_tcc(pwm_t dev)
{
    Tcc *tcc = _tcc(dev);

    if (tcc == TCC0) {
        return PM_APBCMASK_TCC0;
    }

    if (tcc == TCC1) {
        return PM_APBCMASK_TCC1;
    }

    if (tcc == TCC2) {
        return PM_APBCMASK_TCC2;
    }
#ifdef TCC3
    if (tcc == TCC3) {
        return PM_APBCMASK_TCC3;
    }
#endif

    assert(0);
    return 0;
}

static uint8_t get_prescaler(unsigned int target, int *scale)
{
    if (target == 0) {
        return 0xff;
    }

    if (target >= 512) {
        *scale = 1024;
        return TCC_CTRLA_PRESCALER_DIV1024_Val;
    }
    if (target >= 128) {
        *scale = 256;
        return TCC_CTRLA_PRESCALER_DIV256_Val;
    }
    if (target >= 32) {
        *scale = 64;
        return TCC_CTRLA_PRESCALER_DIV64_Val;
    }
    if (target >= 12) {
        *scale = 16;
        return TCC_CTRLA_PRESCALER_DIV16_Val;
    }
    if (target >= 6) {
        *scale = 8;
        return TCC_CTRLA_PRESCALER_DIV8_Val;
    }
    if (target >= 3) {
        *scale = 4;
        return TCC_CTRLA_PRESCALER_DIV4_Val;
    }
    *scale = target;
    return target - 1;
}

static void poweron(pwm_t dev)
{
    PM->APBCMASK.reg |= _apbcmask_tcc(dev);
    GCLK->CLKCTRL.reg = (GCLK_CLKCTRL_CLKEN |
                         GCLK_CLKCTRL_GEN_GCLK0 |
                         GCLK_CLKCTRL_ID(_clk_id(dev)));
    while (GCLK->STATUS.bit.SYNCBUSY) {}
}

uint32_t pwm_init(pwm_t dev, pwm_mode_t mode, uint32_t freq, uint16_t res)
{
    uint8_t prescaler;
    int scale = 1;
    uint32_t f_real;

    if ((unsigned int)dev >= PWM_NUMOF) {
        return 0;
    }

    /* calculate the closest possible clock presacler */
    prescaler = get_prescaler(CLOCK_CORECLOCK / (freq * res), &scale);
    if (prescaler == 0xff) {
        return 0;
    }
    f_real = (CLOCK_CORECLOCK / (scale * res));

    /* configure the used pins */
    for (unsigned i = 0; i < pwm_config[dev].chan_numof; i++) {
        if (pwm_config[dev].chan[i].pin != GPIO_UNDEF) {
            gpio_init(pwm_config[dev].chan[i].pin, GPIO_OUT);
            gpio_init_mux(pwm_config[dev].chan[i].pin, pwm_config[dev].chan[i].mux);
        }
    }

    /* power on the device */
    poweron(dev);

    /* reset TCC module */
    _tcc(dev)->CTRLA.reg = TCC_CTRLA_SWRST;
    while (_tcc(dev)->SYNCBUSY.reg & TCC_SYNCBUSY_SWRST) {}
    /* set PWM mode */
    switch (mode) {
        case PWM_LEFT:
            _tcc(dev)->CTRLBCLR.reg = TCC_CTRLBCLR_DIR;     /* count up */
            break;
        case PWM_RIGHT:
            _tcc(dev)->CTRLBSET.reg = TCC_CTRLBSET_DIR;     /* count down */
            break;
        case PWM_CENTER:        /* currently not supported */
        default:
            return 0;
    }
    while (_tcc(dev)->SYNCBUSY.reg & TCC_SYNCBUSY_CTRLB) {}

    /* configure the TCC device */
    _tcc(dev)->CTRLA.reg = (TCC_CTRLA_PRESCSYNC_GCLK_Val
                            | TCC_CTRLA_PRESCALER(prescaler));
    /* select the waveform generation mode -> normal PWM */
    _tcc(dev)->WAVE.reg = (TCC_WAVE_WAVEGEN_NPWM);
    while (_tcc(dev)->SYNCBUSY.reg & TCC_SYNCBUSY_WAVE) {}
    /* set the selected period */
    _tcc(dev)->PER.reg = (res - 1);
    while (_tcc(dev)->SYNCBUSY.reg & TCC_SYNCBUSY_PER) {}
    /* start PWM operation */
    _tcc(dev)->CTRLA.reg |= (TCC_CTRLA_ENABLE);
    /* return the actual frequency the PWM is running at */
    return f_real;
}

uint8_t pwm_channels(pwm_t dev)
{
    return pwm_config[dev].chan_numof;
}

void pwm_set(pwm_t dev, uint8_t channel, uint16_t value)
{
    if ((channel >= pwm_config[dev].chan_numof) ||
        (pwm_config[dev].chan[channel].pin == GPIO_UNDEF)) {
        return;
    }

    uint8_t chan = _chan(dev, channel);
    if (chan < 4) {
        _tcc(dev)->CC[chan].reg = value;
        while (_tcc(dev)->SYNCBUSY.reg & (TCC_SYNCBUSY_CC0 << chan)) {}
    } else {
        chan -= 4;
        _tcc(dev)->CCB[chan].reg = value;
        while (_tcc(dev)->SYNCBUSY.reg & (TCC_SYNCBUSY_CCB0 << chan)) {}
    }
}

void pwm_poweron(pwm_t dev)
{
    poweron(dev);
    _tcc(dev)->CTRLA.reg |= (TCC_CTRLA_ENABLE);
}

void pwm_poweroff(pwm_t dev)
{
    _tcc(dev)->CTRLA.reg &= ~(TCC_CTRLA_ENABLE);

    PM->APBCMASK.reg &= ~_apbcmask_tcc(dev);
    GCLK->CLKCTRL.reg = (GCLK_CLKCTRL_GEN_GCLK7 |
                         GCLK_CLKCTRL_ID(_clk_id(dev)));
    while (GCLK->STATUS.bit.SYNCBUSY) {}
}
