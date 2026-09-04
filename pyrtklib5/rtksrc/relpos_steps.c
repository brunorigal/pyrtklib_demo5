/*------------------------------------------------------------------------------
* relpos_steps.c : step-by-step relative positioning (vrsgen extension, demo5)
*
*          relpos() and rtkpos() of rtkpos.c, cut into stages so Python can
*          read the intermediate state (float states, DD residuals, fixed
*          ambiguities, ...) between them. Every stage reproduces the exact
*          control flow of the demo5 relpos() it was derived from - the parity
*          test in vrsgen (tests_rtklib_train/test_relpos_processor.py) checks
*          it against rtkpos() epoch by epoch.
*
*          rtkpos.c is #included here ("unity include") instead of being
*          compiled on its own: its static helpers (selsat, udstate, zdres,
*          ddres, intpres, manage_amb_LAMBDA, holdamb, valpos, initx, errmsg)
*          and its state-layout macros (NF, NP, NR, IB, ...) become visible
*          without removing a single `static` upstream. CMakeLists.txt drops
*          rtkpos.c from the source list accordingly; this file is the only
*          translation unit that defines rtkinit/rtkpos/... .
*-----------------------------------------------------------------------------*/
#include "rtkpos.c"
#include "relpos_steps.h"
#include <string.h>

/* state-vector layout helpers -----------------------------------------------*/
int relpos_ib_index(int sat, int freq, const prcopt_t *opt)
{
    return IB(sat,freq,opt);
}
int relpos_nr_index(const prcopt_t *opt)
{
    return NR(opt);
}

/* pre-relpos processing (from rtkpos): base setup, SPP, time sync ----------
*  Everything rtkpos() does before calling relpos(), demo5 flavour: the SPP
*  variance gate (STD_PREC_VAR_THRESH), the static-start reset after a 5 min
*  gap, and the moving-base position filter. The age-of-differential check
*  for the non moving-base modes lives in relpos() here (relpos_zdres_base).
*  Returns: 1 = rtkpos() would go on to relpos(), 0 = it would not.
*---------------------------------------------------------------------------*/
int rtkpos_pre_relpos(rtk_t *rtk, const obsd_t *obs, int n, const nav_t *nav)
{
    prcopt_t *opt=&rtk->opt;
    sol_t solb={{0}};
    gtime_t time;
    int i,nu,nr;
    char msg[128]="";

    /* set base station position */
    if (opt->refpos<=POSOPT_RINEX&&opt->mode!=PMODE_SINGLE&&
        opt->mode!=PMODE_MOVEB) {
        for (i=0;i<6;i++) rtk->rb[i]=i<3?opt->rb[i]:0.0;
    }
    /* count rover/base station observations */
    for (nu=0;nu   <n&&obs[nu   ].rcv==1;nu++) ;
    for (nr=0;nu+nr<n&&obs[nu+nr].rcv==2;nr++) ;

    time=rtk->sol.time; /* previous epoch */

    /* rover position and time by single point positioning, skip if
     position variance smaller than threshold */
    if (rtk->P[0]==0||rtk->P[0]>STD_PREC_VAR_THRESH) {
        if (!pntpos(obs,nu,nav,&rtk->opt,&rtk->sol,NULL,rtk->ssat,msg)) {
            errmsg(rtk,"point pos error (%s)\n",msg);

            if (!rtk->opt.dynamics) {
                return 0;
            }
        }
    } else rtk->sol.time=obs[0].time;
    if (time.time!=0) rtk->tt=timediff(rtk->sol.time,time);

    /* return to static start if long delay without rover data */
    if (fabs(rtk->tt)>300&&rtk->initial_mode==PMODE_STATIC_START) {
        rtk->opt.mode=PMODE_STATIC_START;
        for (i=0;i<3;i++) initx(rtk,rtk->sol.rr[i],VAR_POS,i);
        if (rtk->opt.dynamics) {
            for (i=3;i<6;i++) initx(rtk,1E-6,VAR_VEL,i);
            for (i=6;i<9;i++) initx(rtk,1E-6,VAR_ACC,i);
        }
        trace(3,"No data for > 5 min: switch back to static mode:\n");
    }

    /* single point positioning */
    if (opt->mode==PMODE_SINGLE) return 1;

    /* suppress output of single solution */
    if (!opt->outsingle) {
        rtk->sol.stat=SOLQ_NONE;
    }
    /* precise point positioning: not supported by the step API */
    if (opt->mode>=PMODE_PPP_KINEMA) return 0;

    /* check number of data of base station */
    if (nr==0) {
        errmsg(rtk,"no base station observation data for rtk\n");
        return 0;
    }
    if (opt->mode==PMODE_MOVEB) { /*  moving baseline */
        /* estimate position/velocity of base station,
           skip if position variance below threshold*/
        if (rtk->P[0]==0||rtk->P[0]>STD_PREC_VAR_THRESH) {
            if (!pntpos(obs+nu,nr,nav,&rtk->opt,&solb,NULL,NULL,msg)) {
                errmsg(rtk,"base station position error (%s)\n",msg);
                return 0;
            }
            /* if base position uninitialized, use full position */
            if (fabs(rtk->rb[0])<0.1)
                for (i=0;i<3;i++) rtk->rb[i]=solb.rr[i];
            /* else filter base position to reduce noise from single precision solution */
            else
                for (i=0;i<3;i++) {
                    rtk->rb[i]=0.95*rtk->rb[i]+0.05*solb.rr[i];
                    rtk->rb[i+3]=0; /* set velocity to zero */
                }
        } else solb.time=obs[nu].time;

        rtk->sol.age=(float)timediff(rtk->sol.time,solb.time);

        if (fabs(rtk->sol.age)>MIN(TTOL_MOVEB,opt->maxtdiff)) {
            errmsg(rtk,"time sync error for moving-base (age=%.1f)\n",rtk->sol.age);
            return 0;
        }
    }
    return 1;
}

