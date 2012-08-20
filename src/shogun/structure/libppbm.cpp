/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * libp3bm.h: Implementation of the Proximal Point P-BMRM solver for SO training
 *
 * Copyright (C) 2012 Michal Uricar, uricamic@cmp.felk.cvut.cz
 *
 * Implementation of the Proximal Point P-BMRM
 *--------------------------------------------------------------------- */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <shogun/structure/libp3bm.h>
#include <shogun/lib/external/libqp.h>
#include <shogun/lib/Time.h>

namespace shogun
{
static const uint32_t QPSolverMaxIter=0xFFFFFFFF;
static const float64_t epsilon=0.0;

static float64_t *H, *H2;
static uint32_t BufSize;

/*----------------------------------------------------------------------
  Returns pointer at i-th column of Hessian matrix.
  ----------------------------------------------------------------------*/
static const float64_t *get_col( uint32_t i)
{
	return( &H2[ BufSize*i ] );
}

bmrm_return_value_T svm_ppbm_solver(
		CStructuredModel* model,
		float64_t*      W,
		float64_t       TolRel,
		float64_t       TolAbs,
		float64_t       _lambda,
		uint32_t        _BufSize,
		bool            cleanICP,
		uint32_t        cleanAfter,
		float64_t       K,
		uint32_t        Tmax,
		bool            verbose)
{
	bmrm_return_value_T ppbmrm={0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	libqp_state_T qp_exitflag={0, 0, 0, 0}, qp_exitflag_good={0, 0, 0, 0};
	float64_t *b, *b2, *beta, *beta_good, *beta_start, *diag_H, *diag_H2;
	float64_t R, *subgrad, *A, QPSolverTolRel, C=1.0;
	float64_t *prevW, *wt, alpha, alpha_start, alpha_good=0.0, Fd_alpha0=0.0;
	float64_t lastFp, wdist, gamma=0.0;
	floatmax_t rsum, sq_norm_W, sq_norm_Wdiff, sq_norm_prevW, eps;
	uint32_t *I, *I2, *I_start, *I_good, *ICPcounter, *ACPs, cntICP=0, cntACP=0;
	uint8_t S=1;
	uint32_t nDim=model->get_dim();
	float64_t **ICPs;
	uint32_t nCP_new=0, qp_cnt=0;
	bmrm_ll *CPList_head, *CPList_tail, *cp_ptr, *cp_ptr2, *cp_list=NULL;
	float64_t *A_1=NULL, *A_2=NULL, *H_buff;
	bool *map=NULL, tuneAlpha=true, flag=true, alphaChanged=false, isThereGoodSolution=false;

	CTime ttime;
	float64_t tstart, tstop;


	tstart=ttime.cur_time_diff(false);

	BufSize=_BufSize;
	QPSolverTolRel=1e-9;

	H=NULL;
	b=NULL;
	beta=NULL;
	A=NULL;
	subgrad=NULL;
	diag_H=NULL;
	I=NULL;
	ICPcounter=NULL;
	ICPs=NULL;
	ACPs=NULL;
	prevW=NULL;
	wt=NULL;
	H_buff=NULL;
	diag_H2=NULL;
	b2=NULL;
	I2=NULL;
	H2=NULL;
	I_good=NULL;
	I_start=NULL;
	beta_start=NULL;
	beta_good=NULL;

	alpha=0.0;

	H= (float64_t*) LIBBMRM_CALLOC(BufSize*BufSize, sizeof(float64_t));

	A= (float64_t*) LIBBMRM_CALLOC(nDim*BufSize, sizeof(float64_t));

	b= (float64_t*) LIBBMRM_CALLOC(BufSize, sizeof(float64_t));

	beta= (float64_t*) LIBBMRM_CALLOC(BufSize, sizeof(float64_t));

	subgrad= (float64_t*) LIBBMRM_CALLOC(nDim, sizeof(float64_t));

	diag_H= (float64_t*) LIBBMRM_CALLOC(BufSize, sizeof(float64_t));

	I= (uint32_t*) LIBBMRM_CALLOC(BufSize, sizeof(uint32_t));

	ICPcounter= (uint32_t*) LIBBMRM_CALLOC(BufSize, sizeof(uint32_t));

	ICPs= (float64_t**) LIBBMRM_CALLOC(BufSize, sizeof(float64_t*));

	ACPs= (uint32_t*) LIBBMRM_CALLOC(BufSize, sizeof(uint32_t));

	cp_list= (bmrm_ll*) LIBBMRM_CALLOC(1, sizeof(bmrm_ll));

	prevW= (float64_t*) LIBBMRM_CALLOC(nDim, sizeof(float64_t));

	wt= (float64_t*) LIBBMRM_CALLOC(nDim, sizeof(float64_t));

	if (H==NULL || A==NULL || b==NULL || beta==NULL || subgrad==NULL ||
			diag_H==NULL || I==NULL || ICPcounter==NULL ||	ICPs==NULL ||
			ACPs==NULL || cp_list==NULL || prevW==NULL || wt==NULL)
	{
		ppbmrm.exitflag=-2;
		goto cleanup;
	}

	map= (bool*) LIBBMRM_CALLOC(BufSize, sizeof(bool));

	if (map==NULL)
	{
		ppbmrm.exitflag=-2;
		goto cleanup;
	}

	memset( (bool*) map, true, BufSize);

	/* Temporary buffers for ICP removal */
	H_buff= (float64_t*) LIBBMRM_CALLOC(BufSize*BufSize, sizeof(float64_t));

	if (H_buff==NULL)
	{
		ppbmrm.exitflag=-2;
		goto cleanup;
	}

	/* Temporary buffers */
	beta_start= (float64_t*) LIBBMRM_CALLOC(BufSize, sizeof(float64_t));

	beta_good= (float64_t*) LIBBMRM_CALLOC(BufSize, sizeof(float64_t));

	b2= (float64_t*) LIBBMRM_CALLOC(BufSize, sizeof(float64_t));

	diag_H2= (float64_t*) LIBBMRM_CALLOC(BufSize, sizeof(float64_t));

	H2= (float64_t*) LIBBMRM_CALLOC(BufSize*BufSize, sizeof(float64_t));

	I_start= (uint32_t*) LIBBMRM_CALLOC(BufSize, sizeof(uint32_t));

	I_good= (uint32_t*) LIBBMRM_CALLOC(BufSize, sizeof(uint32_t));

	I2= (uint32_t*) LIBBMRM_CALLOC(BufSize, sizeof(uint32_t));

	if (beta_start==NULL || beta_good==NULL || b2==NULL || diag_H2==NULL ||
			I_start==NULL || I_good==NULL || I2==NULL || H2==NULL)
	{
		ppbmrm.exitflag=-2;
		goto cleanup;
	}

	ppbmrm.hist_Fp.resize_vector(BufSize);
	ppbmrm.hist_Fd.resize_vector(BufSize);
	ppbmrm.hist_wdist.resize_vector(BufSize);

	/* Iinitial solution */
	R = model->risk(subgrad, W);

	ppbmrm.nCP=0;
	ppbmrm.nIter=0;
	ppbmrm.exitflag=0;

	b[0]=-R;

	/* Cutting plane auxiliary double linked list */
	LIBBMRM_MEMCPY(A, subgrad, nDim*sizeof(float64_t));
	map[0]=false;
	cp_list->address=&A[0];
	cp_list->idx=0;
	cp_list->prev=NULL;
	cp_list->next=NULL;
	CPList_head=cp_list;
	CPList_tail=cp_list;

	/* Compute initial value of Fp, Fd, assuming that W is zero vector */
	sq_norm_W=0.0;
	sq_norm_Wdiff=0.0;

	for (uint32_t j=0; j<nDim; ++j)
	{
		b[0]+=subgrad[j]*W[j];
		sq_norm_W+=W[j]*W[j];
		sq_norm_Wdiff+=(W[j]-prevW[j])*(W[j]-prevW[j]);
	}

	ppbmrm.Fp=R+0.5*_lambda*sq_norm_W + alpha*sq_norm_Wdiff;
	ppbmrm.Fd=-LIBBMRM_PLUS_INF;
	lastFp=ppbmrm.Fp;
	wdist=::sqrt(sq_norm_Wdiff);

	K = (sq_norm_W == 0.0) ? 0.4 : 0.01*::sqrt(sq_norm_W);

	LIBBMRM_MEMCPY(prevW, W, nDim*sizeof(float64_t));

	tstop=ttime.cur_time_diff(false);

	/* Keep history of Fp, Fd, wdist */
	ppbmrm.hist_Fp[0]=ppbmrm.Fp;
	ppbmrm.hist_Fd[0]=ppbmrm.Fd;
	ppbmrm.hist_wdist[0]=wdist;

	/* Verbose output */

	if (verbose)
		SG_SPRINT("%4d: tim=%.3lf, Fp=%lf, Fd=%lf, R=%lf, K=%lf\n",
				ppbmrm.nIter, tstop-tstart, ppbmrm.Fp, ppbmrm.Fd, R, K);

	/* main loop */

	while (ppbmrm.exitflag==0)
	{
		tstart=ttime.cur_time_diff(false);
		ppbmrm.nIter++;

		/* Update H */

		if (ppbmrm.nCP>0)
		{
			A_2=get_cutting_plane(CPList_tail);
			cp_ptr=CPList_head;

			for (uint32_t i=0; i<ppbmrm.nCP; ++i)
			{
				A_1=get_cutting_plane(cp_ptr);
				cp_ptr=cp_ptr->next;
				rsum=0.0;

				for (uint32_t j=0; j<nDim; ++j)
				{
					rsum+=A_1[j]*A_2[j];
				}

				H[LIBBMRM_INDEX(i, ppbmrm.nCP, BufSize)]=rsum;
			}

			for (uint32_t i=0; i<ppbmrm.nCP; ++i)
			{
				H[LIBBMRM_INDEX(ppbmrm.nCP, i, BufSize)]=
					H[LIBBMRM_INDEX(i, ppbmrm.nCP, BufSize)];
			}
		}

		rsum=0.0;
		A_2=get_cutting_plane(CPList_tail);

		for (uint32_t i=0; i<nDim; ++i)
			rsum+=A_2[i]*A_2[i];

		H[LIBBMRM_INDEX(ppbmrm.nCP, ppbmrm.nCP, BufSize)]=rsum;

		diag_H[ppbmrm.nCP]=H[LIBBMRM_INDEX(ppbmrm.nCP, ppbmrm.nCP, BufSize)];
		I[ppbmrm.nCP]=1;

		ppbmrm.nCP++;
		beta[ppbmrm.nCP]=0.0; // [beta; 0]

		/* tune alpha cycle */
		/* ---------------------------------------------------------------------- */

		flag=true;
		isThereGoodSolution=false;
		LIBBMRM_MEMCPY(beta_start, beta, ppbmrm.nCP*sizeof(float64_t));
		LIBBMRM_MEMCPY(I_start, I, ppbmrm.nCP*sizeof(uint32_t));
		qp_cnt=0;
		alpha_good=alpha;

		if (tuneAlpha)
		{
			alpha_start=alpha; alpha=0.0;
			beta[ppbmrm.nCP]=0.0;
			LIBBMRM_MEMCPY(I2, I_start, ppbmrm.nCP*sizeof(uint32_t));
			I2[ppbmrm.nCP]=1;

			/* add alpha-dependent terms to H, diag_h and b */
			cp_ptr=CPList_head;

			for (uint32_t i=0; i<ppbmrm.nCP; ++i)
			{
				rsum=0.0;
				A_1=get_cutting_plane(cp_ptr);
				cp_ptr=cp_ptr->next;

				for (uint32_t j=0; j<nDim; ++j)
					rsum+=A_1[j]*prevW[j];

				b2[i]=b[i]-((2*alpha)/(_lambda+2*alpha))*rsum;
				diag_H2[i]=diag_H[i]/(_lambda+2*alpha);

				for (uint32_t j=0; j<ppbmrm.nCP; ++j)
					H2[LIBBMRM_INDEX(i, j, BufSize)]=
						H[LIBBMRM_INDEX(i, j, BufSize)]/(_lambda+2*alpha);
			}

			/* solve QP with current alpha */
			qp_exitflag=libqp_splx_solver(&get_col, diag_H2, b2, &C, I2, &S, beta,
					ppbmrm.nCP, QPSolverMaxIter, 0.0, QPSolverTolRel, -LIBBMRM_PLUS_INF, 0);
			ppbmrm.qp_exitflag=qp_exitflag.exitflag;
			qp_cnt++;
			Fd_alpha0=-qp_exitflag.QP;

			/* obtain w_t and check if norm(w_{t+1} -w_t) <= K */
			for (uint32_t i=0; i<nDim; ++i)
			{
				rsum=0.0;
				cp_ptr=CPList_head;

				for (uint32_t j=0; j<ppbmrm.nCP; ++j)
				{
					A_1=get_cutting_plane(cp_ptr);
					cp_ptr=cp_ptr->next;
					rsum+=A_1[i]*beta[j];
				}

				wt[i]=(2*alpha*prevW[i] - rsum)/(_lambda+2*alpha);
			}

			sq_norm_Wdiff=0.0;

			for (uint32_t i=0; i<nDim; ++i)
				sq_norm_Wdiff+=(wt[i]-prevW[i])*(wt[i]-prevW[i]);

			if (::sqrt(sq_norm_Wdiff) <= K)
			{
				flag=false;

				if (alpha!=alpha_start)
					alphaChanged=true;
			}
			else
			{
				alpha=alpha_start;
			}

			while(flag)
			{
				LIBBMRM_MEMCPY(I2, I_start, ppbmrm.nCP*sizeof(uint32_t));
				LIBBMRM_MEMCPY(beta, beta_start, ppbmrm.nCP*sizeof(float64_t));
				I2[ppbmrm.nCP]=1;
				beta[ppbmrm.nCP]=0.0;

				/* add alpha-dependent terms to H, diag_h and b */
				cp_ptr=CPList_head;

				for (uint32_t i=0; i<ppbmrm.nCP; ++i)
				{
					rsum=0.0;
					A_1=get_cutting_plane(cp_ptr);
					cp_ptr=cp_ptr->next;

					for (uint32_t j=0; j<nDim; ++j)
						rsum+=A_1[j]*prevW[j];

					b2[i]=b[i]-((2*alpha)/(_lambda+2*alpha))*rsum;
					diag_H2[i]=diag_H[i]/(_lambda+2*alpha);

					for (uint32_t j=0; j<ppbmrm.nCP; ++j)
						H2[LIBBMRM_INDEX(i, j, BufSize)]=H[LIBBMRM_INDEX(i, j, BufSize)]/(_lambda+2*alpha);
				}

				/* solve QP with current alpha */
				qp_exitflag=libqp_splx_solver(&get_col, diag_H2, b2, &C, I2, &S, beta,
						ppbmrm.nCP, QPSolverMaxIter, 0.0, QPSolverTolRel, -LIBBMRM_PLUS_INF, 0);
				ppbmrm.qp_exitflag=qp_exitflag.exitflag;
				qp_cnt++;

				/* obtain w_t and check if norm(w_{t+1}-w_t) <= K */
				for (uint32_t i=0; i<nDim; ++i)
				{
					rsum=0.0;
					cp_ptr=CPList_head;

					for (uint32_t j=0; j<ppbmrm.nCP; ++j)
					{
						A_1=get_cutting_plane(cp_ptr);
						cp_ptr=cp_ptr->next;
						rsum+=A_1[i]*beta[j];
					}

					wt[i]=(2*alpha*prevW[i] - rsum)/(_lambda+2*alpha);
				}

				sq_norm_Wdiff=0.0;
				for (uint32_t i=0; i<nDim; ++i)
					sq_norm_Wdiff+=(wt[i]-prevW[i])*(wt[i]-prevW[i]);

				if (::sqrt(sq_norm_Wdiff) > K)
				{
					/* if there is a record of some good solution
					 * (i.e. adjust alpha by division by 2) */

					if (isThereGoodSolution)
					{
						LIBBMRM_MEMCPY(beta, beta_good, ppbmrm.nCP*sizeof(float64_t));
						LIBBMRM_MEMCPY(I2, I_good, ppbmrm.nCP*sizeof(uint32_t));
						alpha=alpha_good;
						qp_exitflag=qp_exitflag_good;
						flag=false;
					}
					else
					{
						if (alpha == 0)
						{
							alpha=1.0;
							alphaChanged=true;
						}
						else
						{
							alpha*=2;
							alphaChanged=true;
						}
					}
				}
				else
				{
					if (alpha > 0)
					{
						/* keep good solution and try for alpha /= 2 if previous alpha was 1 */
						LIBBMRM_MEMCPY(beta_good, beta, ppbmrm.nCP*sizeof(float64_t));
						LIBBMRM_MEMCPY(I_good, I2, ppbmrm.nCP*sizeof(uint32_t));
						alpha_good=alpha;
						qp_exitflag_good=qp_exitflag;
						isThereGoodSolution=true;

						if (alpha!=1.0)
						{
							alpha/=2.0;
							alphaChanged=true;
						}
						else
						{
							alpha=0.0;
							alphaChanged=true;
						}
					}
					else
					{
						flag=false;
					}
				}
			}
		}
		else
		{
			alphaChanged=false;
			LIBBMRM_MEMCPY(I2, I_start, ppbmrm.nCP*sizeof(uint32_t));
			LIBBMRM_MEMCPY(beta, beta_start, ppbmrm.nCP*sizeof(float64_t));

			/* add alpha-dependent terms to H, diag_h and b */
			cp_ptr=CPList_head;

			for (uint32_t i=0; i<ppbmrm.nCP; ++i)
			{
				rsum=0.0;
				A_1=get_cutting_plane(cp_ptr);
				cp_ptr=cp_ptr->next;

				for (uint32_t j=0; j<nDim; ++j)
					rsum+=A_1[j]*prevW[j];

				b2[i]=b[i]-((2*alpha)/(_lambda+2*alpha))*rsum;
				diag_H2[i]=diag_H[i]/(_lambda+2*alpha);

				for (uint32_t j=0; j<ppbmrm.nCP; ++j)
					H2[LIBBMRM_INDEX(i, j, BufSize)]=
						H[LIBBMRM_INDEX(i, j, BufSize)]/(_lambda+2*alpha);
			}
			/* solve QP with current alpha */
			qp_exitflag=libqp_splx_solver(&get_col, diag_H2, b2, &C, I2, &S, beta,
					ppbmrm.nCP, QPSolverMaxIter, 0.0, QPSolverTolRel, -LIBBMRM_PLUS_INF, 0);
			ppbmrm.qp_exitflag=qp_exitflag.exitflag;
			qp_cnt++;
		}

		/* ----------------------------------------------------------------------------------------------- */

		/* Update ICPcounter (add one to unused and reset used) + compute number of active CPs */
		ppbmrm.nzA=0;

		for (uint32_t aaa=0; aaa<ppbmrm.nCP; ++aaa)
		{
			if (beta[aaa]>epsilon)
			{
				++ppbmrm.nzA;
				ICPcounter[aaa]=0;
			}
			else
			{
				ICPcounter[aaa]+=1;
			}
		}

		/* W update */
		for (uint32_t i=0; i<nDim; ++i)
		{
			rsum=0.0;
			cp_ptr=CPList_head;

			for (uint32_t j=0; j<ppbmrm.nCP; ++j)
			{
				A_1=get_cutting_plane(cp_ptr);
				cp_ptr=cp_ptr->next;
				rsum+=A_1[i]*beta[j];
			}

			W[i]=(2*alpha*prevW[i]-rsum)/(_lambda+2*alpha);
		}

		/* risk and subgradient computation */
		R = model->risk(subgrad, W);
		b[ppbmrm.nCP]=-R;
		add_cutting_plane(&CPList_tail, map, A,
				find_free_idx(map, BufSize), subgrad, nDim);

		sq_norm_W=0.0;
		sq_norm_Wdiff=0.0;
		sq_norm_prevW=0.0;

		for (uint32_t j=0; j<nDim; ++j)
		{
			b[ppbmrm.nCP]+=subgrad[j]*W[j];
			sq_norm_W+=W[j]*W[j];
			sq_norm_Wdiff+=(W[j]-prevW[j])*(W[j]-prevW[j]);
			sq_norm_prevW+=prevW[j]*prevW[j];
		}

		/* compute Fp and Fd */
		ppbmrm.Fp=R+0.5*_lambda*sq_norm_W + alpha*sq_norm_Wdiff;
		ppbmrm.Fd=-qp_exitflag.QP+((alpha*_lambda)/(_lambda + 2*alpha))*sq_norm_prevW;

		/* gamma + tuneAlpha flag */
		if (alphaChanged)
		{
			eps=1.0-(ppbmrm.Fd/ppbmrm.Fp);
			gamma=(lastFp*(1-eps)-Fd_alpha0)/(Tmax*(1-eps));
		}

		if ((lastFp-ppbmrm.Fp) <= gamma)
		{
			tuneAlpha=true;
		}
		else
		{
			tuneAlpha=false;
		}

		/* Stopping conditions - set only with nonzero alpha */
		if (alpha==0.0)
		{
			if (ppbmrm.Fp-ppbmrm.Fd<=TolRel*LIBBMRM_ABS(ppbmrm.Fp))
				ppbmrm.exitflag=1;

			if (ppbmrm.Fp-ppbmrm.Fd<=TolAbs)
				ppbmrm.exitflag=2;
		}

		if (ppbmrm.nCP>=BufSize)
			ppbmrm.exitflag=-1;

		tstop=ttime.cur_time_diff(false);

		/* compute wdist (= || W_{t+1} - W_{t} || ) */
		sq_norm_Wdiff=0.0;

		for (uint32_t i=0; i<nDim; ++i)
		{
			sq_norm_Wdiff+=(W[i]-prevW[i])*(W[i]-prevW[i]);
		}

		wdist=::sqrt(sq_norm_Wdiff);

		/* Keep history of Fp, Fd, wdist */
		ppbmrm.hist_Fp[ppbmrm.nIter]=ppbmrm.Fp;
		ppbmrm.hist_Fd[ppbmrm.nIter]=ppbmrm.Fd;
		ppbmrm.hist_wdist[ppbmrm.nIter]=wdist;

		/* Verbose output */
		if (verbose)
			SG_SPRINT("%4d: tim=%.3lf, Fp=%lf, Fd=%lf, (Fp-Fd)=%lf, (Fp-Fd)/Fp=%lf, R=%lf, nCP=%d, nzA=%d, wdist=%lf, alpha=%lf, qp_cnt=%d, gamma=%lf, tuneAlpha=%d\n",
					ppbmrm.nIter, tstop-tstart, ppbmrm.Fp, ppbmrm.Fd, ppbmrm.Fp-ppbmrm.Fd,
					(ppbmrm.Fp-ppbmrm.Fd)/ppbmrm.Fp, R, ppbmrm.nCP, ppbmrm.nzA, wdist, alpha,
					qp_cnt, gamma, tuneAlpha);

		/* Check size of Buffer */
		if (ppbmrm.nCP>=BufSize)
		{
			ppbmrm.exitflag=-2;
			SG_SERROR("Buffer exceeded.\n");
		}

		/* keep w_t + Fp */
		LIBBMRM_MEMCPY(prevW, W, nDim*sizeof(float64_t));
		lastFp=ppbmrm.Fp;

		/* Inactive Cutting Planes (ICP) removal */
		if (cleanICP)
		{
			/* find ICP */
			cntICP=0;
			cntACP=0;
			cp_ptr=CPList_head;
			uint32_t tmp_idx=0;

			while (cp_ptr != CPList_tail)
			{
				if (ICPcounter[tmp_idx++]>=cleanAfter)
				{
					ICPs[cntICP++]=cp_ptr->address;
				}
				else
				{
					ACPs[cntACP++]=tmp_idx-1;
				}

				cp_ptr=cp_ptr->next;
			}

			/* do ICP removal */
			if (cntICP > 0)
			{
				nCP_new=ppbmrm.nCP-cntICP;

				for (uint32_t i=0; i<cntICP; ++i)
				{
					tmp_idx=0;
					cp_ptr=CPList_head;

					while(cp_ptr->address != ICPs[i])
					{
						cp_ptr=cp_ptr->next;
						tmp_idx++;
					}

					remove_cutting_plane(&CPList_head, &CPList_tail, map, ICPs[i]);

					LIBBMRM_MEMMOVE(b+tmp_idx, b+tmp_idx+1,
							(ppbmrm.nCP-tmp_idx)*sizeof(float64_t));
					LIBBMRM_MEMMOVE(beta+tmp_idx, beta+tmp_idx+1,
							(ppbmrm.nCP-tmp_idx)*sizeof(float64_t));
					LIBBMRM_MEMMOVE(diag_H+tmp_idx, diag_H+tmp_idx+1,
							(ppbmrm.nCP-tmp_idx)*sizeof(float64_t));
					LIBBMRM_MEMMOVE(I+tmp_idx, I+tmp_idx+1,
							(ppbmrm.nCP-tmp_idx)*sizeof(uint32_t));
					LIBBMRM_MEMMOVE(ICPcounter+tmp_idx, ICPcounter+tmp_idx+1,
							(ppbmrm.nCP-tmp_idx)*sizeof(uint32_t));
				}

				/* H */
				for (uint32_t i=0; i < nCP_new; ++i)
				{
					for (uint32_t j=0; j < nCP_new; ++j)
					{
						H_buff[LIBBMRM_INDEX(i, j, BufSize)]=
							H[LIBBMRM_INDEX(ACPs[i], ACPs[j], BufSize)];
					}
				}

				for (uint32_t i=0; i<nCP_new; ++i)
					for (uint32_t j=0; j<nCP_new; ++j)
						H[LIBBMRM_INDEX(i, j, BufSize)]=H_buff[LIBBMRM_INDEX(i, j, BufSize)];

				ppbmrm.nCP=nCP_new;
			}
		}
	} /* end of main loop */

	ppbmrm.hist_Fp.resize_vector(ppbmrm.nIter);
	ppbmrm.hist_Fd.resize_vector(ppbmrm.nIter);
	ppbmrm.hist_wdist.resize_vector(ppbmrm.nIter);

	cp_ptr=CPList_head;

	while(cp_ptr!=NULL)
	{
		cp_ptr2=cp_ptr;
		cp_ptr=cp_ptr->next;
		LIBBMRM_FREE(cp_ptr2);
		cp_ptr2=NULL;
	}

	cp_list=NULL;

cleanup:

	LIBBMRM_FREE(H);
	LIBBMRM_FREE(b);
	LIBBMRM_FREE(beta);
	LIBBMRM_FREE(A);
	LIBBMRM_FREE(subgrad);
	LIBBMRM_FREE(diag_H);
	LIBBMRM_FREE(I);
	LIBBMRM_FREE(ICPcounter);
	LIBBMRM_FREE(ICPs);
	LIBBMRM_FREE(ACPs);
	LIBBMRM_FREE(H_buff);
	LIBBMRM_FREE(map);
	LIBBMRM_FREE(prevW);
	LIBBMRM_FREE(wt);
	LIBBMRM_FREE(beta_start);
	LIBBMRM_FREE(beta_good);
	LIBBMRM_FREE(I_start);
	LIBBMRM_FREE(I_good);
	LIBBMRM_FREE(I2);
	LIBBMRM_FREE(b2);
	LIBBMRM_FREE(diag_H2);
	LIBBMRM_FREE(H2);

	if (cp_list)
		LIBBMRM_FREE(cp_list);

	return(ppbmrm);
}
}
