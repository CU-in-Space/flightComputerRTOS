# debug/

`STM32H753.svd` describes every peripheral register on the chip: names, bit
fields, and what each field means. The VS Code debugger (cortex-debug) reads it
to fill the **XPERIPHERALS** view, so while stopped at a breakpoint you can open
`ADC1 → CR` and see `ADEN`, `ADSTART`, `BOOST`… by name instead of decoding hex.

Source: ST's STM32H7 SVD v2.8, Apache-2.0, via the
[modm-io/cmsis-svd-stm32](https://github.com/modm-io/cmsis-svd-stm32) mirror
(`stm32h7/STM32H753.svd`). To update, download the newer file over this one.