/* initialize relpos context -------------------------------------------------
*  The prologue of relpos(): working arrays and the satellite status reset.
*  Call once per epoch before the other step functions.
*---------------------------------------------------------------------------*/
int relpos_init(relpos_ctx_t *ctx, rtk_t *rtk, const obsd_t *obs, int n,
                const nav_t *nav)
{
    prcopt_t *opt=&rtk->opt;
    int i,j,nu,nr;

    memset(ctx,0,sizeof(relpos_ctx_t));
    ctx->rtk=rtk;
    ctx->nav=nav;
    ctx->epoch_pending=1;

    ctx->nf=opt->ionoopt==IONOOPT_IFLC?1:opt->nf;
    ctx->stat=rtk->opt.mode<=PMODE_DGPS?SOLQ_DGPS:SOLQ_FLOAT;

    /* count rover/base observations */
    for (nu=0;nu   <n&&obs[nu   ].rcv==1;nu++) ;
    for (nr=0;nu+nr<n&&obs[nu+nr].rcv==2;nr++) ;
    ctx->nu=nu;
    ctx->nr=nr;
    n=nu+nr;

    /* define local matrices, n=total observations, base + rover */
    ctx->rs  =mat(6,n);
    ctx->dts =mat(2,n);
    ctx->var =mat(1,n);
    ctx->y   =mat(ctx->nf*2,n);
    ctx->e   =mat(3,n);
    ctx->azel=zeros(2,n);
    ctx->freq=zeros(ctx->nf,n);

    /* init satellite status arrays */
    for (i=0;i<MAXSAT;i++) {
        rtk->ssat[i].sys=satsys(i+1,NULL); /* gnss system */
        for (j=0;j<NFREQ;j++) {
            rtk->ssat[i].vsat[j]=0;  /* valid satellite */
            rtk->ssat[i].snr_rover[j]=0;
            rtk->ssat[i].snr_base[j] =0;
        }
    }
    /* provisional; relpos_zdres_base sets the value relpos() uses */
    if (nu>0&&nr>0) ctx->dt=timediff(obs[0].time,obs[nu].time);

    return 1;
}

/* step 1: compute satellite positions/clocks for base and rover ------------*/
int relpos_satpos(relpos_ctx_t *ctx, const obsd_t *obs)
{
    int n=ctx->nu+ctx->nr;
    prcopt_t *opt=&ctx->rtk->opt;

    satposs(obs[0].time,obs,n,ctx->nav,opt->sateph,ctx->rs,ctx->dts,ctx->var,
            ctx->svh);
    return 1;
}

