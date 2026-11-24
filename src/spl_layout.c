/*
 * Reads psp2spl's own secure-world layout out of its global root, instead of
 * duplicating the per-firmware constants in main.c.
 *
 * psp2spl resolves these during SPLx_TZS_INIT: the TZ region's physical base
 * and length, the offsets of the two secure TTBRs within it, and mbase, the
 * non-secure window it mapped over the region (SPLi_PALLOC'd, so it moves
 * every boot). They live in struct tzs_layout_s, which splib's own SVA2PA and
 * PRINTTBR walk; see spltz.c:173 / :506.
 *
 * This lives in its own translation unit on purpose. libspl-int.h is splib's
 * internal header: it pulls in SKGleba's psp2ref hardware headers and does its
 * own `typedef int bool` / `#define NULL`, which would fight the vitasdk
 * headers main.c needs. Keeping it to one tiny TU that includes no vitasdk
 * header at all sidesteps that entirely. The header's own
 * _Static_assert(sizeof(struct spl_root_s) <= 0xC0) still guards against the
 * struct drifting out from under us if splib is rebuilt from a newer tree.
 */
#include "libspl-int.h"

/*
 * Returns mbase - the NS virtual address that corresponds to physical
 * statics[RST] - or 0 if the TZS backend was never initialised, in which case
 * the out-parameters are left untouched.
 */
unsigned int pfe_tz_layout(unsigned int *rst, unsigned int *len,
                           unsigned int *ttbr0_off, unsigned int *ttbr1_off)
{
    /* SPLROOT_MAGIC is a multi-character constant in splib's own header; its
     * value only has to agree with the splib that wrote it, which it does. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmultichar"
    const unsigned int magic = SPLROOT_MAGIC;
#pragma GCC diagnostic pop

    if (!spl_root || spl_root->magic != magic || !spl_root->tzs.initialized)
        return 0;

    struct tzs_layout_s *l = &spl_root->tzs.layout;
    if (!l->mbase)
        return 0;

    if (rst)       *rst       = l->statics[SPLTZ_STATIC_RST];
    if (len)       *len       = l->statics[SPLTZ_STATIC_LEN];
    if (ttbr0_off) *ttbr0_off = l->statics[SPLTZ_STATIC_TTBR0];
    if (ttbr1_off) *ttbr1_off = l->statics[SPLTZ_STATIC_TTBR1];
    return (unsigned int)l->mbase;
}
