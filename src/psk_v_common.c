/*
 * fldigi-psk-core: submode table and mode parameters of the PSK variants.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 (tag v4.2.13, commit e7c3ab37, mirror wave-owl/fldigi):
 * src/psk/psk.cxx (the psk constructor), src/globals/globals.cxx (mode_info names),
 * Copyright (C) Dave Freese W1HKJ, John Douyere VK2ETA, John Phelps KL4YFD and the fldigi
 * developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "psk_v_internal.h"

#include <ctype.h>
#include <stddef.h>

/*
 * Every psk mode of fldigi 4.2.13 in trx_mode order (globals/globals.cxx:127-164, 227-256:
 * short name, long name, ADIF name). Parameters from the psk constructor's switch
 * (psk.cxx:383-903, cited per row) and its derived values (psk.cxx:905-909, 1059-1063):
 * sc_bw = samplerate / symbollen in integer arithmetic (both are int), as fldigi computes it.
 * OFDM-2000/2000F are commented out in fldigi and not listed.
 */
static const fpsk_v_submode SUBMODES[] = {
    /* psk.cxx:384-388 */
    {"BPSK31", "BPSK-31", "PSK31", 59, 8000, 256, 32, 1, 1.4, FPSK_V_BPSK, 0, 0, 2, 0, 0,
     31.0, 31.0, 31.25},
    /* psk.cxx:389-393 */
    {"BPSK63", "BPSK-63", "PSK63", 60, 8000, 128, 64, 1, 1.4, FPSK_V_BPSK, 0, 0, 2, 0, 0,
     62.0, 62.0, 62.5},
    /* psk.cxx:450-455 */
    {"BPSK63F", "BPSK-63F", "PSK63F", 61, 8000, 128, 64, 1, 1.4, FPSK_V_PSKR, 7, 0, 2, 0, 0,
     62.0, 62.0, 62.5},
    /* psk.cxx:394-398 */
    {"BPSK125", "BPSK-125", "PSK125", 62, 8000, 64, 128, 1, 1.4, FPSK_V_BPSK, 0, 0, 2, 0, 0,
     125.0, 125.0, 125.0},
    /* psk.cxx:399-403 */
    {"BPSK250", "BPSK-250", "PSK250", 63, 8000, 32, 256, 1, 1.4, FPSK_V_BPSK, 0, 0, 2, 0, 0,
     250.0, 250.0, 250.0},
    /* psk.cxx:404-408 */
    {"BPSK500", "BPSK-500", "PSK500", 64, 8000, 16, 512, 1, 1.4, FPSK_V_BPSK, 0, 0, 2, 0, 0,
     500.0, 500.0, 500.0},
    /* psk.cxx:409-413 */
    {"BPSK1000", "BPSK-1000", "PSK1000", 65, 8000, 8, 128, 1, 1.4, FPSK_V_BPSK, 0, 0, 2, 0, 0,
     1000.0, 1000.0, 1000.0},
    /* psk.cxx:756-761 */
    {"PSK125C12", "12xPSK125", "PSK125C12", 66, 8000, 64, 128, 12, 1.4, FPSK_V_BPSK, 0, 0, 2, 0, 0,
     125.0, 2050.0, 125.0},
    /* psk.cxx:804-809 */
    {"PSK250C6", "6xPSK250", "PSK250C6", 67, 8000, 32, 512, 6, 1.4, FPSK_V_BPSK, 0, 0, 2, 0, 0,
     250.0, 2000.0, 250.0},
    /* psk.cxx:827-832 */
    {"PSK500C2", "2xPSK500", "PSK500C2", 68, 8000, 16, 512, 2, 1.4, FPSK_V_BPSK, 0, 0, 2, 0, 0,
     500.0, 1200.0, 500.0},
    /* psk.cxx:833-838 */
    {"PSK500C4", "4xPSK500", "PSK500C4", 69, 8000, 16, 512, 4, 1.4, FPSK_V_BPSK, 0, 0, 2, 0, 0,
     500.0, 2599.9999999999995, 500.0},
    /* psk.cxx:865-871 */
    {"PSK800C2", "2xPSK800", "PSK800C2", 70, 8000, 10, 512, 2, 1.4, FPSK_V_BPSK, 0, 0, 2, 0, 0,
     800.0, 1920.0, 800.0},
    /* psk.cxx:881-887 */
    {"PSK1000C2", "2xPSK1000", "PSK1000C2", 71, 8000, 8, 1024, 2, 1.4, FPSK_V_BPSK, 0, 0, 160, 0, 0,
     1000.0, 2400.0, 1000.0},
    /* psk.cxx:415-421 */
    {"QPSK31", "QPSK-31", "QPSK31", 72, 8000, 256, 32, 1, 1.4, FPSK_V_QPSK, 5, 0, 2, 0, 0,
     31.0, 31.0, 31.25},
    /* psk.cxx:422-428 */
    {"QPSK63", "QPSK-63", "QPSK63", 73, 8000, 128, 64, 1, 1.4, FPSK_V_QPSK, 5, 0, 2, 0, 0,
     62.0, 62.0, 62.5},
    /* psk.cxx:429-435 */
    {"QPSK125", "QPSK-125", "QPSK125", 74, 8000, 64, 128, 1, 1.4, FPSK_V_QPSK, 5, 0, 2, 0, 0,
     125.0, 125.0, 125.0},
    /* psk.cxx:436-442 */
    {"QPSK250", "QPSK-250", "QPSK250", 75, 8000, 32, 256, 1, 1.4, FPSK_V_QPSK, 5, 0, 2, 0, 0,
     250.0, 250.0, 250.0},
    /* psk.cxx:443-449 */
    {"QPSK500", "QPSK-500", "QPSK500", 76, 8000, 16, 512, 1, 1.4, FPSK_V_QPSK, 5, 0, 2, 0, 0,
     500.0, 500.0, 500.0},
    /* psk.cxx:534-543 */
    {"8PSK125", "8PSK-125", "8PSK125", 77, 16000, 128, 128, 1, 1.4, FPSK_V_8PSK, 0, 0, 2, 0, 1,
     125.0, 125.0, 125.0},
    /* psk.cxx:576-586 */
    {"8PSK125FL", "8PSK-125FL", "8PSK125FL", 78, 16000, 128, 128, 1, 1.4, FPSK_V_8PSK, 13, 0, 384, 55, 1,
     125.0, 125.0, 125.0},
    /* psk.cxx:598-608 */
    {"8PSK125F", "8PSK-125F", "8PSK125F", 79, 16000, 128, 128, 1, 1.4, FPSK_V_8PSK, 16, 0, 384, 55, 1,
     125.0, 125.0, 125.0},
    /* psk.cxx:544-553 */
    {"8PSK250", "8PSK-250", "8PSK250", 80, 16000, 64, 256, 1, 1.4, FPSK_V_8PSK, 0, 0, 2, 0, 1,
     250.0, 250.0, 250.0},
    /* psk.cxx:587-597 */
    {"8PSK250FL", "8PSK-250FL", "8PSK250F", 81, 16000, 64, 256, 1, 1.4, FPSK_V_8PSK, 13, 0, 512, 65, 1,
     250.0, 250.0, 250.0},
    /* psk.cxx:609-619 */
    {"8PSK250F", "8PSK-250F", "8PSK250F", 82, 16000, 64, 256, 1, 1.4, FPSK_V_8PSK, 16, 0, 512, 65, 1,
     250.0, 250.0, 250.0},
    /* psk.cxx:554-563 */
    {"8PSK500", "8PSK-500", "8PSK500", 83, 16000, 32, 512, 1, 1.4, FPSK_V_8PSK, 0, 0, 2, 0, 1,
     500.0, 500.0, 500.0},
    /* psk.cxx:620-631 */
    {"8PSK500F", "8PSK-500F", "8PSK500F", 84, 16000, 32, 512, 1, 1.4, FPSK_V_8PSK, 13, 1, 640, 80, 1,
     500.0, 500.0, 500.0},
    /* psk.cxx:564-573 */
    {"8PSK1000", "8PSK-1000", "8PSK1000", 85, 16000, 16, 1024, 1, 1.4, FPSK_V_8PSK, 0, 0, 2, 0, 1,
     1000.0, 1000.0, 1000.0},
    /* psk.cxx:632-644 */
    {"8PSK1000F", "8PSK-1000F", "8PSK1000F", 86, 16000, 16, 1024, 1, 1.4, FPSK_V_8PSK, 7, 1, 512, 120, 1,
     1000.0, 1000.0, 1000.0},
    /* psk.cxx:645-657 */
    {"8PSK1200F", "8PSK-1200F", "8PSK1200F", 87, 16000, 13, 2048, 1, 1.4, FPSK_V_8PSK, 7, 1, 512, 175, 1,
     1230.0, 1230.0, 1230.7692307692307},
    /* psk.cxx:458-472 */
    {"OFDM500F", "OFDM-500F", "OFDM500F", 88, 16000, 256, 192, 4, 2.0, FPSK_V_XPSK, 15, 0, 2000, 200, 1,
     62.0, 434.0, 62.5},
    /* psk.cxx:474-488 */
    {"OFDM750F", "OFDM-750F", "OFDM750F", 89, 16000, 128, 384, 3, 2.0, FPSK_V_8PSK, 13, 0, 3600, 360, 1,
     125.0, 625.0, 125.0},
    /* psk.cxx:519-530 */
    {"OFDM3500", "OFDM-3500", "OFDM3500", 90, 16000, 64, 1536, 7, 2.0, FPSK_V_8PSK, 0, 0, 2, 0, 0,
     250.0, 3250.0, 250.0},
    /* psk.cxx:660-666 */
    {"PSK125R", "PSK-125R", "PSK125R", 138, 8000, 64, 128, 1, 1.4, FPSK_V_PSKR, 7, 0, 40, 0, 0,
     125.0, 125.0, 125.0},
    /* psk.cxx:667-673 */
    {"PSK250R", "PSK-250R", "PSK250R", 139, 8000, 32, 256, 1, 1.4, FPSK_V_PSKR, 7, 0, 80, 0, 0,
     250.0, 250.0, 250.0},
    /* psk.cxx:674-680 */
    {"PSK500R", "PSK-500R", "PSK500R", 140, 8000, 16, 512, 1, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     500.0, 500.0, 500.0},
    /* psk.cxx:681-687 */
    {"PSK1000R", "PSK-1000R", "PSK1000R", 141, 8000, 8, 512, 1, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     1000.0, 1000.0, 1000.0},
    /* psk.cxx:690-697 */
    {"PSK63RC4", "4xPSK63R", "PSK63RC4", 142, 8000, 128, 128, 4, 1.4, FPSK_V_PSKR, 7, 0, 80, 0, 0,
     62.0, 322.4, 62.5},
    /* psk.cxx:698-705 */
    {"PSK63RC5", "5xPSK63R", "PSK63RC5", 143, 8000, 128, 512, 5, 1.4, FPSK_V_PSKR, 7, 0, 260, 0, 0,
     62.0, 409.2, 62.5},
    /* psk.cxx:706-713 */
    {"PSK63RC10", "10xPSK63R", "PSK63RC10", 144, 8000, 128, 512, 10, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     62.0, 843.1999999999999, 62.5},
    /* psk.cxx:714-721 */
    {"PSK63RC20", "20xPSK63R", "PSK63RC20", 145, 8000, 128, 512, 20, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     62.0, 1711.1999999999998, 62.5},
    /* psk.cxx:722-729 */
    {"PSK63RC32", "32xPSK63R", "PSK63RC32", 146, 8000, 128, 512, 32, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     62.0, 2752.7999999999997, 62.5},
    /* psk.cxx:731-738 */
    {"PSK125RC4", "4xPSK125R", "PSK125RC4", 147, 8000, 64, 512, 4, 1.4, FPSK_V_PSKR, 7, 0, 80, 0, 0,
     125.0, 649.9999999999999, 125.0},
    /* psk.cxx:739-746 */
    {"PSK125RC5", "5xPSK125R", "PSK125RC5", 148, 8000, 64, 512, 5, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     125.0, 825.0, 125.0},
    /* psk.cxx:747-754 */
    {"PSK125RC10", "10xPSK125R", "PSK125RC10", 149, 8000, 64, 512, 10, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     125.0, 1700.0, 125.0},
    /* psk.cxx:762-769 */
    {"PSK125RC12", "12xPSK125R", "PSK125RC12", 150, 8000, 64, 512, 12, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     125.0, 2050.0, 125.0},
    /* psk.cxx:771-778 */
    {"PSK125RC16", "16xPSK125R", "PSK125RC16", 151, 8000, 64, 512, 16, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     125.0, 2750.0, 125.0},
    /* psk.cxx:780-787 */
    {"PSK250RC2", "2xPSK250R", "PSK250RC2", 152, 8000, 32, 512, 2, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     250.0, 600.0, 250.0},
    /* psk.cxx:788-795 */
    {"PSK250RC3", "3xPSK250R", "PSK250RC3", 153, 8000, 32, 512, 3, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     250.0, 950.0, 250.0},
    /* psk.cxx:796-803 */
    {"PSK250RC5", "5xPSK250R", "PSK250RC5", 154, 8000, 32, 1024, 5, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     250.0, 1650.0, 250.0},
    /* psk.cxx:810-817 */
    {"PSK250RC6", "6xPSK250R", "PSK250RC6", 155, 8000, 32, 1024, 6, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     250.0, 2000.0, 250.0},
    /* psk.cxx:818-825 */
    {"PSK250RC7", "7xPSK250R", "PSK250RC7", 156, 8000, 32, 1024, 7, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     250.0, 2349.9999999999995, 250.0},
    /* psk.cxx:840-847 */
    {"PSK500RC2", "2xPSK500R", "PSK500RC2", 157, 8000, 16, 1024, 2, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     500.0, 1200.0, 500.0},
    /* psk.cxx:848-855 */
    {"PSK500RC3", "3xPSK500R", "PSK500RC3", 158, 8000, 16, 1024, 3, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     500.0, 1900.0, 500.0},
    /* psk.cxx:856-863 */
    {"PSK500RC4", "4xPSK500R", "PSK500RC4", 159, 8000, 16, 1024, 4, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     500.0, 2599.9999999999995, 500.0},
    /* psk.cxx:872-879 */
    {"PSK800RC2", "2xPSK800R", "PSK800RC2", 160, 8000, 10, 1024, 2, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     800.0, 1920.0, 800.0},
    /* psk.cxx:888-895 */
    {"PSK1000RC2", "2xPSK1000R", "PSK1000RC2", 161, 8000, 8, 1024, 2, 1.4, FPSK_V_PSKR, 7, 0, 160, 0, 0,
     1000.0, 2400.0, 1000.0},
};