/* step 2: undifferenced residuals for base station, base/rover time diff ---
*  Returns: 1 ok, 0 base position error (relpos() returns 0),
*           2 age of differential exceeded (relpos() returns 1, no solution).
*---------------------------------------------------------------------------*/
int relpos_zdres_base(relpos_ctx_t *ctx, const obsd_t *obs)
{
    rtk_t *rtk=ctx->rtk;
    prcopt_t *opt=&rtk->opt;
    gtime_t time=obs[0].time;
    int nu=ctx->nu,nr=ctx->nr,nf=ctx->nf;

    if (!zdres(1,obs+nu,nr,ctx->rs+nu*6,ctx->dts+nu*2,ctx->var+nu,ctx->svh+nu,
               ctx->nav,rtk->rb,opt,ctx->y+nu*nf*2,ctx->e+nu*3,ctx->azel+nu*2,
               ctx->freq+nu*nf)) {
        errmsg(rtk,"initial base station position error\n");
        return 0;
    }
    /* time diff between base and rover observations */
    if (opt->intpref) {
        /* time-interpolation of base residuals (state lives in rtk_t) */
        ctx->dt=intpres(time,obs+nu,nr,ctx->nav,rtk,ctx->y+nu*nf*2);
    } else ctx->dt=timediff(time,obs[nu].time);

    if (opt->mode!=PMODE_MOVEB) {
        /* check if exceeded max age of differential */
        rtk->sol.age=ctx->dt;
        if (fabs(rtk->sol.age)>opt->maxtdiff) {
            errmsg(rtk,"age of differential error (age=%.1f)\n",rtk->sol.age);
            return 2;
        }
    }
    return 1;
}

/* step 3: select common satellites between rover and base -------------------*/
int relpos_selsat(relpos_ctx_t *ctx, const obsd_t *obs)
{
    rtk_t *rtk=ctx->rtk;
    prcopt_t *opt=&rtk->opt;

    ctx->ns=selsat(obs,ctx->azel,ctx->nu,ctx->nr,opt,ctx->sat,ctx->iu,ctx->ir);
    if (ctx->ns<=0) {
        errmsg(rtk,"no common satellite\n");
        return 0;
    }
    /* filter working arrays (relpos() allocates them after udstate; they are
       not read before relpos_udstate fills them, so the order is immaterial) */
    ctx->xp  =mat(rtk->nx,1);
    ctx->Pp  =zeros(rtk->nx,rtk->nx);
    ctx->xa  =mat(rtk->nx,1);
    ctx->bias=mat(rtk->nx,1);

    ctx->ny=ctx->ns*ctx->nf*2+2;
    ctx->v=mat(ctx->ny,1);
    ctx->H=zeros(rtk->nx,ctx->ny);
    ctx->R=mat(ctx->ny,ctx->ny);

    return ctx->ns;
}

/* step 4: temporal update of states -----------------------------------------
*  Also records the rover/base SNR in ssat *before* the filter, as relpos()
*  does: ddres() passes them to varerr() (SNR weighting, err[6]).
*---------------------------------------------------------------------------*/
void relpos_udstate(relpos_ctx_t *ctx, const obsd_t *obs)
{
    rtk_t *rtk=ctx->rtk;
    prcopt_t *opt=&rtk->opt;
    int i,j,nf=ctx->nf;

    udstate(rtk,obs,ctx->sat,ctx->iu,ctx->ir,ctx->ns,ctx->nav);

    for (i=0;i<ctx->ns;i++) for (j=0;j<nf;j++) {
        /* snr of base and rover receiver */
        rtk->ssat[ctx->sat[i]-1].snr_rover[j]=obs[ctx->iu[i]].SNR[j];
        rtk->ssat[ctx->sat[i]-1].snr_base[j] =obs[ctx->ir[i]].SNR[j];
    }
    /* initialize Pp,xa to zero, xp to rtk->x (Pp is copied once, before the
       iterations - not once per iteration as in RTKLIB 2.4.3) */
    matcpy(ctx->xp,rtk->x,rtk->nx,1);
    matcpy(ctx->Pp,rtk->P,rtk->nx,rtk->nx);

    ctx->niter=opt->niter;
}

