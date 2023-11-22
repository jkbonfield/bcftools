/*  bam2bcf_indel.c -- indel caller.

    Copyright (C) 2010, 2011 Broad Institute.
    Copyright (C) 2012-2014,2016-2017, 2021-2023 Genome Research Ltd.

    Author: Heng Li <lh3@sanger.ac.uk>
            Petr Danecek <pd3@sanger.ac.uk>
	    James Bonfield <jkb@sanger.ac.uk>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.  */

//#define CONS_DEBUG

#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <htslib/hts.h>
#include <htslib/sam.h>
#include <htslib/khash_str2int.h>
#include "bam2bcf.h"
#include "str_finder.h"

#include <htslib/ksort.h>
// Is there no way to share these between the 3 implementations?
KSORT_INIT_STATIC_GENERIC(uint32_t)

#define MINUS_CONST 0x10000000

#define MAX_TYPES 64

#ifndef MIN
#  define MIN(a,b) ((a)<(b)?(a):(b))
#endif

#ifndef ABS
#  define ABS(a) ((a)<0?-(a):(a))
#endif

#ifndef MAX
#  define MAX(a,b) ((a)>(b)?(a):(b))
#endif

// l is the relative gap length and l_run is the length of the homopolymer
// on the reference.
//
// Larger seqQ is good, so increasing tandemQ calls more indels,
// and longer l_run means fewer calls.  It is capped later at 255.
// For short l_runs, the qual is simply based on size of indel
// larger ones being considered more likely to be real.
// Longer indels get assigned a score based on the relative indel size
// to homopolymer, where l_run base will have already been verified by
// the caller to ensure it's compatible.
static inline int est_seqQ(const bcf_callaux_t *bca, int l, int l_run, int str_len)
{
    int q, qh;
    // Short indels are more likely sequencing error than large ones.
    // So "seqQ" scales with size of observation "l".
    //
    // Note openQ and extQ are error likelihoods in Phred scale.  Hence high
    // openQ means we're very unlikely to miscall an indel.
    // Ie it's not the open/ext "costs" normally used in alignment; more the reverse.
    //
    // We use MIN(q,qh) below, so we can remove the q component by specifying
    // a large -o parameter in mpileup.
    q = bca->openQ + bca->extQ * (abs(l) - 1);

    // Orig method; best with Illumina (high openQ)
//    qh = bca->tandemQ * (double)abs(l) / l_run + .499;

    // Penalise longer homopolymers quadratically more, but boost shorter ones.
    // Best with CCS (low openQ)
    //qh = 2 * bca->tandemQ * pow((double)abs(l) / l_run, 1.5) + .499;

    // (l/l_run)^1.26 for openQ=25 or ^1 for openQ=40.
//    double openQ = MIN(40, bca->openQ);
//    qh = (30/openQ) * bca->tandemQ
//        * pow((double)abs(l) / l_run, 1/sqrt(openQ/40)) + .499;

    // Linear scaled on openQ too
    qh = bca->tandemQ * (double)abs(l) / l_run + .499;

    // Generic maybe ?
    // power = 1/sqrt(MIN(40,bca->openQ)/40.);
    // qh = ... * pow((double)abs(l)/l_run, power)

    // bam2bcf.c caps has "if q>seqQ) q=seqQ" so it caps base qual 'q'.
    // A 1bp indel would therefore have a maximum qual it could be considered based
    // on open+ext.  Hence why openQ is phred score indicating if the base is real
    // or an over/under-call. (high openQ means high trust in base)
    return q < qh? q : qh;
}

// Part of bcf_call_gap_prep.
//
// Scans the pileup to identify all the different sizes of indels
// present.
// types[] returned is sorted by size, from smallest (maybe negative) to largest.
//
// Returns types and fills out n_types_r,  max_rd_len_r and ref_type_r,
//         or NULL on error.
static int *bcf_cgp_find_types(int n, int *n_plp, bam_pileup1_t **plp,
                               int pos, bcf_callaux_t *bca, const char *ref,
                               int *max_rd_len_r, int *n_types_r,
                               int *ref_type_r, int *N_r) {
    int i, j, t, s, N, m, max_rd_len, n_types;
    int n_alt = 0, n_tot = 0, indel_support_ok = 0;
    uint32_t *aux;
    int *types;

    // N is the total number of reads
    for (s = N = 0; s < n; ++s)
        N += n_plp[s];

    bca->max_support = bca->max_frac = 0;
    aux = (uint32_t*) calloc(N + 1, 4);
    if (!aux)
        return NULL;

    m = max_rd_len = 0;
    aux[m++] = MINUS_CONST; // zero indel is always a type (REF)

    // Fill out aux[] array with all the non-zero indel sizes.
    // Also tally number with indels (n_alt) and total (n_tot).
    for (s = 0; s < n; ++s) {
        int na = 0, nt = 0;
        for (i = 0; i < n_plp[s]; ++i) {
            const bam_pileup1_t *p = plp[s] + i;
            ++nt;
            if (p->indel != 0) {
                ++na;
                aux[m++] = MINUS_CONST + p->indel;
            }

            // FIXME: cache me in pileup struct.
            j = bam_cigar2qlen(p->b->core.n_cigar, bam_get_cigar(p->b));
            if (j > max_rd_len) max_rd_len = j;
        }
        double frac = (double)na/nt;
        if ( !indel_support_ok && na >= bca->min_support
             && frac >= bca->min_frac )
            indel_support_ok = 1;
        if ( na > bca->max_support && frac > 0 )
            bca->max_support = na, bca->max_frac = frac;

        n_alt += na;
        n_tot += nt;
    }

    // Sort aux[] and dedup
    ks_introsort(uint32_t, m, aux);
    for (i = 1, n_types = 1; i < m; ++i)
        if (aux[i] != aux[i-1]) ++n_types;

    // Taking totals makes it hard to call rare indels (IMF filter)
    if ( !bca->per_sample_flt )
        indel_support_ok = ( (double)n_alt / n_tot < bca->min_frac
                             || n_alt < bca->min_support )
            ? 0 : 1;
    if ( n_types == 1 || !indel_support_ok ) { // then skip
        free(aux);
        return NULL;
    }

    // Bail out if we have far too many types of indel
    if (n_types >= MAX_TYPES) {
        free(aux);
        // TODO revisit how/whether to control printing this warning
        if (hts_verbose >= 2)
            fprintf(stderr, "[%s] excessive INDEL alleles at position %d. "
                    "Skip the position.\n", __func__, pos + 1);
        return NULL;
    }

    // To prevent long stretches of N's to be mistaken for indels
    // (sometimes thousands of bases), check the number of N's in the
    // sequence and skip places where half or more reference bases are Ns.
    int nN=0, i_end = pos + (2*bca->indel_win_size < max_rd_len
                            ?2*bca->indel_win_size : max_rd_len);
    for (i=pos; i<i_end && ref[i]; i++)
        nN += ref[i] == 'N';
    if ( nN*2>(i-pos) ) {
        free(aux);
        return NULL;
    }

    // Finally fill out the types[] array detailing the size of insertion
    // or deletion.
    types = (int*)calloc(n_types, sizeof(int));
    if (!types) {
        free(aux);
        return NULL;
    }
    t = 0;
    for (i = 0; i < m; ++i) {
        int sz = (int32_t)(aux[i] - MINUS_CONST);
        int j;
        for (j = i+1; j < m; j++)
            if (aux[j] != aux[i])
                break;

        if (sz == 0
            || (j-i >= bca->min_support &&
                // Note, doesn't handle bca->per_sample_flt yet
                (bca->per_sample_flt
                 || (double)(j-i) / n_tot >= bca->min_frac)))
            types[t++] = sz;
        i = j-1;
    }
    free(aux);

    if (t <= 1) {
        free(types);
        return NULL;
    }
    n_types = t;

    // Find reference type; types[?] == 0)
    for (t = 0; t < n_types; ++t)
        if (types[t] == 0) break;

    *ref_type_r   = t;
    *n_types_r    = n_types;
    *max_rd_len_r = max_rd_len;
    *N_r          = N;

    return types;
}

// Increment ins["str"] and freq["str"]
#define NI 100 // number of alternative insertion sequences
// Could use a hash table too, but expectation is a tiny number of alternatives
typedef struct {
    char *str[NI];
    int len[NI];
    int freq[NI];
} str_freq;

