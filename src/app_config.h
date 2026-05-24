#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

/* Central tunables. Front-end scaling defaults must be confirmed against the
 * actual hardware (shunt value, INA240 gain variant, SAADC ref/gain). */

/* ---- Time base & fast acquisition --------------------------------------- */
#define MASTER_CLOCK_HZ          1000000u   /* 1 MHz free-running master tick  */
#define ACQ_SAMPLE_RATE_HZ       200000u    /* INA240 -> SAADC                 */
/* master ticks per fast sample — exact integer by construction (= 5) */
#define ACQ_SAMPLE_PERIOD_TICKS  (MASTER_CLOCK_HZ / ACQ_SAMPLE_RATE_HZ)

#define ACQ_BUF_SAMPLES          8192u      /* int16 per DMA buffer (16 KB)    */
#define ACQ_BUF_COUNT            8u         /* pool depth (~128 KB, ~320 ms)   */

/* ---- Slow precision channel (INA228) ------------------------------------ */
#define SLOW_FLUSH_INTERVAL_MS   1000u      /* batch slow records into a TDMS  */
                                            /* segment ~1x/s                   */

/* ---- Front-end scaling: raw SAADC code -> amps -------------------------- */
/*   I = ((code/2^bits)*Vfull - Vref) / (gain * shunt)                       */
/*   ~15 A full scale: 0.2 V/A -> ~4.4 mA/LSB at 12-bit.                      */
#define ADC_VFULL_SCALE_V        3.6f       /* SAADC: internal 0.6V ref, gain 1/6 */
#define ADC_BITS                 12u
#define INA240_GAIN              200.0f     /* V/V (A4 variant)                */
#define INA240_VREF_V            0.0f       /* REF = GND (unidirectional)      */
#define INA240_SHUNT_OHMS        0.001f     /* INA240 board shunt (0.1 -> 1 mOhm swap) */

/* ---- INA228 precision channel ------------------------------------------- */
#define INA228_SHUNT_OHMS        0.002f     /* INA228 board shunt               */
#define INA228_IMAX_A            20.0f      /* expected max; sets CURRENT_LSB (hi-res) */

#endif /* APP_CONFIG_H_ */