/* step 5: iterative Kalman filter (float solution) --------------------------
*  Returns: 1 if the float solution is valid (ctx->stat == SOLQ_FLOAT, AR may
*  run), 0 otherwise. ctx->stat then tells SOLQ_DGPS (fewer than 4 valid
*  phases: the states were updated and the solution is saved as DGPS) from
*  SOLQ_NONE (filter failure).
*---------------------------------------------------------------------------*/
int relpos_float_filter(relpos_ctx_t *ctx, const obsd_t *obs)
{
    rtk_t *rtk=ctx->rtk;
    prcopt_t *opt=&rtk->opt;
    int i,f,nv,info,nu=ctx->nu,ns=ctx->ns,nf=ctx->nf;

    for (i=0;i<ctx->niter;i++) {
        /* zero diff residuals for rover (phase and code) */
        if (!zdres(0,obs,nu,ctx->rs,ctx->dts,ctx->var,ctx->svh,ctx->nav,ctx->xp,
                   opt,ctx->y,ctx->e,ctx->azel,ctx->freq)) {
            errmsg(rtk,"rover initial position error\n");
            ctx->stat=SOLQ_NONE;
            break;
        }
        /* double-differenced residuals and partial derivatives */
        if ((nv=ddres(rtk,obs,ctx->dt,ctx->xp,ctx->Pp,ctx->sat,ctx->y,ctx->e,
                      ctx->azel,ctx->freq,ctx->iu,ctx->ir,ns,ctx->v,ctx->H,
                      ctx->R,ctx->vflg))<4) {
            errmsg(rtk,"not enough double-differenced residual, n=%d\n",nv);
            ctx->stat=SOLQ_NONE;
            break;
        }
        /* kalman filter measurement update */
        if ((info=filter(ctx->xp,ctx->Pp,ctx->H,ctx->v,ctx->R,rtk->nx,nv))) {
            errmsg(rtk,"filter error (info=%d)\n",info);
            ctx->stat=SOLQ_NONE;
            break;
        }
    }
    /* zero diff residuals again after kalman filter update */
    if (ctx->stat!=SOLQ_NONE&&zdres(0,obs,nu,ctx->rs,ctx->dts,ctx->var,ctx->svh,
                                    ctx->nav,ctx->xp,opt,ctx->y,ctx->e,ctx->azel,
                                    ctx->freq)) {

        /* double diff residuals again after kalman filter update for float solution */
        ctx->nv=ddres(rtk,obs,ctx->dt,ctx->xp,ctx->Pp,ctx->sat,ctx->y,ctx->e,
                      ctx->azel,ctx->freq,ctx->iu,ctx->ir,ns,ctx->v,NULL,ctx->R,
                      ctx->vflg);

        /* validation of float solution, always returns 1, msg to trace file if large residual */
        if (valpos(rtk,ctx->v,ctx->R,ctx->vflg,ctx->nv,4.0)) {

            /* copy states */
            matcpy(rtk->x,ctx->xp,rtk->nx,1);
            matcpy(rtk->P,ctx->Pp,rtk->nx,rtk->nx);

            /* update valid satellite status for ambiguity control */
            rtk->sol.ns=0;
            for (i=0;i<ns;i++) for (f=0;f<nf;f++) {
                if (!rtk->ssat[ctx->sat[i]-1].vsat[f]) continue;
                rtk->ssat[ctx->sat[i]-1].outc[f]=0;
                if (f==0) rtk->sol.ns++; /* valid satellite count by L1 */
            }
            /* too few valid phases */
            if (rtk->sol.ns<4) ctx->stat=SOLQ_DGPS;
        }
        else ctx->stat=SOLQ_NONE;
    }
    return ctx->stat==SOLQ_FLOAT;
}