static int bcf_cgp_append_cons(str_freq *sf, char *str, int len, int freq) {
    int j;

    for (j = 0; j < NI && sf->str[j]; j++) {
        if (sf->len[j] == len && memcmp(sf->str[j], str, len) == 0)
            break;
    }
    if (j >= NI)
        return 0; // too many choices; discard

    sf->freq[j]+=freq;
    if (!sf->str[j]) {
        // new insertion
        if (!(sf->str[j] = malloc(len+1)))
            return -1;
        memcpy(sf->str[j], str, len);
        sf->len[j] = len;
    }

    return 0;
}

/*
 * Compute the consensus for a specific indel type at pos.
 *
 * left_shift is the number of inserted(+) or deleted(-) bases added to
 * the consensus before we get to pos.  This is necessary so the alignment
 * band is correct as it's expected to start at left/right edges in
 * sync
 *
 * We accumulate into several buffers for counting base types:
 * cons_base   - consensus of data with p->indel == type, bases or gap
 * ref_base    - consensus of data with p->indel != type, bases or gap
 * cons_ins    - consensus of data with p->indel == type, insertions
 * ref_ins     - consensus of data with p->indel == type, bases or gap
 *
 * The purpose of cons_ins vs cons_base is if we have very low
 * coverage due to nearly all reads being another type, then we can
 * still get a robust consensus using the other data.  If we don't
 * have shallow data, then we'll not use as much of ref_base as we may
 * have correlated variants.
 *
 * Eg:
 * REF: AGCTATGAGGCTGATA
 * SEQ: AGGTAGGAGGGTGATA (x1)
 * SEQ: AGCTACGAGG*TGATA (x24)
 * SEQ: AGCTACTAGG*TGATA (x24)
 *
 * Cons for no-del is Cs not Gs.  Cannot trust it, so use N if shallow.
 * CON: AGCTACNAGGGTGATA
 *
 * There are still some problems in cons_ins vs ref_ins assignment.
 * We sometimes seem multiple similar-length insertions added at
 * different locations.  Ideally we'd like to consider these as all
 * the same insertion if the size is the same and it's comparable seq.
 */