#define FPSK_VERSION "0.3.0"

/* Contract and capability tags (wave-owl INV-751, INV-931, D-932). */
const char *fpsk_version(void)
{
    return "fldigi-psk-core " FPSK_VERSION " contract=" FPSK_CONTRACT
           " abi=1 tx=1 variants=1 dsp=" FDSP_VERSION_STRING;
}

#define NSUBMODES ((int)(sizeof SUBMODES / sizeof SUBMODES[0]))

int fpsk_v_submode_count(void)
{
    return NSUBMODES;
}

const fpsk_v_submode *fpsk_v_submode_at(int index)
{
    return index >= 0 && index < NSUBMODES ? &SUBMODES[index] : NULL;
}

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
    return *a == *b;
}

const fpsk_v_submode *fpsk_v_submode_find(const char *name)
{
    int i;
    if (!name)
        return NULL;
    for (i = 0; i < NSUBMODES; i++)
        if (same_name(name, SUBMODES[i].name) || same_name(name, SUBMODES[i].label))
            return &SUBMODES[i];
    return NULL;
}

/* Flags and FEC of the psk constructor, psk.cxx:362-1063. */
int pskv_mode_init(pskv_mode *m, const fpsk_v_submode *sm)
{
    int id;
    if (!m || !sm)
        return -1;
    id = sm->fldigi_id;
    m->sm = sm;
    m->samplerate = sm->rate;
    m->symbollen = sm->symbollen;
    m->dcdbits = sm->dcdbits;
    m->numcarriers = sm->carriers;
    m->qpsk = sm->kind == FPSK_V_QPSK;
    m->pskr = sm->kind == FPSK_V_PSKR;
    m->x8psk = sm->kind == FPSK_V_8PSK;
    m->xpsk = sm->kind == FPSK_V_XPSK;
    /* _disablefec: the 8PSK and OFDM modes without FEC (psk.cxx:538, 548, ..., 523) */
    m->disablefec = (m->x8psk || m->xpsk) && sm->fec_k == 0;
    m->puncturing = sm->puncturing;
    m->psk63f = id == 61;                       /* MODE_PSK63F */
    m->ofdm = id >= 88 && id <= 90;             /* MODE_OFDM_500F .. MODE_OFDM_3500 */
    m->ofdm_fec = id == 88 || id == 89;         /* MODE_OFDM_500F, MODE_OFDM_750F */
    m->bpsk500 = id == 64 || id == 76;          /* MODE_PSK500, MODE_QPSK500 (psk.cxx:1449-1451) */
    m->psk_viterbi = id == 86 || id == 87;      /* PSKviterbi, psk.cxx:642, 655 */
    /* PSK_CORE filters for PSK31, PSK63, PSK63F, QPSK31, QPSK63; SINC for all others */
    m->sinc = !(id == 59 || id == 60 || id == 61 || id == 72 || id == 73);
    /* psk.cxx:905-909 */
    if (m->qpsk || m->xpsk)
        m->symbits = 2;
    else if (m->x8psk)
        m->symbits = 3;
    else
        m->symbits = 1;
    m->idepth = sm->idepth;
    m->flushlength = sm->flushlength;
    m->vestigial = sm->vestigial;
    /* psk.cxx:977-1031 */
    m->k = 0;
    m->dec2 = 0;
    m->chunksize = 0;
    m->traceback = 0;
    if (m->qpsk) {
        m->k = 5;                               /* K, POLY1, POLY2: psk.cxx:66-68 */
        m->poly1 = 0x17;
        m->poly2 = 0x19;
    } else if (m->pskr || m->psk_viterbi) {
        m->k = 7;                               /* PSKR_K: psk.cxx:72-74 */
        m->poly1 = 0x6d;
        m->poly2 = 0x4f;
        m->chunksize = 4;
        m->dec2 = 1;
    } else if (id == 79 || id == 82) {          /* MODE_8PSK125F, MODE_8PSK250F */
        m->k = 16;                              /* K16: psk.cxx:92-94 */
        m->poly1 = 0152711;
        m->poly2 = 0126723;
        m->chunksize = 4;
        m->dec2 = 1;
    } else if (id == 88) {                      /* MODE_OFDM_500F */
        m->k = 15;                              /* THOR_K15: psk.cxx:87-89 */
        m->poly1 = 044735;
        m->poly2 = 063057;
        m->chunksize = 4;
        m->traceback = 15 * 10;
    } else if ((m->xpsk || m->x8psk) && !m->disablefec) {
        m->k = 13;                              /* K13: psk.cxx:83-85 */
        m->poly1 = 016461;
        m->poly2 = 012767;
        m->chunksize = 4;
        m->dec2 = m->x8psk && !m->puncturing;
        m->traceback = m->puncturing ? 13 * 16 : 13 * 10;
    }
    /* psk.cxx:1059-1063 */
    m->sc_bw = sm->sc_bw;
    m->inter_carrier = sm->separation * m->sc_bw;
    m->bandwidth = sm->bandwidth;
    return 0;
}

double pskv_clamp_freq(const pskv_mode *m, double f)
{
    /* CLAMP(freq, LowFreqCutoff + bandwidth / 2, HighFreqCutoff - bandwidth / 2) with the
     * defaults 0 and 3000 Hz (configuration.h:204-209), util.h:52. OFDM-3500 is wider than
     * 3000 Hz: with the default waterfall fldigi clamps its carrier to 1375 Hz and cannot
     * decode it; it needs the 4000 Hz waterfall setting, which is used here. */
    double high = m->bandwidth >= 3000 ? 4000 : 3000;
    double lo = 0 + m->bandwidth / 2, hi = high - m->bandwidth / 2;
    return f > hi ? hi : (f < lo ? lo : f);
}