/* step 6: integer ambiguity resolution ---------------------------------------
*  manage_amb_LAMBDA() is called, not copied: the partial-AR retry policy
*  (satellite exclusion, arfilter, GLONASS re-run, adaptive threshold) stays
*  upstream's. Its outcome is readable from Python afterwards through rtk_t /
*  sol_t (excsat, nb_ar, holdamb, prev_ratio1/2, thres, ratio).
*  Returns: number of fixed DD ambiguities (>1) on success, 0 otherwise.
*---------------------------------------------------------------------------*/
int relpos_ambiguity_resolution(relpos_ctx_t *ctx, const obsd_t *obs)
{
    rtk_t *rtk=ctx->rtk;
    prcopt_t *opt=&rtk->opt;
    int nb,nv,nu=ctx->nu,ns=ctx->ns,nf=ctx->nf;

    if (ctx->stat!=SOLQ_FLOAT) return 0;

    /* if valid fixed solution, process it */
    if ((nb=manage_amb_LAMBDA(rtk,ctx->bias,ctx->xa,ctx->sat,nf,ns))<=1) return 0;

    /* find zero-diff residuals for fixed solution */
    if (!zdres(0,obs,nu,ctx->rs,ctx->dts,ctx->var,ctx->svh,ctx->nav,ctx->xa,opt,
               ctx->y,ctx->e,ctx->azel,ctx->freq)) {
        return 0;
    }
    /* post-fit residuals for fixed solution (xa includes fixed phase biases, rtk->xa does not) */
    nv=ddres(rtk,obs,ctx->dt,ctx->xa,ctx->Pp,ctx->sat,ctx->y,ctx->e,ctx->azel,
             ctx->freq,ctx->iu,ctx->ir,ns,ctx->v,NULL,ctx->R,ctx->vflg);

    /* validation of fixed solution, always returns valid */
    if (!valpos(rtk,ctx->v,ctx->R,ctx->vflg,nv,4.0)) return 0;

    /* hold integer ambiguity if meet minfix count */
    if (++rtk->nfix>=rtk->opt.minfix) {
        /* modear must be fix-and-hold for glomodear fix-and-hold to apply */
        if (rtk->opt.modear==ARMODE_FIXHOLD)
            holdamb(rtk,ctx->xa);
        /* switch to kinematic after qualify for hold if in static-start mode */
        if (rtk->opt.mode==PMODE_STATIC_START) {
            rtk->opt.mode=PMODE_KINEMA;
            trace(3,"Fix and hold complete: switch to kinematic mode\n");
        }
    }
    ctx->stat=SOLQ_FIX;
    return nb;
}

/* step 7: save solution status and per-satellite bookkeeping ---------------*/
void relpos_save_solution(relpos_ctx_t *ctx, const obsd_t *obs)
{
    rtk_t *rtk=ctx->rtk;
    int i,j,n=ctx->nu+ctx->nr,nf=ctx->nf,stat=ctx->stat;

    /* save solution status (fixed or float) */
    if (stat==SOLQ_FIX) {
        for (i=0;i<3;i++) {
            rtk->sol.rr[i]=rtk->xa[i];
            rtk->sol.qr[i]=(float)rtk->Pa[i+i*rtk->na];
        }
        rtk->sol.qr[3]=(float)rtk->Pa[1];
        rtk->sol.qr[4]=(float)rtk->Pa[1+2*rtk->na];
        rtk->sol.qr[5]=(float)rtk->Pa[2];

        if (rtk->opt.dynamics) { /* velocity and covariance */
            for (i=3;i<6;i++) {
                rtk->sol.rr[i]=rtk->xa[i];
                rtk->sol.qv[i-3]=(float)rtk->Pa[i+i*rtk->na];
            }
            rtk->sol.qv[3]=(float)rtk->Pa[4+3*rtk->na];
            rtk->sol.qv[4]=(float)rtk->Pa[5+4*rtk->na];
            rtk->sol.qv[5]=(float)rtk->Pa[5+3*rtk->na];
        }
    }
    else {  /* float solution */
        for (i=0;i<3;i++) {
            rtk->sol.rr[i]=rtk->x[i];
            rtk->sol.qr[i]=(float)rtk->P[i+i*rtk->nx];
        }
        rtk->sol.qr[3]=(float)rtk->P[1];
        rtk->sol.qr[4]=(float)rtk->P[1+2*rtk->nx];
        rtk->sol.qr[5]=(float)rtk->P[2];

        if (rtk->opt.dynamics) { /* velocity and covariance */
            for (i=3;i<6;i++) {
                rtk->sol.rr[i]=rtk->x[i];
                rtk->sol.qv[i-3]=(float)rtk->P[i+i*rtk->nx];
            }
            rtk->sol.qv[3]=(float)rtk->P[4+3*rtk->nx];
            rtk->sol.qv[4]=(float)rtk->P[5+4*rtk->nx];
            rtk->sol.qv[5]=(float)rtk->P[5+3*rtk->nx];
        }
        rtk->nfix=0;
    }
    /* save phase measurements */
    for (i=0;i<n;i++) for (j=0;j<nf;j++) {
        if (obs[i].L[j]==0.0) continue;
        rtk->ssat[obs[i].sat-1].pt[obs[i].rcv-1][j]=obs[i].time;
        rtk->ssat[obs[i].sat-1].ph[obs[i].rcv-1][j]=obs[i].L[j];
    }
    for (i=0;i<MAXSAT;i++) for (j=0;j<nf;j++) {
        /* Don't lose track of which sats were used to try and resolve the ambiguities */
        if (rtk->ssat[i].slip[j]&LLI_SLIP) rtk->ssat[i].slipc[j]++;
        /* Inc lock count if this sat used for good fix */
        if (!rtk->ssat[i].vsat[j]) continue;
        if (rtk->ssat[i].lock[j]<0||(rtk->nfix>0&&rtk->ssat[i].fix[j]>=2))
            rtk->ssat[i].lock[j]++;
    }
    if (stat!=SOLQ_NONE) rtk->sol.stat=stat;
}