static char **bcf_cgp_consensus(int n, int *n_plp, bam_pileup1_t **plp,
                                int pos, bcf_callaux_t *bca, const char *ref,
                                int ref_len, int left, int right,
                                int sample, int type, int biggest_del,
                                int *left_shift, int *right_shift,
                                int *band, int *tcon_len, int *cpos_pos) {
    // Map ASCII ACGTN* to 012345
    static uint8_t base6[256] = {
        4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,
        4,4,4,4,4,4,4,4,  4,4,5,4,4,4,4,4,  4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,
        //A   C       G       *^                     T
        4,0,4,1,4,4,4,2,  4,4,4,4,4,4,4,4,  4,4,4,4,3,3,4,4,  4,4,4,4,4,4,4,4,
        4,0,4,1,4,4,4,2,  4,4,4,4,4,4,4,4,  4,4,4,4,3,3,4,4,  4,4,4,4,4,4,4,4,

        4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,
        4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,
        4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,
        4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,  4,4,4,4,4,4,4,4,
    };

    // single base or del
    int (*cons_base)[6] = calloc(right - left + 1, sizeof(*cons_base));
    // multi-base insertions
    str_freq *cons_ins  = calloc(right - left + 1, sizeof(*cons_ins));

    // non-indel ref for all reads on this sample, rather than those just
    // matching type.  We use this for handling the case where we have a
    // homozygous deletion being studied, but with 1 or 2 reads misaligned
    // and containing a base there.
    //
    // Eg if the type[]=0 consensus is made up of a very small sample size,
    // which is also enriched for highly error prone data.  We can use
    // the other reads from type[] != 0 to flesh out the consensus and
    // improve accuracy.
    int (*ref_base)[6]  = calloc(right - left + 1, sizeof(*ref_base));
    str_freq *ref_ins   = calloc(right - left + 1, sizeof(*ref_ins));
    int i, j, k, s = sample;
    char **cons = NULL;

    if (!cons_base || !cons_ins || !ref_base || !ref_ins)
        goto err;

    //--------------------------------------------------
    // Accumulate sequences into cons_base and cons_ins arrays
    int local_band_max = 0; // maximum absolute deviation from diagonal
    for (i = 0; i < n_plp[s]; i++) {
        const bam_pileup1_t *p = plp[s] + i;
        bam1_t *b = p->b;
        int x = b->core.pos;  // ref coordinate
        int y = 0;            // seq coordinate
        uint32_t *cigar = bam_get_cigar(b);
        uint8_t *seq = bam_get_seq(b);

        int local_band = 0; // current deviation from diagonal
        for (k = 0; k < b->core.n_cigar; ++k) {
            int op  = cigar[k] &  BAM_CIGAR_MASK;
            int len = cigar[k] >> BAM_CIGAR_SHIFT;
            int base;
            int skip_to = 0;

            switch(op) {
            case BAM_CSOFT_CLIP:
                y += len;
                break;

            case BAM_CMATCH:
            case BAM_CEQUAL:
            case BAM_CDIFF: {
                // Can short-cut this with j_start and j_end based on
                // x+len and left,right
                for (j = 0; j < len; j++, x++, y++) {
                    if (x < left) continue;
                    if (x >= right) break;

                    base = bam_seqi(seq, y);
                    if (p->indel == type)
                        // Convert 4-bit base ambig code to 0,1,2,3,4 range
                        cons_base[x-left][seq_nt16_int[base]]++;
                    else if (x != pos+1) // indel being assessed question
                        ref_base[x-left][seq_nt16_int[base]]++;
                }
                break;
            }

            case BAM_CINS: {
                if (x >= left && x < right) {
                    local_band += p->indel;
                    if (local_band_max < local_band)
                        local_band_max = local_band;
                }

                char ins[1024];
                for (j = 0; j < len; j++, y++) {
                    if (x < left) continue;
                    if (x >= right)
                        break;
                    base = bam_seqi(seq, y);
                    if (j < 1024)
                        ins[j] = seq_nt16_int[base];
                }

                // Insertions come before a ref match.
                // 5I 5M is IIIIIM M M M M events, not
                // {IIIII,M} M M M M choice.  So we need to include the
                // next match in our sequence when choosing the consensus.
                if (x >= left && x < right) {
                    int ilen = j<1024?j:1024;
                    if (p->indel == type /*&& x == pos+1*/) {
                        // Assume any ins of the same size is the same ins.
                        // (This rescues misaligned insertions.)
                        if (bcf_cgp_append_cons(&cons_ins[x-left], ins,
                                                ilen, 1) < 0)
                            goto err;
                    } else  if (x != pos+1){
                        if (bcf_cgp_append_cons(&ref_ins[x-left],  ins,
                                                ilen, 1) < 0)
                            goto err;
                    }
                }
                break;
            }

            case BAM_CDEL:
                if (x >= left && x < right) {
                    local_band += p->indel;
                    if (local_band_max < -local_band)
                        local_band_max = -local_band;
                }

                // Maybe not perfect for I/D combos, but likely sufficient.
                for (j = 0; j < len; j++, x++) {
                    if (x < left) continue;
                    if (x >= right) break;
                    if ((p->indel == type && !p->is_del) ||  // starts here
                        (p->indel == 0 && p->is_del && len == -type)) // left
                        cons_base[x-left][5]++;
                    else if (x+len <= pos+1 || (skip_to && x > skip_to))
                        ref_base[x-left][5]++;
                    else if (x <= pos && x+len > pos+1) {
                        // we have a deletion which overlaps pos, but
                        // isn't the same "type".  We don't wish to
                        // include these as they may bias the
                        // evaluation by confirming against a
                        // secondary consensus produced with the other
                        // deletion.  We set a marker for how long to
                        // skip adding to ref_base.
                        if (x > skip_to)
                            skip_to = x+len;
                    }
                }
                break;
            }
        }

        // Also track the biggest deviation +/- from diagonal.  We use
        // this band observation in our BAQ alignment step.
        if (*band < local_band_max)
            *band = local_band_max;
    }

    //--------------------------------------------------
    // Expand cons_base to include depth from ref_base/ref_ins
    // Caveat: except at pos itself, where true ref is used if type != 0

    // Note this harms PB-CCS test at chr1:10171880.
    // We could retest this heuristic further maybe.
    for (i = 0; i < right-left; i++) {
        // Total observed depth
        int t = cons_base[i][0] + cons_base[i][1] + cons_base[i][2] +
            cons_base[i][3] + cons_base[i][4] + cons_base[i][5];
        for (j = 0; j < NI; j++) {
            if (!cons_ins[i].str[j])
                break;
            t += cons_ins[i].freq[j];
        }

        // Similarly for depth on the non-ALT calls (NB: not necessarily
        // REF as maybe it's other ALTs).
        int r = ref_base[i][0] + ref_base[i][1] + ref_base[i][2] +
            ref_base[i][3] + ref_base[i][4] + ref_base[i][5];
        for (j = 0; j < NI; j++) {
            if (!ref_ins[i].str[j])
                break;
            r += ref_ins[i].freq[j];
        }

        // When evaluating this particular indel, we don't want to
        // penalise alignments by SNP errors elsewhere.  This can
        // happen when we have low depth for a particular 'type'.
        //
        // So add in a little data from ref_base/ref_ins.
        double rfract = (r - t*2)*.75 / (r+1);

        if (rfract < 1.01 / (r+1e-10))
            rfract = 1.01 / (r+1e-10); // low depth compensation

        // TODO: consider limiting rfract so we never drown out the
        // signal.  We want to use the remaining data only to correct
        // for sequencing errors in low depth alleles.  If we get
        // conflicts, it's better to use N than to change a base
        // incase that variant is genuine.
        if (i+left >= pos+1 && i+left < pos+1-biggest_del) {
            // We're overlapping the current indel region, so
            // we don't wish to bring in evidence from the other
            // "type" data as it'll harm calling.
            continue;
        } else {
            // Otherwise add in a portion of other data to
            // boost low population numbers.
            cons_base[i][0] += rfract * ref_base[i][0];
            cons_base[i][1] += rfract * ref_base[i][1];
            cons_base[i][2] += rfract * ref_base[i][2];
            cons_base[i][3] += rfract * ref_base[i][3];
            cons_base[i][4] += rfract * ref_base[i][4];
            cons_base[i][5] += rfract * ref_base[i][5];
        }

        // Similarly for insertions too; consider a different rfract here?
        for (j = 0; j < NI; j++) {
            if (!ref_ins[i].str[j])
                break;
            if (bcf_cgp_append_cons(&cons_ins[i],
                                    ref_ins[i].str[j], ref_ins[i].len[j],
                                    rfract * ref_ins[i].freq[j]) < 0)
                goto err;
        }
    }

    //--------------------------------------------------
    // Allocate consensus buffer, to worst case length
    int max_len = right-left;
    for (i = 0; i < right-left; i++) {
        if (!cons_ins[i].str[0])
            continue;

        int ins = 0;
        for (j = 0; j < NI; j++) {
            if (!cons_ins[i].str[j])
                break;
            if (cons_ins[i].str[j] && ins < cons_ins[i].len[j])
                ins = cons_ins[i].len[j];
        }
        max_len += ins;
    }
    cons = malloc((max_len+1)*2 + sizeof(char *)*2);
    if (!cons)
        goto err;
    cons[0] = (char *)&cons[2];
    cons[1] = cons[0] + max_len+1;

    //--------------------------------------------------
    // Merge insertions where they are the same length but different
    // sequences.
    // NB: we could just index by length and have accumulators for each,
    // instead of storing separately and merging later (here).
    // Ie str_freq.str is [NI][5] instead.
    for (i = 0; i < right-left; i++) {
        int ins[1024][5];
        for (j = 0; j < NI; j++) {
            if (!cons_ins[i].str[j])
                break;

            if (cons_ins[i].freq[j] == 0)
                continue; // already merged

            int l;
            for (l = 0; l < cons_ins[i].len[j]; l++) {
                // Append to relevant frequency counter, zero all others
                ins[l][0] = ins[l][1] = ins[l][2] = ins[l][3] = ins[l][4] = 0;
                uint8_t b = cons_ins[i].str[j][l];
                ins[l][b] = cons_ins[i].freq[j];
            }

            // Merge other insertions of the same length to ins[] counters
            for (k = j+1; k < NI; k++) {
                if (!cons_ins[i].str[k])
                    break;
                if (cons_ins[i].len[k] != cons_ins[i].len[j])
                    continue;
                if (cons_ins[i].freq[k] == 0)
                    continue; // redundant?

                // Merge str[j] and str[k]
                for (l = 0; l < cons_ins[i].len[k]; l++) {
                    uint8_t b = cons_ins[i].str[k][l];
                    ins[l][b] += cons_ins[i].freq[k];
                }
                cons_ins[i].freq[j] += cons_ins[i].freq[k];
                cons_ins[i].freq[k] = 0;
            }

            // Now replace ins[j] with the consensus insertion of this len.
            for (l = 0; l < cons_ins[i].len[j]; l++) {
                int max_v = 0, base = 0;
                int tot = ins[l][0] + ins[l][1] + ins[l][2]
                        + ins[l][3] + ins[l][4];
                if (max_v < ins[l][0]) max_v = ins[l][0], base = 0;
                if (max_v < ins[l][1]) max_v = ins[l][1], base = 1;
                if (max_v < ins[l][2]) max_v = ins[l][2], base = 2;
                if (max_v < ins[l][3]) max_v = ins[l][3], base = 3;
                if (max_v < ins[l][4]) max_v = ins[l][4], base = 4;

                cons_ins[i].str[j][l] = (max_v > 0.6*tot) ? base : 4;
            }
        }
    }

#define CONS_CUTOFF      .40 // 40% needed for base vs N
#define CONS_CUTOFF2     .80 // 80% needed for gap in cons[1]
#define CONS_CUTOFF_INC  .40 // 40% to include any insertion cons[0]
#define CONS_CUTOFF_INC2 .80 // 80% to include any insertion cons[1] HOM
#define CONS_CUTOFF_INS  .60 // and then 60% needed for it to be bases vs N

    //--------------------------------------------------
    // Walk through the frequency arrays to call the consensus.
    // We produce cons[0] and cons[1].  Both include strongly
    // homozygous indels.  Both also include the indel at 'pos'.
    // However for heterozygous indels we call the most likely event
    // for cons[0] and the less-likely alternative in cons[1].
    // TODO: a proper phase analysis so multiple events end up
    // combining together into the correct consensus.
    *left_shift = 0;
    *right_shift = 0;
    int cnum;

    // Het call filled out in cnum==0 (+ve or -ve).
    // Used in cnum==1 to do the opposite of whichever way we did before.
    int heti[1024] = {0}, hetd[1024] = {0};

    *cpos_pos = -1;
    for (cnum = 0; cnum < 2; cnum++) {
        for (i = k = 0; i < right-left; i++) {
            // Location in consensus matching the indel itself
            if (i >= pos-left+1 && *cpos_pos == -1)
                *cpos_pos = k;

            int max_v = 0, max_v2 = 0, max_j = 4, max_j2 = 4, tot = 0;
            for (j = 0; j < 6; j++) {
                // Top 2 consensus calls
                if (max_v < cons_base[i][j]) {
                    max_v2 = max_v, max_j2 = max_j;
                    max_v = cons_base[i][j], max_j = j;
                } else if (max_v2 < cons_base[i][j]) {
                    max_v2 = cons_base[i][j], max_j2 = j;
                }
                tot += cons_base[i][j];
            }

            // +INS
            int max_v_ins = 0, max_j_ins = 0;
            int tot_ins = 0;
            for (j = 0; j < NI; j++) {
                if (!cons_ins[i].str[j])
                    break;
                if (cons_ins[i].freq[j] == 0)
                    continue; // previously merged

                if (max_v_ins < cons_ins[i].freq[j])
                    //if (i != pos-left+1 || cons_ins[i].len[j] == type)
                    max_v_ins = cons_ins[i].freq[j], max_j_ins = j;
                tot_ins += cons_ins[i].freq[j];
            }

            // NB: tot is based on next matching base, so it includes
            // everything with or without the insertion.
            int tot_sum = tot;
            int always_ins =
                (i == pos-left+1 && type>0) ||       // current eval
                max_v_ins > CONS_CUTOFF_INC2*tot_sum;// HOM
            int het_ins = 0;
            if (!always_ins && max_v_ins >= bca->min_support) {
                // Candidate HET ins.
                if (cnum == 0) {
                    het_ins = max_v_ins > CONS_CUTOFF_INC * tot_sum;
                    if (i < 1024) heti[i] = het_ins
                                      ? 1
                                      : (max_v_ins > .3*tot_sum ? -1:0);
                } else {
                    // HET but uncalled before
                    het_ins = i < 1024 ? (heti[i] == -1) : 0;
                }
            }

            if (always_ins || het_ins) {
                if (max_v_ins > CONS_CUTOFF_INS*tot_ins) {
                    // Insert bases
                    for (j = 0; j < cons_ins[i].len[max_j_ins]; j++) {
                        if (cnum == 0) {
                            if (k < pos-left+*left_shift)
                                (*left_shift)++;
                            else
                                (*right_shift)++;
                        }
                        cons[cnum][k++] = cons_ins[i].str[max_j_ins][j];
                    }
                } else {
                    for (j = 0; j < cons_ins[i].len[max_j_ins]; j++)
                        cons[cnum][k++] = 4; // 'N';
                }
            }

            // Call deletions & bases
            int always_del = (type < 0 && i > pos-left && i <= pos-left-type)
                || cons_base[i][5] > CONS_CUTOFF2 * tot; // HOM del
            int het_del = 0;
            if (!always_del && cons_base[i][5] >= bca->min_support) {
                // Candidate HET del.
                if (cnum == 0) {
                    het_del = cons_base[i][5] >= CONS_CUTOFF * tot;
                    if (i < 1024) {
                        if (i > pos-left && i <= pos-left-biggest_del)
                            hetd[i] = 0;
                        else
                            hetd[i] = het_del
                                ? 1
                                : (cons_base[i][5] >= .3 * tot ? -1 : 0);
                    }
                } else {
                    // HET del uncalled on cnum 0
                    het_del = i < 1024 ? (hetd[i] == -1) : 0;
                    if (max_j == 5 && het_del == 0) {
                        max_v = max_v2;
                        max_j = max_j2;
                    }
                }
            }
            if (always_del || het_del) {
                // Deletion
                if (k < pos-left+*left_shift)
                    (*left_shift)--;
                else
                    (*right_shift)++;
            } else {
                // Finally the easy case - a non-indel base or an N
                if (max_v > CONS_CUTOFF*tot)
                    cons[cnum][k++] = max_j; // "ACGTN*"
                else if (max_v > 0)
                    cons[cnum][k++] = 4;     // 'N';
                else {
                    cons[cnum][k] = left+k < ref_len
                        ? base6[(uint8_t)ref[left+k]]
                        : 4;
                    k++;
                }
            }
        }

        tcon_len[cnum] = k;
    }

    // TODO: replace by io_lib's string pool for rapid tidying.
    // For now this isn't the bottleneck though.
    for (i = 0; i < right-left; i++) {
        for (j = 0; j < NI; j++) {
            if (cons_ins[i].str[j])
                free(cons_ins[i].str[j]);
            if (ref_ins[i].str[j])
                free(ref_ins[i].str[j]);
        }
    }

 err:
    free(cons_base);
    free(ref_base);
    free(cons_ins);
    free(ref_ins);

    return cons;
}

