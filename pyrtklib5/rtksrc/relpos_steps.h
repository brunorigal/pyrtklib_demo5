/*------------------------------------------------------------------------------
* relpos_steps.h : step-by-step relative positioning API (vrsgen extension)
*
*          Kept out of rtklib.h on purpose: gen_rtk.py parses rtklib.h only, so
*          nothing here is ever auto-bound twice (relpos_ctx_t) or mis-parsed
*          (the three int[MAXSAT] declarators). The pybind11 layer includes this
*          header explicitly, inside a HANDMERGE block.
*-----------------------------------------------------------------------------*/
#ifndef RELPOS_STEPS_H
#define RELPOS_STEPS_H

#include "rtklib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* relpos step-by-step context -----------------------------------------------*/
typedef struct {
    rtk_t *rtk;
    const nav_t *nav;
    int nu, nr, ns, nf, ny, nv, niter;
    int stat;                 /* SOLQ_??? carried between steps: SOLQ_DGPS is a
                                 real value here (demo5 gives it to a float
                                 solution with fewer than 4 valid phases) */
    int epoch_pending;        /* set by relpos_init, consumed by relpos_free:
                                 rtk->epoch++ happens exactly once per relpos
                                 invocation, as rtkpos() does after relpos() */
    double dt;
    double *rs, *dts, *var, *y, *e, *azel, *freq;
    double *v, *H, *R, *xp, *Pp, *xa, *bias;
    int sat[MAXSAT];
    int iu[MAXSAT];
    int ir[MAXSAT];
    int vflg[MAXOBS*NFREQ*2+1];
    int svh[MAXOBS*2];
} relpos_ctx_t;

/* everything rtkpos() does before relpos(): base position, SPP, time sync.
   return : 1 = proceed with the relpos steps, 0 = no relative solution this
            epoch (rtkpos() would not have called relpos()) */
int rtkpos_pre_relpos(rtk_t *rtk, const obsd_t *obs, int n, const nav_t *nav);

/* the body of relpos(), one call per stage. Return conventions:
   relpos_zdres_base : 1 ok, 0 base position error, 2 age of differential
                       exceeded (relpos() returns 1 there, without a solution)
   relpos_selsat     : number of common satellites (<= 0: none)
   relpos_float_filter : 1 = float solution valid (SOLQ_FLOAT, AR may run),
                       0 = otherwise; read ctx->stat to tell SOLQ_DGPS
                       (solution to be saved) from SOLQ_NONE
   relpos_ambiguity_resolution : number of fixed DD ambiguities (0: not fixed) */
int relpos_init(relpos_ctx_t *ctx, rtk_t *rtk, const obsd_t *obs, int n,
                const nav_t *nav);
int relpos_satpos(relpos_ctx_t *ctx, const obsd_t *obs);
int relpos_zdres_base(relpos_ctx_t *ctx, const obsd_t *obs);
int relpos_selsat(relpos_ctx_t *ctx, const obsd_t *obs);
void relpos_udstate(relpos_ctx_t *ctx, const obsd_t *obs);
int relpos_float_filter(relpos_ctx_t *ctx, const obsd_t *obs);
int relpos_ambiguity_resolution(relpos_ctx_t *ctx, const obsd_t *obs);
void relpos_save_solution(relpos_ctx_t *ctx, const obsd_t *obs);
void relpos_free(relpos_ctx_t *ctx);

/* state-vector layout, straight from rtkpos.c's macros (never re-derived) */
int relpos_ib_index(int sat, int freq, const prcopt_t *opt);
int relpos_nr_index(const prcopt_t *opt);

void relpos_extract_sat_data(const relpos_ctx_t *ctx,
    const double *rover_ecef, const double *base_ecef, int flags,
    double *out_el_deg, double *out_az_deg,
    double *out_sat_pos, double *out_sat_vel,
    double *out_sat_clk, double *out_sat_clk_drift,
    double *out_los, double *out_geom_range, double *out_sagnac,
    double *out_tropo, double *out_iono, double *out_phw,
    double *out_base_geom_range, double *out_base_el_deg, double *out_base_az_deg,
    double *out_float_amb, double *out_wl,
    double *out_resc, double *out_resp, double *out_fix, double *out_lock,
    double *out_slip, double *out_snr,
    double *out_rover_dant, double *out_base_dant,
    double *out_base_tropo, double *out_base_iono,
    double *out_icbias);
void relpos_extract_fixed_amb(const relpos_ctx_t *ctx,
    double *out_fixed_amb, double *out_fix_flags);

#ifdef __cplusplus
}
#endif
#endif /* RELPOS_STEPS_H */
