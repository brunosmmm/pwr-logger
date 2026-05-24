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
#define ADC_VFULL_SCALE_V        3.6f       /* internal 0.6V ref, gain 1/6     */
#define ADC_BITS                 12u
#define INA240_GAIN              50.0f      /* V/V (A2 variant)                */
#define INA240_VREF_V            1.65f      /* output bias (bidir = VDD/2)     */
#define SHUNT_OHMS               0.010f     /* sense resistor                  */

#endif /* APP_CONFIG_H_ */
