/*
 * wolfSSL declares wc_ecc_fp_free() in ecc.h, but builds the implementation
 * only when FP_ECC is enabled. websocket-client calls the cleanup hook after
 * certificate verification; without FP_ECC there is no fixed-point cache to
 * free, so this weak fallback keeps the optional API linkable.
 */
#if defined(__GNUC__) || defined(__clang__)
#define SC_WOLFSSL_WEAK __attribute__((weak))
#else
#define SC_WOLFSSL_WEAK
#endif

void wc_ecc_fp_free(void);

SC_WOLFSSL_WEAK
void wc_ecc_fp_free(void)
{
}

#undef SC_WOLFSSL_WEAK
