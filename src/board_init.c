/*
 * Early-init override for the promicro_nrf52840.
 *
 * Sets UICR.REGOUT0 to 3.3 V when the chip boots in HV mode (powered via
 * VDDH from USB or LiPo through REG0). The factory default is 1.8 V, which
 * means every GPIO output drives at most 1.8 V -- below the forward voltage
 * of red LEDs and below the logic-high threshold of 3.3 V peripherals on the
 * gated rail. Symptom is "the LED never lights and GPIO high reads 1.8 V."
 *
 * Same pattern as Zephyr's upstream nrf52840dongle board.c
 * (boards/nordic/nrf52840dongle/board.c) -- this file shouldn't exist in the
 * app at all, but the promicro_nrf52840 board doesn't ship its own board.c
 * so we provide the hook here.
 *
 * UICR is one-time-programmable per chip-erase, so this only does anything on
 * the first boot after a fresh erase; subsequent boots see REGOUT0 already
 * programmed and skip the write.
 */

#include <zephyr/init.h>
#include <hal/nrf_power.h>

void board_early_init_hook(void)
{
	if (nrf_power_mainregstatus_get(NRF_POWER) != NRF_POWER_MAINREGSTATUS_HIGH) {
		return;   /* not in HV mode -- regulator is bypassed, no action */
	}

	if ((NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk) !=
	    (UICR_REGOUT0_VOUT_DEFAULT << UICR_REGOUT0_VOUT_Pos)) {
		return;   /* already programmed by a previous boot */
	}

	/* Unlock UICR for write. */
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
	while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
		/* spin */
	}

	/* Program REGOUT0 = 3.3 V (matches our external 3V3 rail). */
	NRF_UICR->REGOUT0 =
	    (NRF_UICR->REGOUT0 & ~((uint32_t)UICR_REGOUT0_VOUT_Msk)) |
	    (UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos);

	/* Re-lock UICR. */
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
	while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
		/* spin */
	}

	/* Settings only take effect after a reset. */
	NVIC_SystemReset();
}