// A rename of bcf_cgp_calc_cons from bam2bcf_indel.c
//
// Compute the insertion consensus for this sample 's' via a basic
// majority rule.
//
// TODO: merge this into bcf_cgp_consensus as another return value?
static char *bcf_cgp_calc_ins_cons(int n, int *n_plp, bam_pileup1_t **plp,
                                   int pos, int *types, int n_types,
                                   int max_ins, int s) {
    return bcf_cgp_calc_cons(n, n_plp, plp, pos, types, n_types, max_ins, s);
}

#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))

// Compile with LIBS="-L. -ldl -ledlib" CLD=g++

// This is faster than ksw and BAQ, meaning we can use larger --indel-size and
// get a more accurate context, improving alignments further.  This *may*
// compensate for reduced sensitivity.
#include "edlib.h"
int edlib_glocal(uint8_t *ref, int l_ref, uint8_t *query, int l_query,
                 double m, double del_bias)
{
    EdlibAlignConfig cfg = 
        edlibNewAlignConfig(
                            //ABS(type)+ABS(l_ref-l_query)+10,
                            -1, // k; use small positive for faster alignment
                            EDLIB_MODE_HW, // mode
                            EDLIB_TASK_LOC, // task
                            //EDLIB_TASK_PATH, // for manual alignment scoring
                            NULL, // additionalEqualities
                            0); // additionalEqualitiesLength
    EdlibAlignResult r = 
        edlibAlign((char *)query, l_query, (char *)ref, l_ref, cfg);

    if (r.status != EDLIB_STATUS_OK || r.numLocations < 1 ||
        !r.endLocations || !r.startLocations) {
        edlibFreeAlignResult(r);
        return INT_MAX;
    }

    int score;
//    score = m*r.editDistance; // Illumina: ie  -0*(glen - l_query)

#if 0
    // Alignment based score, scaled by average sequence quality
    int i, indel=0;
    for (i = score = 0; i < r.alignmentLength; i++) {
        switch(r.alignment[i]) {
        case 0:           indel=0; break;  // match
        case 3: score++;  indel=0; break;  // mismatch
        case 1: case 2:                    // indel
            score+=indel?4:2;
            indel=1;
            break;
        }
    }
    score *= m/2;
#elif 0
    // Alignment based score, using per-base sequence quality
    int i, indel=0, qpos = 0;
    for (i = score = 0; i < r.alignmentLength; i++) {
        switch(r.alignment[i]) {
        case 0:                    indel=0; qpos++; break;  // match
        case 3: score+=qq[qpos]/2; indel=0; qpos++; break;  // mismatch
        case 1:                                             // ins
        case 2:                                             // del
            score+=(indel?2:1)*qq[qpos];
            indel=1;
            qpos += r.alignment[i]==2;
            break;
        }
    }
#elif 0
    // BEST for PB
    //
    // Alignment based score, using per-base sequence quality.
    // Eg params for PacBio CCS.
    // This is *marginally* better than the naive t_len-l_query below, but it's
    // 34% slower mpileup for CCS.  Perhaps not worth the trade off?
    int i;
    double fscore = 0;
    for (i = score = 0; i < r.alignmentLength; i++) {
        switch(r.alignment[i]) {
        case 0:              break;  // match
//        case 3: fscore+=0.5; break;  // mismatch
//        case 1: fscore+=1.0; break;  // ins; higher qual
//        case 2: fscore+=0.6; break;  // del; more often an error
        case 3: score+= 5; break;  // mismatch
        case 1: score+=10; break;  // ins; higher qual
        case 2: score+= 6; break;  // del; more often an error
        }
    }
    score *= m/10;
//    score = m*fscore;
#elif 0
    // As above, but accounting for minimum quality in STR region instead.
    // BAD
    int i;
    double fscore = 0;
    for (i = score = 0; i < r.alignmentLength; i++) {
        switch(r.alignment[i]) {
        case 0:                      break;  // match
        case 3: fscore++;            break;  // mismatch
        case 1: fscore+=m2min/m;     break;  // ins; higher qual
        case 2: fscore+=0.6*m2min/m; break;  // del; more often an error
        }
    }
    score = fscore*m;
#elif 0
    // As above, but factoring in quality.
    // BAD
    int i, qpos = 0;
    double fscore = 0;
    for (i = score = 0; i < r.alignmentLength; i++) {
        switch(r.alignment[i]) {
        case 0:            qpos++;  break;  // match
        case 3: fscore+=qq[qpos++]; break;  // mismatch
        case 1: fscore+=qq[qpos++]; break;  // ins; higher qual
        //case 2: fscore+=0.6*m;      break;  // del; more often an error
        case 2: fscore+=.6*qq[qpos];break;  // del; more often an error
        }
    }
    score = fscore;
#endif


//    int nins = 0, ndel = 0, nmis = 0;
//    for (i = score = 0; i < r.alignmentLength; i++) {
//        switch (r.alignment[i]) {
//        case 1: nins++; break;
//        case 2: ndel++; break;
//        case 3: nmis++; break;
//        }
//    }
//    assert((*r.endLocations - *r.startLocations + 1) - l_query == ndel-nins);
//
//    Then score = f(nins,ndel,nmis).
//    Could also track nis_o,nins_e,ndel_o,ndel_e for open/extend.

#if 1
    int t_len = *r.endLocations - *r.startLocations + 1;

    // Aligned target length minus query length is an indication of the number
    // of insertions and/or deletions.
    // 
    // For CIGAR 10M1I10M t_len > l_query ("AC"  / "ATC")
    // For CIGAR 10M1D10M t_len < l_query ("ATC" / "AC")
    // Hence t_len-l_query is -ve for net insertions and +ve for net deletions.
    // If we compute nins and ndel directly via walking though EDLIB_TASK_PATH
    // we'll see t_len-l_query == ndel-nins.
    // 
    // If a technology has a significantly higher chance of making deletion
    // errors than insertion errors, then we would view deletions as less
    // indicative of this sequence not coming from this candidate allele than
    // if it had insertion (as the deletions are more likely to be errors
    // rather than real, relative to the insertions).  Hence we can skew the
    // score by the net delta of num_del - num_ins.
    //
    // Note this is an approximation that doesn't account for multiple
    // insertions and deletions within the same sequence, but it is much faster
    // as it doesn't require EDLIB_TASK_PATH to be computed.
    //
    // Given editDistance is +1 for every mismatch, insertion and deletion,
    // provided the t_len-l_query multiplier < 1 then this is always +ve.

    score = m*(r.editDistance - del_bias*(t_len - l_query));
#endif

#if 0
    // DEBUG: dump out the sequence alignment
    {
        char rseq[1024], *rcp = rseq, qseq[1024], *qcp = qseq;

        int i, rpos = 0, qpos = 0;
        for (i = 0; i < r.alignmentLength; i++) {
            switch(r.alignment[i]) {
            case 0: // match
            case 3: // mismath
                *rcp++ = "ACGTN"[ref[rpos++]];
                *qcp++ = "ACGTN"[query[qpos++]];
                break;
            case 1: // ins
                *rcp++ = '-';
                *qcp++ = "ACGTN"[query[qpos++]];
                break;
            case 2: // del
                *rcp++ = "ACGTN"[ref[rpos++]];
                *qcp++ = '-';
                break;
            }
        }
        *rcp = 0;
        *qcp = 0;
        fprintf(stderr, "Ref   %s\n", rseq);
        fprintf(stderr, "Seq   %s\n", qseq);
        fprintf(stderr, "Score %d  t-l %d\n", score, t_len - l_query);
    }
#endif

    edlibFreeAlignResult(r);
    return score;
}

