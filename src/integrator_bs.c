/**
 * @file 	integrator.c
 * @brief 	BS integration scheme.
 * @author 	Hanno Rein <hanno@hanno-rein.de>
 * @details	This file implements the Gragg-Bulirsch-Stoer integration scheme.  
 *          It is a reimplementation of the fortran code by E. Hairer and G. Wanner.
 *          The starting point was the JAVA implementation in hipparchus:
 *          https://github.com/Hipparchus-Math/hipparchus/blob/master/hipparchus-ode/src/main/java/org/hipparchus/ode/nonstiff/GraggBulirschStoerIntegrator.java
 *
 * @section 	LICENSE
 * Copyright (c) 2021 Hanno Rein
 *
 * This file is part of rebound.
 *
 * rebound is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * rebound is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with rebound.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (c) 2004, Ernst Hairer
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 * 
 *  - Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A  PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <string.h> // memset
#include <float.h> // for DBL_MAX
#include "rebound.h"
#include "integrator_bs.h"
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
    
// Default configuration parameter. 
// They are hard coded here because it
// is unlikely that these need to be changed by the user.
static const int maxOrder = 18;// was 18 
static const int sequence_length = maxOrder / 2; 
static const double stepControl1 = 0.65;
static const double stepControl2 = 0.94;
static const double stepControl3 = 0.02;
static const double stepControl4 = 4.0;
static const double orderControl1 = 0.8;
static const double orderControl2 = 0.9;
static const double stabilityReduction = 0.5;
static const int maxIter = 2; // maximal number of iterations for which checks are performed
static const int maxChecks = 1; // maximal number of checks for each iteration


static int tryStep(struct reb_ode_state* states, const int Ns, const int k, const int n, const double t0, const double step, const int method) {
    const double subStep  = step / n;
    double t = t0;

    switch (method) {
        case 0: // LeapFrog
            {
                // first substep
                for (int s=0; s < Ns; s++){
                    double* y0 = states[s].y;
                    double* y1 = states[s].y1;
                    const int length = states[s].length;
                    for (int i = 0; i < length; ++i) {
                        if (i%6<3){ // Drift
                            y1[i] = y0[i] + 0.5*subStep * y0[i+3];
                        }
                    }
                }
                t += 0.5*subStep;
                for (int s=0; s < Ns; s++){
                    states[s].derivatives(&states[s], states[s].yDot, states[s].y1, t);
                }
                for (int s=0; s < Ns; s++){
                    double* y0 = states[s].y;
                    double* y1 = states[s].y1;
                    double* yDot = states[s].yDot;
                    const int length = states[s].length;
                    for (int i = 0; i < length; ++i) {
                        if (i%6>2){ // Kick
                            y1[i] = y0[i] + subStep * yDot[i];
                        }
                    }
                }


                // other substeps
                for (int j = 1; j < n; ++j) {
                    t += subStep;
                    for (int s=0; s < Ns; s++){
                        double* y1 = states[s].y1;
                        const int length = states[s].length;
                        for (int i = 0; i < length; ++i) {
                            if (i%6<3){ // Drift
                                y1[i] = y1[i] + subStep * y1[i+3];
                            }
                        }
                    }
                    for (int s=0; s < Ns; s++){
                        states[s].derivatives(&states[s], states[s].yDot, states[s].y1, t);
                    }
                    for (int s=0; s < Ns; s++){
                        double* y1 = states[s].y1;
                        double* yDot = states[s].yDot;
                        const int length = states[s].length;
                        for (int i = 0; i < length; ++i) {
                            if (i%6>2){ // Kick
                                y1[i] = y1[i] + subStep * yDot[i];
                            }
                        }
                    }

                    // stability checki // TODO
                    //if (performStabilityCheck && (j <= maxChecks) && (k < maxIter)) {
                    //    double initialNorm = 0.0;
                    //    for (int l = 0; l < length; ++l) {
                    //        const double ratio = y0Dot[l] / scale[l];
                    //        initialNorm += ratio * ratio;
                    //    }
                    //    double deltaNorm = 0.0;
                    //    for (int l = 0; l < length; ++l) {
                    //        const double ratio = (yDot[l] - y0Dot[l]) / scale[l];
                    //        deltaNorm += ratio * ratio;
                    //    }
                    //    //printf("iii   %e %e\n",initialNorm, deltaNorm);
                    //    if (deltaNorm > 4 * MAX(1.0e-15, initialNorm)) {
                    //        return 0;
                    //    }
                    //}
                }

                // correction of the last substep (at t0 + step)
                for (int s=0; s < Ns; s++){
                    double* y1 = states[s].y1;
                    const int length = states[s].length;
                    for (int i = 0; i < length; ++i) {
                        if (i%6<3){ // Drift
                            y1[i] = y1[i] + 0.5 * subStep * y1[i+3];
                        }
                    }
                }

                return 1;
            }
        case 1: // Modified Midpoint
            {
                // first substep
                t += subStep;
                for (int s=0; s < Ns; s++){
                    double* y0 = states[s].y;
                    double* y1 = states[s].y1;
                    double* y0Dot = states[s].y0Dot;
                    const int length = states[s].length;
                    for (int i = 0; i < length; ++i) {
                        y1[i] = y0[i] + subStep * y0Dot[i];
                    }
                }

                // other substeps
                for (int s=0; s < Ns; s++){
                    states[s].derivatives(&states[s], states[s].yDot, states[s].y1, t);
                }
                for (int s=0; s < Ns; s++){
                    double* y0 = states[s].y;
                    double* yTmp = states[s].yTmp;
                    const int length = states[s].length;
                    for (int i = 0; i < length; ++i) {
                        yTmp[i] = y0[i];
                    }
                }

                for (int j = 1; j < n; ++j) {  // Note: iterating n substeps, not 2n substeps as in Eq. (9.13)
                    t += subStep;
                    for (int s=0; s < Ns; s++){
                        double* y1 = states[s].y1;
                        double* yDot = states[s].yDot;
                        double* yTmp = states[s].yTmp;
                        const int length = states[s].length;
                        for (int i = 0; i < length; ++i) {
                            const double middle = y1[i];
                            y1[i]       = yTmp[i] + 2.* subStep * yDot[i];
                            yTmp[i]       = middle;
                        }
                    }

                    for (int s=0; s < Ns; s++){
                        states[s].derivatives(&states[s], states[s].yDot, states[s].y1, t);
                    }

                    // stability check
                    if (j <= maxChecks && k < maxIter) {
                        double initialNorm = 0.0;
                        for (int s=0; s < Ns; s++){
                            double* y0Dot = states[s].y0Dot;
                            double* scale = states[s].scale;
                            const int length = states[s].length;
                            for (int l = 0; l < length; ++l) {
                                const double ratio = y0Dot[l] / scale[l];
                                initialNorm += ratio * ratio;
                            }
                        }
                        double deltaNorm = 0.0;
                        for (int s=0; s < Ns; s++){
                            double* yDot = states[s].yDot;
                            double* y0Dot = states[s].y0Dot;
                            double* scale = states[s].scale;
                            const int length = states[s].length;
                            for (int l = 0; l < length; ++l) {
                                const double ratio = (yDot[l] - y0Dot[l]) / scale[l];
                                deltaNorm += ratio * ratio;
                            }
                        }
                        if (deltaNorm > 4 * MAX(1.0e-15, initialNorm)) {
                            return 0;
                        }
                    }

                }

                // correction of the last substep (at t0 + step)
                for (int s=0; s < Ns; s++){
                    double* y1 = states[s].y1;
                    double* yTmp = states[s].yTmp;
                    double* yDot = states[s].yDot;
                    const int length = states[s].length;
                    for (int i = 0; i < length; ++i) {
                        y1[i] = 0.5 * (yTmp[i] + y1[i] + subStep * yDot[i]); // = 0.25*(y_(2n-1) + 2*y_n(2) + y_(2n+1))     Eq (9.13c)
                    }
                }

                return 1;
            }
            return 0;
            break;
        default:
            printf("Error. method not implemented in BS\n");
            exit(1);
    }
}

static void extrapolate(const struct reb_ode_state* state, double * const coeff, const int k) {
    double* const y1 = state->y1;
    double* const C = state->C;  // C and D values follow Numerical Recipes 
    double** const D =  state->D;
    double const length = state->length;
        for (int j = 0; j < k; ++j) {
        double xi = coeff[k-j-1];
        double xim1 = coeff[k];
        double facC = xi/(xi-xim1);
        double facD = xim1/(xi-xim1);
        for (int i = 0; i < length; ++i) {
            double CD = C[i] - D[k - j -1][i];
            C[i] = facC * CD; // Only need to keep one C value
            D[k - j - 1][i] = facD * CD; // Keep all D values for recursion
        }
    }
    for (int i = 0; i < length; ++i) {
        y1[i] = D[0][i];
    }
    for (int j = 1; j <= k; ++j) {
        for (int i = 0; i < length; ++i) {
        y1[i] += D[j][i];
        }
    }
}

static void nbody_derivatives(struct reb_ode_state* state, double* const yDot, const double* const y, double const t){
    struct reb_simulation* const r = (struct reb_simulation* const)(state->ref);
    for (int i=0; i<r->N; i++){
         struct reb_particle* const p = &(r->particles[i]);
         p->x  = y[i*6+0];
         p->y  = y[i*6+1];
         p->z  = y[i*6+2];
         p->vx = y[i*6+3];
         p->vy = y[i*6+4];
         p->vz = y[i*6+5];
    }
    reb_update_acceleration(r);

    for (int i=0; i<r->N; i++){
        const struct reb_particle p = r->particles[i];
        yDot[i*6+0] = p.vx;
        yDot[i*6+1] = p.vy;
        yDot[i*6+2] = p.vz;
        yDot[i*6+3] = p.ax;
        yDot[i*6+4] = p.ay;
        yDot[i*6+5] = p.az;
    }
}




void reb_integrator_bs_part1(struct reb_simulation* r){
}

static void allocate_sequence_arrays(struct reb_simulation_integrator_bs* ri_bs){
    ri_bs->sequence        = malloc(sizeof(int)*sequence_length);
    ri_bs->costPerStep     = malloc(sizeof(int)*sequence_length);
    ri_bs->coeff           = malloc(sizeof(double)*sequence_length);
    ri_bs->costPerTimeUnit = malloc(sizeof(double)*sequence_length);
    ri_bs->optimalStep     = malloc(sizeof(double)*sequence_length);

    // step size sequence: 2, 6, 10, 14, ...  // only needed for dense output
     for (int k = 0; k < sequence_length; ++k) {
        ri_bs->sequence[k] = 4 * k + 2;
    }
    
    // step size sequence: 1,2,3,4,5 ...
    //for (int k = 0; k < sequence_length; ++k) {
    //    ri_bs->sequence[k] = 2*( k+1);
    //}

    // initialize the order selection cost array
    // (number of function calls for each column of the extrapolation table)
    ri_bs->costPerStep[0] = ri_bs->sequence[0] + 1;
    for (int k = 1; k < sequence_length; ++k) {
        ri_bs->costPerStep[k] = ri_bs->costPerStep[k - 1] + ri_bs->sequence[k];
    }
    ri_bs->costPerTimeUnit[0]       = 0;

    // initialize the extrapolation tables
    for (int j = 0; j < sequence_length; ++j) {
        double r = 1./((double) ri_bs->sequence[j]);
        ri_bs->coeff[j] = r*r;
    }
}

static void reb_integrator_bs_default_scale(struct reb_ode_state* state, double* y1, double* y2, double absTol, double relTol){
    double* scale = state->scale;
    int length = state->length;
    for (int i = 0; i < length; i++) {
        scale[i] = absTol + relTol * MAX(fabs(y1[i]), fabs(y2[i]));
    }
}


int reb_integrator_bs_step(struct reb_simulation_integrator_bs* ri_bs, double t, double dt){
    // return 1 if step was successful
    //        0 if rejected 

    // initial order selection
    if (ri_bs->targetIter == 0){
        const double tol    = ri_bs->scalRelativeTolerance;
        const double log10R = log10(MAX(1.0e-10, tol));
        ri_bs->targetIter = MAX(1, MIN(sequence_length - 2, (int) floor(0.5 - 0.6 * log10R)));
    }

    double  maxError = DBL_MAX;

    int Ns = ri_bs->N; // Number of states
    struct reb_ode_state* states = ri_bs->states;
    double error;
    int reject = 0;
    
    
    for (int s=0; s < Ns; s++){
        if (states[s].getscale){
            states[s].getscale(&states[s], states[s].y, states[s].y); // initial scaling
        }else{
            reb_integrator_bs_default_scale(&states[s], states[s].y, states[s].y, ri_bs->scalRelativeTolerance, ri_bs->scalAbsoluteTolerance);
        }
    }

    // first evaluation, at the beginning of the step
    if (ri_bs->method == 1){ // Note: only for midpoint. leapfrog calculates it itself
        for (int s=0; s < Ns; s++){
            ri_bs->states[s].derivatives(&(ri_bs->states[s]), ri_bs->states[s].y0Dot, ri_bs->states[s].y, t);
        }
    }

    const int forward = (dt >= 0.);
    printf("step = %.7e    order== %d\n",dt, ri_bs->targetIter);


    // iterate over several substep sizes
    int k = -1;
    for (int loop = 1; loop; ) {

        ++k;
        
        // modified midpoint integration with the current substep
        if ( ! tryStep(ri_bs->states, Ns, k, ri_bs->sequence[k], t, dt, ri_bs->method)) {

            // the stability check failed, we reduce the global step
            printf("S"); //TODO
            dt  = fabs(dt * stabilityReduction);
            reject = 1;
            loop   = 0;

        } else {
            for (int s=0; s < Ns; s++){
                const int length = states[s].length;
                for (int i = 0; i < length; ++i) {
                    double CD = states[s].y1[i];
                    states[s].C[i] = CD;
                    states[s].D[k][i] = CD;
                }
            }

            // the substep was computed successfully
            if (k > 0) {

                // extrapolate the state at the end of the step
                // using last iteration data
                for (int s=0; s < Ns; s++){
                    extrapolate(&ri_bs->states[s], ri_bs->coeff, k);
                    if (states[s].getscale){
                        states[s].getscale(&states[s], states[s].y, states[s].y1);
                    }else{
                        reb_integrator_bs_default_scale(&states[s], states[s].y, states[s].y, ri_bs->scalRelativeTolerance, ri_bs->scalAbsoluteTolerance);
                    }
                }

                // estimate the error at the end of the step.
                error = 0;
                long int combined_length = 0;
                for (int s=0; s < Ns; s++){
                    const int length = states[s].length;
                    combined_length += length;
                    double * C = states[s].C;
                    double * scale = states[s].scale;
                    for (int j = 0; j < length; ++j) {
                        const double e = C[j] / scale[j];
                        error = MAX(error, e * e);
                    }
                }
                error = sqrt(error / combined_length);
                if (isnan(error)) {
                    printf("Error. NaN appearing during integration.");
                    exit(0);
                }

                if ((error > 1.0e25)){ // TODO: Think about what to do when error increases: || ((k > 1) && (error > maxError))) {
                    // error is too big, we reduce the global step
                    printf("R (error= %.5e)",error);  // TODO
                    dt  = fabs(dt * stabilityReduction);
                    reject = 1;
                    loop   = 0;
                } else {

                    maxError = MAX(4 * error, 1.0);

                    // compute optimal stepsize for this order
                    const double exp = 1.0 / (2 * k + 1);
                    double fac = stepControl2 / pow(error / stepControl1, exp);
                    const double power = pow(stepControl3, exp);
                    fac = MAX(power / stepControl4, MIN(1. / power, fac));
                    ri_bs->optimalStep[k]     = fabs(dt * fac);
                    ri_bs->costPerTimeUnit[k] = ri_bs->costPerStep[k] / ri_bs->optimalStep[k];

                    // check convergence
                    switch (k - ri_bs->targetIter) {

                        case -1 : // one before target
                            if ((ri_bs->targetIter > 1) && ! ri_bs->previousRejected) {

                                // check if we can stop iterations now
                                if (error <= 1.0) {
                                    // convergence have been reached just before targetIter
                                    loop = 0;
                                } else {
                                    // estimate if there is a chance convergence will
                                    // be reached on next iteration, using the
                                    // asymptotic evolution of error
                                    const double ratio = ((double) ri_bs->sequence[ri_bs->targetIter] * ri_bs->sequence[ri_bs->targetIter + 1]) / (ri_bs->sequence[0] * ri_bs->sequence[0]);
                                    if (error > ratio * ratio) {
                                        // we don't expect to converge on next iteration
                                        // we reject the step immediately and reduce order
                                        reject = 1;
                                        loop   = 0;
                                        ri_bs->targetIter = k;
                                        if ((ri_bs->targetIter > 1) &&
                                                (ri_bs->costPerTimeUnit[ri_bs->targetIter - 1] <
                                                 orderControl1 * ri_bs->costPerTimeUnit[ri_bs->targetIter])) {
                                            ri_bs->targetIter -= 1;
                                        }
                                        dt = ri_bs->optimalStep[ri_bs->targetIter];
                                        printf("O"); // TODO
                                    }
                                }
                            }
                            break;

                        case 0: // exactly on target
                            if (error <= 1.0) {
                                // convergence has been reached exactly at targetIter
                                loop = 0;
                            } else {
                                // estimate if there is a chance convergence will
                                // be reached on next iteration, using the
                                // asymptotic evolution of error
                                const double ratio = ((double) ri_bs->sequence[k + 1]) / ri_bs->sequence[0];
                                if (error > ratio * ratio) {
                                    // we don't expect to converge on next iteration
                                    // we reject the step immediately
                                    printf("o"); // TODO
                                    reject = 1;
                                    loop = 0;
                                    if ((ri_bs->targetIter > 1) &&
                                            (ri_bs->costPerTimeUnit[ri_bs->targetIter - 1] <
                                             orderControl1 * ri_bs->costPerTimeUnit[ri_bs->targetIter])) {
                                        --ri_bs->targetIter;
                                    }
                                    dt = ri_bs->optimalStep[ri_bs->targetIter];
                                }
                            }
                            break;

                        case 1 : // one past target
                            if (error > 1.0) {
                                printf("e"); // TODO
                                reject = 1;
                                if ((ri_bs->targetIter > 1) &&
                                        (ri_bs->costPerTimeUnit[ri_bs->targetIter - 1] <
                                         orderControl1 * ri_bs->costPerTimeUnit[ri_bs->targetIter])) {
                                    --ri_bs->targetIter;
                                }
                                dt = ri_bs->optimalStep[ri_bs->targetIter];
                            }
                            loop = 0;
                            break;

                        default :
                            if (ri_bs->firstOrLastStep && (error <= 1.0)) {
                                loop = 0;
                            }
                            break;

                    }
                }
            }
        }
    }


    if (! reject) {
        printf("."); // TODO
        // Swap arrays
        for (int s=0; s < Ns; s++){
            double* y_tmp = states[s].y;
            states[s].y = states[s].y1; 
            states[s].y1 = y_tmp; 
        }

        int optimalIter;
        if (k == 1) {
            optimalIter = 2;
            if (ri_bs->previousRejected) {
                optimalIter = 1;
            }
        } else if (k <= ri_bs->targetIter) { // Converged before or on target
            optimalIter = k;
            if (ri_bs->costPerTimeUnit[k - 1] < orderControl1 * ri_bs->costPerTimeUnit[k]) {
                optimalIter = k - 1;
            } else if (ri_bs->costPerTimeUnit[k] < orderControl2 * ri_bs->costPerTimeUnit[k - 1]) {
                optimalIter = MIN(k + 1, sequence_length - 2);
            }
        } else {                            // converged after target
            optimalIter = k - 1;
            if ((k > 2) && (ri_bs->costPerTimeUnit[k - 2] < orderControl1 * ri_bs->costPerTimeUnit[k - 1])) {
                optimalIter = k - 2;
            }
            if (ri_bs->costPerTimeUnit[k] < orderControl2 * ri_bs->costPerTimeUnit[optimalIter]) {
                optimalIter = MIN(k, sequence_length - 2);
            }
        }

        if (ri_bs->previousRejected) {
            // after a rejected step neither order nor stepsize
            // should increase
            ri_bs->targetIter = MIN(optimalIter, k);
            dt = MIN(fabs(dt), ri_bs->optimalStep[ri_bs->targetIter]);
        } else {
            // stepsize control
            if (optimalIter <= k) {
                dt = ri_bs->optimalStep[optimalIter];
            } else {
                if ((k < ri_bs->targetIter) &&
                        (ri_bs->costPerTimeUnit[k] < orderControl2 * ri_bs->costPerTimeUnit[k - 1])) {
                    dt = ri_bs->optimalStep[k] * ri_bs->costPerStep[optimalIter + 1] / ri_bs->costPerStep[k];
                } else {
                    dt = ri_bs->optimalStep[k] * ri_bs->costPerStep[optimalIter] / ri_bs->costPerStep[k];
                }
            }

            ri_bs->targetIter = optimalIter;

        }
    }

    dt = fabs(dt);

    if (dt < ri_bs->minStep) {
        dt = ri_bs->minStep;
        printf("Error. Minimal stepsize reached during integration."); // TODO
        exit(0);
    }

    if (dt > ri_bs->maxStep && ri_bs->maxStep>0.) {
        dt = ri_bs->maxStep;
        printf("Error. Maximum stepsize reached during integration."); // TODO
        exit(0);
    }

    if (! forward) {
        dt = -dt;
    }
    ri_bs->dt_proposed = dt;

    if (reject) {
        ri_bs->previousRejected = 1;
    } else {
        ri_bs->previousRejected = 0;
        ri_bs->firstOrLastStep = 0;
    }
    return !reject;
}

struct reb_ode_state* reb_integrator_bs_add_ode(struct reb_simulation_integrator_bs* ri_bs, unsigned int length){
    if (ri_bs->allocatedN <= ri_bs->N){
        ri_bs->allocatedN += 1;
        ri_bs->states = realloc(ri_bs->states,sizeof(struct reb_ode_state)*ri_bs->allocatedN);
        memset(&ri_bs->states[ri_bs->allocatedN-1], 0, sizeof(struct reb_ode_state));
    }
    ri_bs->N += 1;

    struct reb_ode_state* state = &ri_bs->states[ri_bs->N-1];

    state->length = length;
    state->allocatedN = length;
    state->D   = malloc(sizeof(double*)*(sequence_length));
    for (int k = 0; k < sequence_length; ++k) {
        state->D[k]   = malloc(sizeof(double)*length);
    }

    state->C     = realloc(state->C, sizeof(double)*length);
    state->y     = realloc(state->y, sizeof(double)*length);
    state->y1    = realloc(state->y1, sizeof(double)*length);
    state->y0Dot = realloc(state->y0Dot, sizeof(double)*length);
    state->yTmp  = realloc(state->yTmp, sizeof(double)*length);
    state->yDot  = realloc(state->yDot, sizeof(double)*length);

    state->scale = realloc(state->scale, sizeof(double)*length);

    return state;
}

void reb_integrator_bs_part2(struct reb_simulation* r){
    struct reb_simulation_integrator_bs* ri_bs = &(r->ri_bs);
    
    if (r->status==REB_RUNNING_LAST_STEP){
        ri_bs->firstOrLastStep = 1;
    }
    
    if (ri_bs->sequence==NULL){
        allocate_sequence_arrays(ri_bs);
    }

    int nbody_length = r->N*3*2;
    if (ri_bs->nbody_state == NULL){ 
        ri_bs->nbody_state = reb_integrator_bs_add_ode(ri_bs, nbody_length);
        ri_bs->nbody_state->derivatives = nbody_derivatives;
        ri_bs->nbody_state->ref = r;
        ri_bs->firstOrLastStep = 1;
    }

    {
        double* const y = ri_bs->nbody_state->y;
        for (int i=0; i<r->N; i++){
            const struct reb_particle p = r->particles[i];
            y[i*6+0] = p.x;
            y[i*6+1] = p.y;
            y[i*6+2] = p.z;
            y[i*6+3] = p.vx;
            y[i*6+4] = p.vy;
            y[i*6+5] = p.vz;
        }
    }


    // Generic integrator stuff
    int success = reb_integrator_bs_step(ri_bs, r->t, r->dt);
    if (success){
        printf("dt %f %f\n",r->dt, ri_bs->dt_proposed);
        r->t += r->dt;
        r->dt_last_done = r->dt;
    }
    r->dt = ri_bs->dt_proposed;

    // N-body specific:
    {
        double* const y = ri_bs->nbody_state->y; // y might have been swapped
        for (int i=0; i<r->N; i++){
            struct reb_particle* const p = &(r->particles[i]);
            p->x  = y[i*6+0];
            p->y  = y[i*6+1];
            p->z  = y[i*6+2];
            p->vx = y[i*6+3];
            p->vy = y[i*6+4];
            p->vz = y[i*6+5];
        }
    }
}

void reb_integrator_bs_synchronize(struct reb_simulation* r){
    // Do nothing.
}

void reb_ode_free(struct reb_ode_state* state){
    // Free data array
    free(state->y1);
    state->y1 = NULL;
    free(state->C);
    state->C = NULL;
    free(state->scale);
    state->scale = NULL;
    
    if (state->D){
        for (int k = 0; k < sequence_length; ++k) {
            state->D[k] = NULL;
        }
        free(state->D);
        state->D = NULL;
    }
    if (state->y0Dot){
        free(state->y0Dot);
        state->y0Dot = NULL;
    }
    if (state->yTmp){
        free(state->yTmp);
        state->yTmp = NULL;
    }
    if (state->yDot){
        free(state->yDot);
        state->yDot = NULL;
    }
}


void reb_integrator_bs_reset_struct(struct reb_simulation_integrator_bs* ri_bs){
    if (ri_bs->N){
        for (int s=0; s < ri_bs->N; s++){
            reb_ode_free(&ri_bs->states[s]);
        }
        free(ri_bs->states);
        ri_bs->N = 0;
        ri_bs->allocatedN = 0;
    }

    // Free sequence arrays
    free(ri_bs->sequence);
    ri_bs->sequence = NULL;
    
    free(ri_bs->coeff);
    ri_bs->coeff = NULL;
    free(ri_bs->costPerStep);
    ri_bs->costPerStep = NULL;
    free(ri_bs->costPerTimeUnit);
    ri_bs->costPerTimeUnit = NULL;
    free(ri_bs->optimalStep);
    ri_bs->optimalStep = NULL;
    
    
    // Default settings
    ri_bs->scalAbsoluteTolerance= 1e-5;
    ri_bs->scalRelativeTolerance= 1e-5;
    ri_bs->maxStep              = 10; // Note: always positive
    ri_bs->minStep              = 1e-8; // Note: always positive
    ri_bs->firstOrLastStep      = 1;
    ri_bs->previousRejected     = 0;
    ri_bs->method               = 1;  // 1== midpoint
        
}

void reb_integrator_bs_reset(struct reb_simulation* r){
    struct reb_simulation_integrator_bs* ri_bs = &(r->ri_bs);
    reb_integrator_bs_reset_struct(ri_bs);
}