/* bulk per-satellite data extraction ----------------------------------------
*  Same contract as the 2.4.3 version, plus out_icbias (GLONASS inter-channel
*  bias estimate, cycles, per frequency): with glomodear == GLO_ARMODE_AUTOCAL
*  the SD ambiguity x[IB()] of a GLONASS satellite excludes it, so a DD
*  rebuilt from x[IB()] alone is offset by the icbias difference.
*  SNR is in dBHz straight from ssat (no SNR_UNIT in this fork).
*
*  flags bitmask controls which fields are computed:
*    bit 0 (1)  : VRS corrections (geodist, tropo, iono, sagnac, rover dant)
*    bit 1 (2)  : base station geometry (base geodist, base azel, base dant)
*    bit 2 (4)  : per-frequency ssat fields (fix, lock, slip, snr, resc, resp, icbias)
*    bit 3 (8)  : float ambiguities + wavelengths
*---------------------------------------------------------------------------*/
void relpos_extract_sat_data(
    const relpos_ctx_t *ctx,
    const double *rover_ecef,
    const double *base_ecef,
    int flags,
    double *out_el_deg,
    double *out_az_deg,
    double *out_sat_pos,
    double *out_sat_vel,
    double *out_sat_clk,
    double *out_sat_clk_drift,
    double *out_los,
    double *out_geom_range,
    double *out_sagnac,
    double *out_tropo,
    double *out_iono,
    double *out_phw,
    double *out_base_geom_range,
    double *out_base_el_deg,
    double *out_base_az_deg,
    double *out_float_amb,
    double *out_wl,
    double *out_resc,
    double *out_resp,
    double *out_fix,
    double *out_lock,
    double *out_slip,
    double *out_snr,
    double *out_rover_dant,
    double *out_base_dant,
    double *out_base_tropo,
    double *out_base_iono,
    double *out_icbias
)
{
    rtk_t *rtk=ctx->rtk;
    prcopt_t *opt=&rtk->opt;
    int j,f,sat_no,iu_j,ir_j,nx=rtk->nx;
    int ns=ctx->ns,nf=ctx->nf;
    double rover_pos[3],base_pos[3];
    double scratch_sv[6],scratch_e[3],azel_buf[2];
    double trp[1],trp_var[1],ion[1],ion_var[1];
    double btrp[1],btrp_var[1],bion[1],bion_var[1];
    int do_vrs =(flags&1);
    int do_base=(flags&2);
    int do_ssat=(flags&4);
    int do_amb =(flags&8);

    if (do_vrs)  ecef2pos(rover_ecef,rover_pos);
    if (do_base) ecef2pos(base_ecef,base_pos);

    for (j=0;j<ns;j++) {
        sat_no=ctx->sat[j];
        if (sat_no<=0) continue;

        iu_j=ctx->iu[j];
        ir_j=ctx->ir[j];

        /* elevation and azimuth from ssat (already set by zdres) */
        out_el_deg[j]=rtk->ssat[sat_no-1].azel[1]*R2D;
        out_az_deg[j]=rtk->ssat[sat_no-1].azel[0]*R2D;

        /* satellite position/velocity from ctx->rs */
        {
            int off6=6*iu_j;
            out_sat_pos[j*3+0]=ctx->rs[off6+0];
            out_sat_pos[j*3+1]=ctx->rs[off6+1];
            out_sat_pos[j*3+2]=ctx->rs[off6+2];
            out_sat_vel[j*3+0]=ctx->rs[off6+3];
            out_sat_vel[j*3+1]=ctx->rs[off6+4];
            out_sat_vel[j*3+2]=ctx->rs[off6+5];
        }
        /* satellite clock bias/drift */
        {
            int off2=2*iu_j;
            out_sat_clk[j]      =-CLIGHT*ctx->dts[off2+0];
            out_sat_clk_drift[j]=-CLIGHT*ctx->dts[off2+1];
        }
        /* LOS unit vector from ctx->e */
        {
            int off3=3*iu_j;
            out_los[j*3+0]=ctx->e[off3+0];
            out_los[j*3+1]=ctx->e[off3+1];
            out_los[j*3+2]=ctx->e[off3+2];
        }
        /* phase wind-up */
        out_phw[j]=rtk->ssat[sat_no-1].phw;

        /* VRS corrections: geodist, sagnac, tropo, iono */
        if (do_vrs) {
            int off6=6*iu_j;
            for (f=0;f<6;f++) scratch_sv[f]=ctx->rs[off6+f];

            out_geom_range[j]=geodist(scratch_sv,rover_ecef,scratch_e);

            out_sagnac[j]=OMGE*(scratch_sv[0]*rover_ecef[1]
                               -scratch_sv[1]*rover_ecef[0])/CLIGHT;

            azel_buf[0]=rtk->ssat[sat_no-1].azel[0];
            azel_buf[1]=rtk->ssat[sat_no-1].azel[1];

            trp[0]=0.0;
            tropcorr(rtk->sol.time,ctx->nav,rover_pos,azel_buf,opt->tropopt,trp,
                     trp_var);
            out_tropo[j]=trp[0];

            ion[0]=0.0;
            ionocorr(rtk->sol.time,ctx->nav,sat_no,rover_pos,azel_buf,
                     opt->ionoopt,ion,ion_var);
            out_iono[j]=ion[0];

            /* rover receiver antenna correction (PCO + PCV) */
            if (out_rover_dant) {
                double dant_buf[NFREQ]={0};
                antmodel_sys(opt->pcvr+0,satsys(sat_no,NULL),opt->antdel[0],
                             azel_buf,1,dant_buf);
                for (f=0;f<nf;f++) out_rover_dant[j*nf+f]=dant_buf[f];
            }
        }
        /* base station geometry */
        if (do_base) {
            int boff=6*ir_j;
            double base_scratch_e[3],base_azel[2];
            for (f=0;f<6;f++) scratch_sv[f]=ctx->rs[boff+f];

            out_base_geom_range[j]=geodist(scratch_sv,base_ecef,base_scratch_e);
            satazel(base_pos,base_scratch_e,base_azel);
            out_base_el_deg[j]=base_azel[1]*R2D;
            out_base_az_deg[j]=base_azel[0]*R2D;

            if (out_base_tropo) {
                btrp[0]=0.0;
                tropcorr(rtk->sol.time,ctx->nav,base_pos,base_azel,opt->tropopt,
                         btrp,btrp_var);
                out_base_tropo[j]=btrp[0];
            }
            if (out_base_iono) {
                bion[0]=0.0;
                ionocorr(rtk->sol.time,ctx->nav,sat_no,base_pos,base_azel,
                         opt->ionoopt,bion,bion_var);
                out_base_iono[j]=bion[0];
            }
            /* base receiver antenna correction (PCO + PCV) */
            if (out_base_dant) {
                double dant_buf[NFREQ]={0};
                antmodel_sys(opt->pcvr+1,satsys(sat_no,NULL),opt->antdel[1],
                             base_azel,1,dant_buf);
                for (f=0;f<nf;f++) out_base_dant[j*nf+f]=dant_buf[f];
            }
        }
        /* per-frequency ssat fields */
        if (do_ssat) {
            ssat_t *ss=&rtk->ssat[sat_no-1];
            for (f=0;f<nf;f++) {
                out_resc[j*nf+f]=ss->resc[f];
                out_resp[j*nf+f]=ss->resp[f];
                out_fix[j*nf+f] =(double)ss->fix[f];
                out_lock[j*nf+f]=(double)ss->lock[f];
                out_slip[j*nf+f]=(double)ss->slip[f];
                out_snr[j*nf+f] =(double)ss->snr_rover[f]; /* dBHz */
                if (out_icbias) out_icbias[j*nf+f]=ss->icbias[f];
            }
        }
        /* float ambiguities and wavelengths */
        if (do_amb) {
            for (f=0;f<nf;f++) {
                int amb_idx=IB(sat_no,f,opt);
                out_float_amb[j*nf+f]=(amb_idx<nx)?rtk->x[amb_idx]:0.0/0.0;
                {
                    double freq_hz=ctx->freq[f+nf*iu_j];
                    out_wl[j*nf+f]=(freq_hz>0.0)?CLIGHT/freq_hz:0.0/0.0;
                }
            }
        }
    }
}