// Part of bcf_call_gap_prep.
//
// Realign using BAQ to get an alignment score of a single read vs
// a haplotype consensus.  TODO: replace BAQ with something more robust.
//
// There are many coordinates, so let's explain them.
// - left, right, tbeg, tend, r_start and r_end are in aligned reference
//   coordinates.
//   left/right start from pos +/- indel_win_size.
//   r_start/r_end are the BAM first and last mapped coord on the reference.
//   tbeg and tend are the intersection of the two.
// - qbeg and qend are in BAM sequence coordinates
// - qpos is in sequence coordinates, relative to qbeg.
//
// To see what this means, we have illustrations with coordinates
// above the seqs in reference space and below the seqs in BAM seq space.
//
// Overlap left:
//                     tbeg                        tend
//      r_start        left                 pos    r_end          right
// REF  :..............|--------------------#------:--------------|...
// SEQ  :..............|--------------------#------|
//      0              qbeg                 qpos   qend
//
// Overlap right:
//                        r_start                     tend
//         left           tbeg  pos                   right       r_end
// REF  ...|--------------:-----#---------------------|...........:
// SEQ                    |-----#---------------------|...........:
//                        qbeg  qpos                  qend
//                        0
//
// The "-" sequence is the bit passed in.
// Ie ref2 spans left..right and query spans qbeg..qend.
// We need to adjust ref2 therefore to tbeg..tend.
//
// Fills out score
// Returns 0 on success,
//        <0 on error
static int bcf_cgp_align_score(bam_pileup1_t *p, bcf_callaux_t *bca,
                               int type, int band,
                               uint8_t *ref1, uint8_t *ref2, uint8_t *query,
                               int r_start, int r_end, int long_read,
                               int tbeg, int tend1, int tend2,
                               int left, int right,
                               int qbeg, int qend,
                               int pos, int qpos, int max_deletion,
                               double qavg, double del_bias, int *score,
                               int *str_len1_p, int *str_len2_p) {
    int atype = abs(type);
    int l, sc1, sc2;
    const uint8_t *qual = bam_get_qual(p->b), *bq = NULL;
    uint8_t *qq;

    // Trim poly_Ns at ends of ref.
    // This helps to keep len(ref) and len(query) similar, to reduce
    // band size and reduce the chance of -ve BAQ scores.
    for (l = 0; l < tend1-tbeg && l < tend2-tbeg; l++)
        if (ref1[l + tbeg-left] != 4 || ref2[l + tbeg-left] != 4)
            break;
    if (l > atype)
        tbeg += l-atype;

    for (l = tend1-tbeg-1; l >= 0; l--)
        if (ref1[l + tbeg-left] != 4)
            break;
    l = tend1-tbeg-1 - l;
    if (l > atype)
        tend1 -= l-atype;

    for (l = tend2-tbeg-1; l >= 0; l--)
        if (ref2[l + tbeg-left] != 4)
            break;
    l = tend2-tbeg-1 - l;
    if (l > atype) {
        tend2 -= l-atype;
    }

    // Get segment of quality, either ZQ tag or if absent QUAL.
    if (!(qq = (uint8_t*) calloc(qend - qbeg, 1)))
        return -1;
    //bq = (uint8_t*)bam_aux_get(p->b, "ZQ");
    //if (bq) ++bq; // skip type
    double m = 0;
    for (l = qbeg; l < qend; ++l) {
        int qval = bq? qual[l] + (bq[l] - 64) : qual[l];
        if (qval > 30)
            qval = 30;
        if (qval < 7)
            qval = 7;
        qq[l - qbeg] = qval;
        m += qval;
    }
    m /= (qend - qbeg); // avg qual

    // Identify STRs in ref covering the indel up to
    // (or close to) the end of the sequence.
    // Those having an indel and right at the sequence
    // end do not confirm the total length of indel
    // size.  Specifically a *lack* of indel at the
    // end, where we know indels occur in other
    // sequences, is a possible reference bias.
    //
    // This is emphasised further if the sequence ends with
    // soft clipping.
    // FIXME: need to make this work on IUPAC?
    rep_ele *reps, *elt, *tmp;
    uint8_t *seg = ref2 + tbeg - left;
    int seg_len = tend2 - tbeg;
    reps = find_STR((char *)seg, seg_len, 0);
    int iscore = 0;
    double m2 = 0;
    int mn = 0, m2min = INT_MAX;
    int str_len1 = *str_len1_p, str_len2 = *str_len2_p;
    DL_FOREACH_SAFE(reps, elt, tmp) {
        if (elt->start <= qpos && elt->end >= qpos) {
            iscore += (elt->end-elt->start) / elt->rep_len;  // c
            if (str_len1 < elt->end-elt->start)
                str_len1 = elt->end-elt->start;
            if (str_len2 < (elt->end-elt->start) / elt->rep_len)
                str_len2 = (elt->end-elt->start) / elt->rep_len;
            for (l = MAX(qbeg, elt->start);
                 l < MIN(qend, elt->end);
                 l++, mn++) {
                m2 += qq[l-qbeg];
                if (m2min > qq[l-qbeg])
                    m2min = qq[l-qbeg];
            }
            if (elt->start+tbeg <= r_start ||
                elt->end+tbeg   >= r_end)
                iscore += 2*(elt->end-elt->start);
        }

        DL_DELETE(reps, elt);
        free(elt);
    }
    *str_len1_p = str_len1;
    *str_len2_p = str_len2;
    if (mn)
        m2 /= mn;
    else
        m2 = m2min = qavg;

    // The bottom 8 bits are length-normalised score while
    // the top bits are unnormalised.
    //
    // Try original cons and new cons and pick best.
    // This doesn't reduce FN much (infact maybe adds very slightly),
    // but it does reduce GT errors and is a slight reduction to FP.
    //m = MIN(30, (m2+m2min)/2); // best so far
    m = MIN(30, m2min);

#if 1
    // edlib

    //double mm = (m+m2)/2;
    //double mm = m2min;
    double mm = m;
    sc2 = edlib_glocal(ref2 + tbeg - left, tend2 - tbeg,
                       query, qend - qbeg, mm, del_bias);

    if (tend1 != tend2 ||
        memcmp((char *)ref1 + tbeg - left, (char *)ref2 + tbeg - left,
               tend1 - tbeg) != 0)
        sc1 = edlib_glocal(ref1 + tbeg - left, tend1 - tbeg,
                           query, qend - qbeg, mm, del_bias);
    else
        sc1 = INT_MAX; // skip
#endif

#if 0
    // BAQ

    int SC1, SC2;
    probaln_par_t apf = { 1e-4, 1e-2, 10 };
    if (long_read) {
        apf.d = 1e-3;
        apf.e = 1e-1;
    }

    if (band > (qend-qbeg)/2-3)
        band = (qend-qbeg)/2-3;
    apf.bw = band + 3; // or abs(l_ref - l_query), so we want to keep similar

    SC2 = probaln_glocal(ref2 + tbeg - left, tend2 - tbeg,
                         query, qend - qbeg, qq, &apf, 0, 0);

    if (tend1 != tend2 ||
        memcmp((char *)ref1 + tbeg - left, (char *)ref2 + tbeg - left,
               tend1 - tbeg) != 0)
        SC1 = probaln_glocal(ref1 + tbeg - left, tend1 - tbeg,
                             query, qend - qbeg, qq, &apf, 0, 0);
    else
        SC1 = INT_MAX; // skip

    sc1 = SC1;
    sc2 = SC2;
#endif

    // Find the best of the two alignments
    if (sc1 < 0 && sc2 < 0) {
        *score = 0xffffff;
        free(qq);
        return 0;
    }
    if (sc1 < 0) {
        // sc2 is already correct
    } else if (sc2 < 0) {
        sc2 = sc1;
    } else {
        // sc1 and sc2 both pass, so use best
        if (sc2 > sc1)
            sc2 = sc1;
    }

#if 0
    // Old indel-tweak-jkb1:bam2bcf_indel.c code
    l = (int)((100. * sc2 / (qend - qbeg) + .499) * bca->indel_bias);
    *score = sc2<<8 | MIN(255, l);
    l  =  (*score&0xff)*.8 + iscore*2;
    *score = (*score & ~0xff) | MIN(255, l);
    free(qq);
    return 0;
#endif

    // Sc is overall alignment score, in top 24 bits (SeqQ). It's based
    // purely on the scores for the whole alignment.
    // We also have a separate indel score in bottom 8 bits (IndelQ).
    // This is a function of all sorts of attributes of the candidate indel
    // itself, such as STR length and the presence of poor quality bases.

    // Used for adjusting indelQ below.  Lower l is more likely to call
    // (--FN, ++FP).  (NB CLI --indel_bias is 1/indel_bias var).
    // Starts as average score per base, and then adjusted based on seq
    // complexity / quality.

    l = .5*(100. * sc2 / (qend - qbeg) + .499);
    l += iscore*(qavg/(m2min+1.0) + qavg/m2);

    *score = (sc2<<8) | (int)MIN(255, l * bca->indel_bias/10);

    // NOTE: indel_bias now seems to have a very minimal impact on scoring.
    // Why is this so?

    free(qq);

    return 0;
}

