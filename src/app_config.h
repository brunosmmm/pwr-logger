#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

/* Central tunables. Front-end scaling defaults must be confirmed against the
 * actual hardware (shunt value, INA240 gain variant, SAADC ref/gain). */

/* ---- Time base & fast acquisition --------------------------------------- */
#define MASTER_CLOCK_HZ          1000000u   /* 1 MHz free-running master tick  */
/* SAADC output (logged) rate. The SAADC hardware-oversamples ACQ_OVERSAMPLE
 * conversions per output sample (BURST mode) and DMAs the average, so the
 * internal conversion rate is ACQ_SAMPLE_RATE_HZ * ACQ_OVERSAMPLE and must stay
 * <= ~200 kSPS. 20 kHz * 8 = 160 kHz, OK. Oversampling at 14-bit retains the
 * sub-LSB resolution the averaging buys (~+1.5 bit), with zero CPU. */
#define ACQ_SAMPLE_RATE_HZ       20000u     /* 20 kSPS logged (INA240 -> SAADC) */
#define ACQ_OVERSAMPLE           8u         /* HW average 8 conv/sample        */
/* master ticks per output sample (= 50) */
#define ACQ_SAMPLE_PERIOD_TICKS  (MASTER_CLOCK_HZ / ACQ_SAMPLE_RATE_HZ)

#define ACQ_BUF_SAMPLES          8192u      /* int16 per DMA buffer; at 20 kSPS */
#define ACQ_BUF_COUNT            8u         /* = ~410 ms each, ~3.3 s of pool   */

/* ---- Slow precision channel (INA228) ------------------------------------ */
#define SLOW_FLUSH_INTERVAL_MS   1000u      /* batch slow records into a TDMS  */
                                            /* segment ~1x/s                   */

/* ---- Front-end scaling: raw SAADC code -> amps -------------------------- */
/*   I = ((code/2^bits)*Vfull - Vref) / (gain * shunt)                       */
/*   Bidirectional +/-~7.5 A around 1.65 V center; 0.2 V/A -> ~4.4 mA/LSB.     */
#define ADC_VFULL_SCALE_V        3.6f       /* SAADC: internal 0.6V ref, gain 1/6 */
#define ADC_BITS                 14u        /* 14-bit output (oversample retains sub-LSB) */
#define INA240_GAIN              200.0f     /* V/V (A4 variant)                */
#define INA240_VREF_V            1.65f      /* REF = VS/2 -> bidirectional (~1.65V idle); refine via calibration */
#define INA240_SHUNT_OHMS        0.001f     /* INA240 board shunt (0.1 -> 1 mOhm swap) */

/* ---- INA228 precision channel ------------------------------------------- */
#define INA228_SHUNT_OHMS        0.002f     /* INA228 board shunt               */
#define INA228_IMAX_A            20.0f      /* expected max; sets CURRENT_LSB (hi-res) */

#endif /* APP_CONFIG_H_ */