/* bulk extraction of fixed-solution ambiguities ----------------------------*/
void relpos_extract_fixed_amb(
    const relpos_ctx_t *ctx,
    double *out_fixed_amb,
    double *out_fix_flags
)
{
    rtk_t *rtk=ctx->rtk;
    prcopt_t *opt=&rtk->opt;
    int j,f,sat_no,nx=rtk->nx;
    int ns=ctx->ns,nf=ctx->nf;

    if (!ctx->xa) return;

    for (j=0;j<ns;j++) {
        sat_no=ctx->sat[j];
        if (sat_no<=0) continue;
        for (f=0;f<nf;f++) {
            int amb_idx=IB(sat_no,f,opt);
            out_fixed_amb[j*nf+f]=(amb_idx<nx)?ctx->xa[amb_idx]:0.0/0.0;
            out_fix_flags[j*nf+f]=(double)rtk->ssat[sat_no-1].fix[f];
        }
    }
}

/* free relpos context -------------------------------------------------------
*  Always the last call of an epoch; it also advances rtk->epoch, which
*  rtkpos() increments right after relpos() returns (intpres() reads it).
*---------------------------------------------------------------------------*/
void relpos_free(relpos_ctx_t *ctx)
{
    if (ctx->epoch_pending&&ctx->rtk) {
        ctx->rtk->epoch++;
        ctx->epoch_pending=0;
    }
    if (ctx->rs)   { free(ctx->rs);   ctx->rs  =NULL; }
    if (ctx->dts)  { free(ctx->dts);  ctx->dts =NULL; }
    if (ctx->var)  { free(ctx->var);  ctx->var =NULL; }
    if (ctx->y)    { free(ctx->y);    ctx->y   =NULL; }
    if (ctx->e)    { free(ctx->e);    ctx->e   =NULL; }
    if (ctx->azel) { free(ctx->azel); ctx->azel=NULL; }
    if (ctx->freq) { free(ctx->freq); ctx->freq=NULL; }
    if (ctx->xp)   { free(ctx->xp);   ctx->xp  =NULL; }
    if (ctx->Pp)   { free(ctx->Pp);   ctx->Pp  =NULL; }
    if (ctx->xa)   { free(ctx->xa);   ctx->xa  =NULL; }
    if (ctx->bias) { free(ctx->bias); ctx->bias=NULL; }
    if (ctx->v)    { free(ctx->v);    ctx->v   =NULL; }
    if (ctx->H)    { free(ctx->H);    ctx->H   =NULL; }
    if (ctx->R)    { free(ctx->R);    ctx->R   =NULL; }
}