// Part of bcf_call_gap_prep.
//
// Returns n_alt on success
//         -1 on failure

// TODO: almost identical to bam2bcf_indel.c's copy, so we could share
// the code and add a check on bca->edlib.
static int bcf_cgp_compute_indelQ(int n, int *n_plp, bam_pileup1_t **plp,
                                  bcf_callaux_t *bca, char *inscns,
                                  int l_run, int max_ins,
                                  int ref_type, int *types, int n_types,
                                  double qavg, int *score,
                                  int str_len1, int str_len2) {
    // FIXME: n_types has a maximum; no need to alloc - use a #define?
    int sc[MAX_TYPES], sumq[MAX_TYPES], s, i, j, t, K, n_alt, tmp;
    memset(sumq, 0, n_types * sizeof(int));

    // Confusing variable naming and bit usage.
    //
    // score[] is low 8  bits normalised (by len) alignment score
    //            top 24 bits full alignment score
    // This gets cast into "sct"; mnemonic score-per-indel-type.
    //
    // sc = (score<<6) | type  (index to types[] array for indel size)
    // So sc>>14 = score>>(14-6) = score>>8.  Ie full alignment score
    for (s = K = 0; s < n; ++s) {
        for (i = 0; i < n_plp[s]; ++i, ++K) {
            bam_pileup1_t *p = plp[s] + i;
            // Labelling is confusing here.
            //    sct is short for score.
            //    sc is score + t(type)
            // Why aren't these variable names reversed?
            int *sct = &score[K*n_types], seqQ, indelQ;
            for (t = 0; t < n_types; ++t) sc[t] = sct[t]<<6 | t;
            for (t = 1; t < n_types; ++t) // insertion sort
                for (j = t; j > 0 && sc[j] < sc[j-1]; --j)
                    tmp = sc[j], sc[j] = sc[j-1], sc[j-1] = tmp;

            /* errmod_cal() assumes that if the call is wrong, the
             * likelihoods of other events are equal. This is about
             * right for substitutions, but is not desired for
             * indels. To reuse errmod_cal(), I have to make
             * compromise for multi-allelic indels.
             */
            if ((sc[0]&0x3f) == ref_type) {
                // sc >> 14 is the total score.  It's been shifted by 8
                // from normalised score and 6 from type.
                indelQ = (sc[1]>>14) - (sc[0]>>14);
                // &0x3f is type number
                seqQ = est_seqQ(bca, types[sc[1]&0x3f], l_run, str_len1);
            } else {
                for (t = 0; t < n_types; ++t) // look for the reference type
                    if ((sc[t]&0x3f) == ref_type) break;
                indelQ = (sc[t]>>14) - (sc[0]>>14);
                seqQ = est_seqQ(bca, types[sc[0]&0x3f], l_run, str_len1);
            }

            // Skew SeqQ and IndelQ based on a portion of the minimum quality
            // found within a homopolymer.  This is useful where the quality
            // values are a bit mutable and move around in such data, but less
            // so on clocked sequencing technologies.
            //
            // Enabling this causes lots of GT errors on Illumina.
            // However on PacBio it's key to removal of false positives.
            // ONT and UG seem somewhere inbetween.
            if (bca->poly_mqual) {
                int qpos = p->qpos, l;
                uint8_t *seq = bam_get_seq(p->b);
                uint8_t *qual = bam_get_qual(p->b);
                int min_q = qual[qpos];

                // scan homopolymer left
                char baseL = bam_seqi(seq, qpos+1 < p->b->core.l_qseq
                                      ? qpos+1 : qpos);
                for (l = qpos; l >= 0; l--) {
                    if (bam_seqi(seq, l) != baseL)
                        break;
                    if (min_q > qual[l])
                        min_q = qual[l];
                }

                // scan homo-polymer right (including site of indel)
                char base = bam_seqi(seq, qpos+1);
                for (l = qpos+1; l < p->b->core.l_qseq; l++) {
                    if (min_q > qual[l])
                        min_q = qual[l];
                    if (bam_seqi(seq, l) != base)
                        break;
                }

                // We reduce -h so homopolymers get reduced likelihood of being
                // called, but then optionally increase or decrease from there
                // based on base quality.  Hence lack of low quality bases in
                // homopolymer will rescue the score back again, reducing FNs.

                // The score factors here may also be machine specific, but for
                // now these work well (tuned on PB HiFi).
                seqQ   += MIN(qavg/20,  min_q - qavg/10);
                indelQ += MIN(qavg/20,  min_q - qavg/5);

                if (seqQ   < 0) seqQ   = 0;
                if (indelQ < 0) indelQ = 0;
            }

            // This is the length-normalised score from bcf_cgp_align_score
            tmp = sc[0]>>6 & 0xff;

            // reduce indelQ
            // high score = bad, low score = good.
            // low normalised scores leave indelQ unmodified
            // high normalised scores set indelQ to 0
            // inbetween scores have a linear scale from indelQ to 0
            indelQ = tmp > 111? 0 : (int)((1. - tmp/111.) * indelQ + .499);

            // Doesn't really help accuracy, but permits -h to take
            // affect still.
            if (indelQ > seqQ) indelQ = seqQ;
            if (indelQ > 255) indelQ = 255;
            if (seqQ > 255) seqQ = 255;

            // use 22 bits in total
            p->aux = (sc[0]&0x3f)<<16 | seqQ<<8 | indelQ;
            sumq[sc[0]&0x3f] += indelQ;
        }
    }
    // determine bca->indel_types[] and bca->inscns
    bca->maxins = max_ins;
    bca->inscns = (char*) realloc(bca->inscns, bca->maxins * 4);
    if (bca->maxins && !bca->inscns)
        return -1;
    for (t = 0; t < n_types; ++t)
        sumq[t] = sumq[t]<<6 | t;
    for (t = 1; t < n_types; ++t) // insertion sort
        for (j = t; j > 0 && sumq[j] > sumq[j-1]; --j)
            tmp = sumq[j], sumq[j] = sumq[j-1], sumq[j-1] = tmp;
    for (t = 0; t < n_types; ++t) // look for the reference type
        if ((sumq[t]&0x3f) == ref_type) break;
    if (t) { // then move the reference type to the first
        tmp = sumq[t];
        for (; t > 0; --t) sumq[t] = sumq[t-1];
        sumq[0] = tmp;
    }
    for (t = 0; t < 4; ++t) bca->indel_types[t] = B2B_INDEL_NULL;
    for (t = 0; t < 4 && t < n_types; ++t) {
        bca->indel_types[t] = types[sumq[t]&0x3f];
        if (bca->maxins)
            memcpy(&bca->inscns[t * bca->maxins],
                   &inscns[(sumq[t]&0x3f) * max_ins], bca->maxins);
    }
    // update p->aux
    for (s = n_alt = 0; s < n; ++s) {
        for (i = 0; i < n_plp[s]; ++i) {
            bam_pileup1_t *p = plp[s] + i;
            int x = types[p->aux>>16&0x3f];
            for (j = 0; j < 4; ++j)
                if (x == bca->indel_types[j]) break;
            p->aux = j<<16 | (j == 4? 0 : (p->aux&0xffff));
            if ((p->aux>>16&0x3f) > 0) ++n_alt;
            //fprintf(stderr, "X pos=%d read=%d:%d name=%s call=%d type=%d seqQ=%d indelQ=%d\n", pos, s, i, bam_get_qname(p->b), (p->aux>>16)&0x3f, bca->indel_types[(p->aux>>16)&0x3f], (p->aux>>8)&0xff, p->aux&0xff);
        }
    }

    return n_alt;
}

/*
FIXME: with high number of samples, do we handle IMF correctly?  Is it
fraction of indels across entire data set, or just fraction for this
specific sample? Needs to check bca->per_sample_flt (--per-sample-mF) opt.
 */

/*
    notes:
    - n .. number of samples
    - the routine sets bam_pileup1_t.aux of each read as follows:
        - 6: unused
        - 6: the call; index to bcf_callaux_t.indel_types   .. (aux>>16)&0x3f
        - 8: estimated sequence quality                     .. (aux>>8)&0xff
        - 8: indel quality                                  .. aux&0xff
 */
int bcf_edlib_gap_prep(int n, int *n_plp, bam_pileup1_t **plp, int pos,
		       bcf_callaux_t *bca, const char *ref, int ref_len)
{
    if (ref == 0 || bca == 0) return -1;

    int i, s, t, n_types, *types = NULL, max_rd_len, left, right, max_ins;
    int *score = NULL;
    int N, K, l_run, ref_type, n_alt = -1;
    char *inscns = NULL, *query = NULL;

    // determine if there is a gap
    for (s = N = 0; s < n; ++s) {
        for (i = 0; i < n_plp[s]; ++i)
            if (plp[s][i].indel != 0) break;
        if (i < n_plp[s]) break;
    }
    if (s == n)
        // there is no indel at this position.
        return -1;

    // Find average base quality over this region
    double qavg = 30, qsum = 0, qcount = 0;
    int qmax = 0;
    for (s = 0; s < n; s++) {
        for (i = 0; i < n_plp[s]; i++) {
#define QWIN 50
            bam_pileup1_t *p = plp[s] + i;
            int kstart = p->qpos - QWIN > 0 ? p->qpos - QWIN : 0;
            int kend = p->qpos + QWIN < p->b->core.l_qseq
                ? p->qpos + QWIN : p->b->core.l_qseq;
            uint8_t *qual = bam_get_qual(p->b);
            int k;
            for (k = kstart; k < kend; k++) {
                qsum += qual[k];
                qcount++;
                if (qmax < qual[k])
                    qmax = qual[k];
            }
        }
    }
    qavg = (qsum+1) / (qcount+1);
    //qavg = (qavg + qmax)/2; // bias avg toward maximum observed.

    // find out how many types of indels are present
    types = bcf_cgp_find_types(n, n_plp, plp, pos, bca, ref,
                               &max_rd_len, &n_types, &ref_type, &N);
    if (!types)
        goto err;


    // calculate left and right boundary
#if 0
    left = pos > bca->indel_win_size ? pos - bca->indel_win_size : 0;
    right = pos + bca->indel_win_size;
#else
    int max_indel = 20*MAX(ABS(types[0]), ABS(types[n_types-1]))
                  + bca->indel_win_size/4;
    if (max_indel > bca->indel_win_size)
        max_indel = bca->indel_win_size;
    left = pos > max_indel ? pos - max_indel : 0;
    right = pos + max_indel;
#endif

    int del_size = types[0]<0 ? -types[0] : 0;
    right += del_size;

    // in case the alignments stand out the reference
    for (i = pos; i < right; ++i)
        if (ref[i] == 0) break;
    right = i;

    // compute the likelihood given each type of indel for each read
    max_ins = types[n_types - 1];   // max_ins is at least 0

    // The length of the homopolymer run around the current position
    l_run = bcf_cgp_l_run(ref, pos);
    int l_run_base = seq_nt16_table[(uint8_t)ref[pos+1]];
    int l_run_ins = 0;

    // construct the consensus sequence (minus indels, which are added later)
    if (max_ins > 0) {
        // TODO: replace filling inscns[] with calc_consensus return
        // so the merges of the insertion consensus for type[t] is
        // reported directly.  (It may need adjustment to avoid N)
        inscns = bcf_cgp_calc_ins_cons(n, n_plp, plp, pos,
                                       types, n_types, max_ins, s);
        if (!inscns)
            return -1;
    }

    query = (char*) calloc(right - left + max_rd_len + max_ins + 2, 1);
    score = (int*) calloc(N * n_types, sizeof(int));
    bca->indelreg = 0;
    double nqual_over_60 = bca->nqual / 60.0;

    int biggest_del = 0;
    int biggest_ins = 0;
    for (t = 0; t < n_types; t++) {
        if (biggest_del > types[t])
            biggest_del = types[t];
        if (biggest_ins < types[t])
            biggest_ins = types[t];
    }
    int band = biggest_ins - biggest_del; // NB del is -ve

    int str_len1 = l_run, str_len2 = l_run/4;
    for (t = 0; t < n_types; ++t) {
        int l, ir;

        // Compute indelreg.  This is the context in the reference.  Eg:
        //
        // REF:  AG--TTTC  Inscns   is "TT".
        // SEQ:  AGTTTTTC  Indelreg is 3; next 3 "TTT" bases
        //
        // => GTTT GTTTTT is call.
        if (types[t] == 0)
            ir = 0;
        else if (types[t] > 0)
            ir = est_indelreg(pos, ref, types[t], &inscns[t*max_ins]);
        else
            ir = est_indelreg(pos, ref, -types[t], 0);

        if (ir > bca->indelreg)
            bca->indelreg = ir;

        // Realignment score, computed via BAQ
        for (s = K = 0; s < n; ++s) {
            char **tcons;
            int left_shift, right_shift;
            int tcon_len[2];
            int cpos_pos;
            tcons = bcf_cgp_consensus(n, n_plp, plp, pos, bca, ref, ref_len,
                                      left, right, s, types[t], biggest_del,
                                      &left_shift, &right_shift, &band,
                                      tcon_len, &cpos_pos);
            // TODO: Consensus for a deletion shouldn't match the
            // consensus for type 0.  Eg consider
            //         vv                          vv
            // REF:  AATGTGTGAACAA        REF:   AATGTG--AACAA
            // T0:   AATGTG--AACAA        T0:    AATGTG--AACAA
            // T-2:  AA--TGTGAATAA        T-2:   AA--TGTGAATAA:
            //
            // On left: both T0 and T-2 are the same length, as it's
            // just a deletion that moved.  We may end up assigning
            // reads to an indel allele based on the SNP they have and
            // not the actual indel.
            // There *is* a deletion here though, but only 1.  How do
            // we call it once only?  Need to replace entire region
            // with a reassembly.
            //
            // On right: T0 and T-2 have same length again, but there
            // isn't an indel as it's ins+del vs del+ins. They're
            // also the same length as the REF for this region.
            // Hence likelihood of this variant existing is tied in
            // with their equal and high similarity with/to the ref.
            //
            // We could do an alignment of tcons[0] and tcons[1] and check
            // whether their differences are consistent with (ie the
            // hamming distance is at least ABS(types[t]/2).  I don't think
            // it'll rescue many FPs though.

#ifdef CONS_DEBUG
            {
                int j;
                for (j = 0; j < 2; j++) {
                    int k;
                    fprintf(stderr, "Cons%d @ %d %4d/%4d ",
                            j, pos, types[t], left_shift);
                    for (k = 0; k < tcon_len[j]; k++) {
                        if (k == cpos_pos)
                            putc('#', stderr);
                        putc("ACGTN"[(uint8_t)tcons[j][k]], stderr);
                    }
                    putc('\n', stderr);
                }
            }
#endif

            // Scan for base-runs in the insertion.
            // We use this to avoid over-correction in est_seqQ when the
            // insertion is not part of the neighbouring homopolymer.
            int k = tcons[0][cpos_pos], j;
            for (j = 0; j < types[t]; j++)
                if (tcons[0][cpos_pos+j] != k)
                    break;
            if (j && j == types[t])
                l_run_ins |= "\x1\x2\x4\x8\xf"[k]; // ACGTN
            if (types[t] < 0)
                l_run_ins |= 0xff;

            // align each read to consensus(es)
            for (i = 0; i < n_plp[s]; ++i, ++K) {
                bam_pileup1_t *p = plp[s] + i;

                // Some basic ref vs alt stats.
                int imq = p->b->core.qual > 59 ? 59 : p->b->core.qual;
                imq *= nqual_over_60;

                int sc_len, slen, epos, sc_end;

                // Only need to gather stats on one type, as it's
                // identical calculation for all the subsequent ones
                // and we're sharing the same stats array
                if (t == 0) {
                    // Gather stats for INFO field to aid filtering.
                    // mq and sc_len not very helpful for filtering, but could
                    // help in assigning a better QUAL value.
                    //
                    // Pos is slightly useful.
                    // Base qual can be useful, but need qual prior to BAQ?
                    // May need to cache orig quals in aux tag so we can fetch
                    // them even after mpileup step.
                    get_pos(bca, p, &sc_len, &slen, &epos, &sc_end);

                    assert(imq >= 0 && imq < bca->nqual);
                    assert(epos >= 0 && epos < bca->npos);
                    assert(sc_len >= 0 && sc_len < 100);
                    if (p->indel) {
                        bca->ialt_mq[imq]++;
                        bca->ialt_scl[sc_len]++;
                        bca->ialt_pos[epos]++;
                    } else {
                        bca->iref_mq[imq]++;
                        bca->iref_scl[sc_len]++;
                        bca->iref_pos[epos]++;
                    }
                }

                int qbeg, qpos, qend, tbeg, tend, kk;
                uint8_t *seq = bam_get_seq(p->b);
                uint32_t *cigar = bam_get_cigar(p->b);
                if (p->b->core.flag & BAM_FUNMAP) continue;

                // FIXME: the following loop should be better moved outside;
                // nonetheless, realignment should be much slower anyway.
                for (kk = 0; kk < p->b->core.n_cigar; ++kk)
                    if ((cigar[kk]&BAM_CIGAR_MASK) == BAM_CREF_SKIP)
                        break;
                if (kk < p->b->core.n_cigar)
                    continue;

                // determine the start and end of sequences for alignment
                int left2 = left, right2 = right;
                int min_win_size = MAX(-biggest_del, biggest_ins);
                min_win_size += ABS(left_shift) + ABS(right_shift);
                {
                    rep_ele *reps, *elt, *tmp;
                    reps = find_STR(tcons[0], tcon_len[0], 0);
                    //int max_str = 0;
                    int tot_str = 0;
                    DL_FOREACH_SAFE(reps, elt, tmp) {
                        // if (max_str < elt->end - elt->start)
                        //     max_str = elt->end - elt->start;
                        tot_str += elt->end - elt->start;
                        DL_DELETE(reps, elt);
                        free(elt);
                    }

                    // Ideally max_str should be enough, but it's still not
                    // sufficient in longer range some repeats.
                    //min_win_size += max_str;
                    min_win_size += tot_str;
                }
                min_win_size += 10;
                if (p->b->core.l_qseq > 1000) { // ||1 for 7f-long
                    // long read data needs less context.  It also tends to
                    // have many more candidate indels to investigate so
                    // speed here matters more.
                    if (pos - left >= min_win_size)
                        left2 = MAX(left2, pos - min_win_size);
                    if (right-pos >= min_win_size)
                        right2 = MIN(right2, pos + min_win_size);
                }

                // Genomic coords for first and last base of query
                // alignment.  This is only used in bcf_cgp_align_score
                // for computing scores by looking for the proximity
                // of STRs with the end of the query alignment.
                int r_start = p->b->core.pos;
                int r_end = bam_cigar2rlen(p->b->core.n_cigar,
                                           bam_get_cigar(p->b))
                            -1 + r_start;

                // Map left2/right2 genomic coordinates to qbeg/qend
                // query coordinates.  The query may not span the
                // entire left/right region, so this also returns the
                // equivalent genomic coords for qbeg/qend in tbeg/tend.
                qbeg = tpos2qpos(&p->b->core, bam_get_cigar(p->b),
                                 left2, 0, &tbeg);
                qpos = tpos2qpos(&p->b->core, bam_get_cigar(p->b), pos,
                                     0, &tend) - qbeg;
                qend = tpos2qpos(&p->b->core, bam_get_cigar(p->b),
                                 right2, 1, &tend);

                int old_tend = tend;
                int old_tbeg = tbeg;

                // write the query sequence
                for (l = qbeg; l < qend; ++l)
                    query[l - qbeg] = seq_nt16_int[bam_seqi(seq, l)];

                // A fudge for now.  Consider checking SAM header for
                // RG platform field.
                int long_read = p->b->core.l_qseq > 1000;

                // tbeg and tend are the genomic locations equivalent
                // to qbeg and qend on the sequence.
                // These may being entirely within our left/right
                // coordinates over which we've computed the
                // consensus, or overlapping to left/right.
                //
                // We know an estimation of band, plus biggest indel,
                // so we can trim tbeg/tend to a smaller region if we
                // wish here.  This speeds up BAQ scoring.
                int wband = band + MAX(-biggest_del, biggest_ins)*2 + 20;
                int tend1 = left + tcon_len[0] - (left2-left);
                int tend2 = left + tcon_len[1] - (left2-left);
                tend1 = MIN(tend1, old_tend + wband);
                tend2 = MIN(tend2, old_tend + wband);
                tbeg = MAX(left2, old_tbeg - wband);

                // do realignment; this is the bottleneck.
                //
                // Note low score = good, high score = bad.
                if (tend > tbeg) {
                    //fprintf(stderr, "Num %d\n", i);
                    if (bcf_cgp_align_score(p, bca, types[t], band,
                                            (uint8_t *)tcons[0] + left2-left,
                                            (uint8_t *)tcons[1] + left2-left,
                                            (uint8_t *)query,
                                            r_start, r_end, long_read,
                                            tbeg, tend1, tend2,
                                            left2, left + tcon_len[0],
                                            qbeg, qend, pos,qpos, -biggest_del,
                                            qavg, bca->del_bias,
                                            &score[K*n_types + t],
                                            &str_len1, &str_len2) < 0) {
                        goto err;
                    }
                } else {
                    // place holder large cost for reads that cover the
                    // region entirely within a deletion (thus tend < tbeg).
                    score[K*n_types + t] = 0xffffff;
                }
            }
            free(tcons);
        }
    }

    // compute indelQ
    if (!(l_run_base & l_run_ins))
        l_run = 1; // different base type in ins to flanking region.
    n_alt = bcf_cgp_compute_indelQ(n, n_plp, plp, bca, inscns, l_run, max_ins,
                                   ref_type, types, n_types, qavg, score,
                                   str_len1, str_len2);

 err:
    // free
    free(query);
    free(score);
    free(types);
    free(inscns);

    return n_alt > 0? 0 : -1;
}