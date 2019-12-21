/**
 * @file    integrator_mercurana.c
 * @brief   MERCURANA, a modified version of John Chambers' MERCURY algorithm
 *          using the IAS15 integrator and WHFast. It works with planet-planry
 *          collisions, test particles, and additional forces.
 * @author  Hanno Rein, Dan Tamayo
 * 
 * @section LICENSE
 * Copyright (c) 2019 Hanno Rein, Dan Tamayo
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include "rebound.h"
#include "integrator.h"
#include "gravity.h"
#include "integrator_mercurana.h"
#include "integrator_eos.h"
#include "collision.h"
#define MIN(a, b) ((a) > (b) ? (b) : (a))    ///< Returns the minimum of a and b
#define MAX(a, b) ((a) > (b) ? (a) : (b))    ///< Returns the maximum of a and b

// Machine independent implementation of pow(*,1./3.) using Newton's method.
// Speed is not an issue. Only used to calculate dcrit.
static double sqrt3(double a){
    double x = 1.;
    for (int k=0; k<200;k++){  // A smaller number should be ok too.
        double x2 = x*x;
        x += (a/x2-x)/3.;
    }
    return x;
}

// Helper functions for L_infinity
static double f(double x){
    if (x<0) return 0;
    return exp(-1./x);
}

static double dfdy(double x){
    if (x<0) return 0;
    return exp(-1./x)/(x*x);
}

// Infinitely differentiable function.
static double reb_integrator_mercurana_L_infinity(const struct reb_simulation* const r, double d, double ri, double ro){
    double y = (d-ri)/(ro-ri);
    if (y<0.){
        return 0.;
    }else if (y>1.){
        return 1.;
    }else{
        return f(y) /(f(y) + f(1.-y));
    }
}

// Infinitely differentiable function.
static double reb_integrator_mercurana_dLdr_infinity(const struct reb_simulation* const r, double d, double ri, double ro){
    double y = (d-ri)/(ro-ri);
    double dydr = 1./(ro-ri);
    if (y<0.){
        return 0.;
    }else if (y>1.){
        return 0.;
    }else{
        return dydr*(
                dfdy(y) /(f(y) + f(1.-y))
                -f(y) /(f(y) + f(1.-y))/(f(y) + f(1.-y)) * (dfdy(y) - dfdy(1.-y))
                );
    }
}

void reb_mercurana_predict_rmin2(struct reb_particle p1, struct reb_particle p2, double dt, double* rmin2_ab, double* rmin2_abc){ 
    double dts = copysign(1.,dt); 
    dt = fabs(dt);
    const double dx1 = p1.x - p2.x; // distance at beginning
    const double dy1 = p1.y - p2.y;
    const double dz1 = p1.z - p2.z;
    const double r1 = (dx1*dx1 + dy1*dy1 + dz1*dz1);
    const double dvx1 = dts*(p1.vx - p2.vx); 
    const double dvy1 = dts*(p1.vy - p2.vy);
    const double dvz1 = dts*(p1.vz - p2.vz);
    const double dx2 = dx1 +dt*dvx1; // distance at end
    const double dy2 = dy1 +dt*dvy1;
    const double dz2 = dz1 +dt*dvz1;
    const double r2 = (dx2*dx2 + dy2*dy2 + dz2*dz2);
    const double t_closest = (dx1*dvx1 + dy1*dvy1 + dz1*dvz1)/(dvx1*dvx1 + dvy1*dvy1 + dvz1*dvz1);
    const double dx3 = dx1+t_closest*dvx1; // closest approach
    const double dy3 = dy1+t_closest*dvy1;
    const double dz3 = dz1+t_closest*dvz1;
    const double r3 = (dx3*dx3 + dy3*dy3 + dz3*dz3);

    *rmin2_ab = MIN(r1,r2);
    if (t_closest/dt>=0. && t_closest/dt<=1.){
        *rmin2_abc = MIN(*rmin2_ab, r3);
    }else{
        *rmin2_abc = *rmin2_ab;
    }
}

static void reb_mercurana_encounter_predict(struct reb_simulation* const r, double dt, int shell){
    struct reb_simulation_integrator_mercurana* rim = &(r->ri_mercurana);
    struct reb_particle* const particles = r->particles;
    const double* const dcrit = rim->dcrit[shell];
    const int N = rim->shellN[shell];
    const int N_active = rim->shellN_active[shell];
    unsigned int* map = rim->map[shell];

    if (shell==0 && rim->whsplitting){ // for WH splitting
        for (int i=0; i<N; i++){
            int mi = map[i]; 
            rim->inshell[mi] = 0;
            rim->map[shell+1][i] = mi;
        }
        rim->shellN[shell+1] = N;
        rim->shellN_active[shell+1] = N_active;
        return;
    }

    // Put all particles in current shell by default
    for (int i=0; i<N; i++){
        int mi = map[i]; 
        rim->inshell[mi] = 1;
    }
    
    if (shell+1>=rim->Nmaxshells){ // does sub-shell exist?
        return;
    }
	
    // Check if particles are in sub-shell
    rim->shellN[shell+1] = 0;
    rim->shellN_active[shell+1] = 0;

    // Note: Need to find the active particles first
    // TODO make this O(N^2/2)
    for (int i=0; i<N_active; i++){
        int mi = map[i]; 
        for (int j=0; j<N; j++){
            int mj = map[j]; 
            if (i==j) continue;
            double rmin2_ab, rmin2_abc;
            reb_mercurana_predict_rmin2(particles[mi],particles[mj],dt,&rmin2_ab,&rmin2_abc);
            double dcritsum = dcrit[mi]+dcrit[mj];
            if (rmin2_abc< dcritsum*dcritsum){ 
                // j particle will be added later (active particles need to be in array first)
                rim->inshell[mi] = 0;
                rim->map[shell+1][rim->shellN[shell+1]] = mi;
                rim->shellN[shell+1]++;
                break; // only add particle i once
            }

        }
    }
    rim->shellN_active[shell+1] = rim->shellN[shell+1];
    for (int i=N_active; i<N; i++){
        int mi = map[i]; 
        for (int j=0; j<N_active; j++){
            int mj = map[j]; 
            double rmin2_ab, rmin2_abc;
            reb_mercurana_predict_rmin2(particles[mi],particles[mj],dt,&rmin2_ab,&rmin2_abc);
            double dcritsum = dcrit[mi]+dcrit[mj];
            if (rmin2_abc< dcritsum*dcritsum){ 
                rim->inshell[mi] = 0;
                rim->map[shell+1][rim->shellN[shell+1]] = mi;
                rim->shellN[shell+1]++;
                break; // only add particle i once
            }
        }
    }
}

static void reb_integrator_mercurana_preprocessor(struct reb_simulation* const r, double dt, int shell, enum REB_EOS_TYPE type);
static void reb_integrator_mercurana_postprocessor(struct reb_simulation* const r, double dt, int shell, enum REB_EOS_TYPE type);
static void reb_integrator_mercurana_step(struct reb_simulation* const r, double dt, int shell, enum REB_EOS_TYPE type);
    
static void reb_integrator_mercurana_drift_step(struct reb_simulation* const r, double a, unsigned int shell){
    //printf("drift s=%d\n",shell);
    struct reb_simulation_integrator_mercurana* const rim = &(r->ri_mercurana);
    struct reb_particle* restrict const particles = r->particles;
    reb_mercurana_encounter_predict(r, a, shell);
    unsigned int* map = rim->map[shell];
    unsigned int N = rim->shellN[shell];
    for (int i=0;i<N;i++){  // loop over all particles in shell (includes subshells)
        int mi = map[i]; 
        if(rim->inshell[mi]){  // only advance in-shell particles
            particles[mi].x += a*particles[mi].vx;
            particles[mi].y += a*particles[mi].vy;
            particles[mi].z += a*particles[mi].vz;
        }
    }
    if (shell+1<rim->Nmaxshells){ // does sub-shell exist?
        if (rim->shellN[shell+1]>0){ // are there particles in it?
            rim->Nmaxshellused = MAX(rim->Nmaxshellused, shell+2);
            // advance all sub-shell particles
            double as = a/rim->n;
            reb_integrator_mercurana_preprocessor(r, as, shell+1, rim->phi1);
            for (int i=0;i<rim->n;i++){
                reb_integrator_mercurana_step(r, as, shell+1, rim->phi1);
            }
            reb_integrator_mercurana_postprocessor(r, as, shell+1, rim->phi1);
        }
    }
}

static void reb_integrator_mercurana_interaction_step(struct reb_simulation* r, double y, double v, int shell){
    struct reb_simulation_integrator_mercurana* const rim = &(r->ri_mercurana);
    const int N = rim->shellN[shell];
    const int N_active = rim->shellN_active[shell];
    struct reb_particle* const particles = r->particles;
    const int testparticle_type   = r->testparticle_type;
    const double G = r->G;
    unsigned int* map = rim->map[shell];
    const double* dcrit_i = NULL;
    const double* dcrit_c = NULL;
    const double* dcrit_o = NULL;
    if (shell<rim->Nmaxshells-1){
        dcrit_i = r->ri_mercurana.dcrit[shell+1];
    }
    dcrit_c = r->ri_mercurana.dcrit[shell];
    if (shell>0){
        dcrit_o = r->ri_mercurana.dcrit[shell-1];
    }

    double (*_L) (const struct reb_simulation* const r, double d, double dcrit, double fracin) = r->ri_mercurana.L;
    double (*_dLdr) (const struct reb_simulation* const r, double d, double dcrit, double fracin) = r->ri_mercurana.dLdr;
    // Normal force calculation 
    for (int i=0; i<N; i++){
        int mi = map[i];
        particles[mi].ax = 0; 
        particles[mi].ay = 0; 
        particles[mi].az = 0; 
    }
    int starti = 0;
    if (rim->whsplitting && shell==0){
        // Planet star interactions are not in shell 0, but at least in shell 1
        starti = 1;
    }

    for (int i=starti; i<N_active; i++){
        if (reb_sigint) return;
        const int mi = map[i];
        for (int j=i+1; j<N_active; j++){
            const int mj = map[j];
            const double dx = particles[mi].x - particles[mj].x;
            const double dy = particles[mi].y - particles[mj].y;
            const double dz = particles[mi].z - particles[mj].z;
            const double dr = sqrt(dx*dx + dy*dy + dz*dz);
            const double dc_c = dcrit_c[mi]+dcrit_c[mj];
            double Lsum = 0.;
            double dc_o = 0;
            if (dcrit_o && ((!rim->whsplitting) || shell!=1 || i!=0) ){
                // Do not subtract anything for planet/star interactions in shell 1
                dc_o = dcrit_o[mi]+dcrit_o[mj];
                Lsum -= _L(r,dr,dc_c,dc_o);
            }
            double dc_i = 0;
            if (dcrit_i){
                dc_i = dcrit_i[mi]+dcrit_i[mj];
                Lsum += _L(r,dr,dc_i,dc_c);
            }else{
                Lsum += 1; // Innermost
            }

            const double prefact = G*Lsum/(dr*dr*dr);
            const double prefactj = -prefact*particles[mj].m;
            const double prefacti = prefact*particles[mi].m;
            particles[mi].ax    += prefactj*dx;
            particles[mi].ay    += prefactj*dy;
            particles[mi].az    += prefactj*dz;
            particles[mj].ax    += prefacti*dx;
            particles[mj].ay    += prefacti*dy;
            particles[mj].az    += prefacti*dz;
        }
    }
    for (int i=N_active; i<N; i++){
        if (reb_sigint) return;
        const int mi = map[i];
        for (int j=starti; j<N_active; j++){
            const int mj = map[j];
            const double dx = particles[mi].x - particles[mj].x;
            const double dy = particles[mi].y - particles[mj].y;
            const double dz = particles[mi].z - particles[mj].z;
            const double dr = sqrt(dx*dx + dy*dy + dz*dz);
            const double dc_c = dcrit_c[mi]+dcrit_c[mj];
            double Lsum = 0.;
            double dc_o = 0;
            if (dcrit_o && ((!rim->whsplitting) || shell!=1 || j!=0) ){
                // Do not subtract anything for planet/star interactions in shell 1
                dc_o = dcrit_o[mi]+dcrit_o[mj];
                Lsum -= _L(r,dr,dc_c,dc_o);
            }
            double dc_i = 0;
            if (dcrit_i){
                dc_i = dcrit_i[mi]+dcrit_i[mj];
                Lsum += _L(r,dr,dc_i,dc_c);
            }else{
                Lsum += 1; // Innermost
            }

            const double prefact = G*Lsum/(dr*dr*dr);
            const double prefactj = -prefact*particles[mj].m;
            particles[mi].ax    += prefactj*dx;
            particles[mi].ay    += prefactj*dy;
            particles[mi].az    += prefactj*dz;
            if (testparticle_type){
                const double prefacti = prefact*particles[mi].m;
                particles[mj].ax    += prefacti*dx;
                particles[mj].ay    += prefacti*dy;
                particles[mj].az    += prefacti*dz;
            }
        }
    }
    // Jerk calculation
    if (v!=0.){ // is jerk even used?
        struct reb_particle* jerk = rim->jerk; // temorary storage for jerk  
        for (int j=0; j<N; j++){
            jerk[j].ax = 0; 
            jerk[j].ay = 0; 
            jerk[j].az = 0; 
        }
        for (int i=starti; i<N_active; i++){
            const int mi = map[i];
            if (reb_sigint) return;
            for (int j=i+1; j<N_active; j++){
                const int mj = map[j];
                const double dx = particles[mj].x - particles[mi].x; 
                const double dy = particles[mj].y - particles[mi].y; 
                const double dz = particles[mj].z - particles[mi].z; 
                
                const double dax = particles[mj].ax - particles[mi].ax; 
                const double day = particles[mj].ay - particles[mi].ay; 
                const double daz = particles[mj].az - particles[mi].az; 

                const double dr = sqrt(dx*dx + dy*dy + dz*dz);
                const double dc_c = dcrit_c[mi]+dcrit_c[mj];
                double Lsum = 0.;
                double dLdrsum = 0.;
                if (dcrit_o && ((!rim->whsplitting) || shell!=1 || i!=0) ){
                    double dc_o = dcrit_o[mi]+dcrit_o[mj];
                    Lsum    -=    _L(r,dr,dc_c,dc_o);
                    dLdrsum -= _dLdr(r,dr,dc_c,dc_o);
                }
                if (dcrit_i){
                    double dc_i = dcrit_i[mi]+dcrit_i[mj];
                    Lsum    +=    _L(r,dr,dc_i,dc_c);
                    dLdrsum += _dLdr(r,dr,dc_i,dc_c);
                }else{
                    Lsum += 1; // Innermost
                }
                const double alphasum = dax*dx+day*dy+daz*dz;
                const double prefact2 = 2.*G /(dr*dr*dr);
                const double prefact2i = Lsum*prefact2*particles[mi].m;
                const double prefact2j = Lsum*prefact2*particles[mj].m;
                jerk[j].ax    -= dax*prefact2i;
                jerk[j].ay    -= day*prefact2i;
                jerk[j].az    -= daz*prefact2i;
                jerk[i].ax    += dax*prefact2j;
                jerk[i].ay    += day*prefact2j;
                jerk[i].az    += daz*prefact2j;
                const double prefact1 = alphasum*prefact2/dr *(3.*Lsum/dr-dLdrsum);
                const double prefact1i = prefact1*particles[mi].m;
                const double prefact1j = prefact1*particles[mj].m;
                jerk[j].ax    += dx*prefact1i;
                jerk[j].ay    += dy*prefact1i;
                jerk[j].az    += dz*prefact1i;
                jerk[i].ax    -= dx*prefact1j;
                jerk[i].ay    -= dy*prefact1j;
                jerk[i].az    -= dz*prefact1j;
            }
        }
        for (int i=N_active; i<N; i++){
            if (reb_sigint) return;
            const int mi = map[i];
            for (int j=starti; j<N_active; j++){
                const int mj = map[j];
                const double dx = particles[mj].x - particles[mi].x; 
                const double dy = particles[mj].y - particles[mi].y; 
                const double dz = particles[mj].z - particles[mi].z; 
                
                const double dax = particles[mj].ax - particles[mi].ax; 
                const double day = particles[mj].ay - particles[mi].ay; 
                const double daz = particles[mj].az - particles[mi].az; 

                const double dr = sqrt(dx*dx + dy*dy + dz*dz);
                const double dc_c = dcrit_c[mi]+dcrit_c[mj];
                double Lsum = 0.;
                double dLdrsum = 0.;
                if (dcrit_o && ((!rim->whsplitting) || shell!=1 || j!=0) ){
                    double dc_o = dcrit_o[mi]+dcrit_o[mj];
                    Lsum    -=    _L(r,dr,dc_c,dc_o);
                    dLdrsum -= _dLdr(r,dr,dc_c,dc_o);
                }
                if (dcrit_i){
                    double dc_i = dcrit_i[mi]+dcrit_i[mj];
                    Lsum    +=    _L(r,dr,dc_i,dc_c);
                    dLdrsum += _dLdr(r,dr,dc_i,dc_c);
                }else{
                    Lsum += 1; // Innermost
                }
                const double alphasum = dax*dx+day*dy+daz*dz;
                const double prefact2 = 2.*G /(dr*dr*dr);
                const double prefact2j = Lsum*prefact2*particles[mj].m;
                const double prefact1 = alphasum*prefact2/dr*(3.*Lsum/dr-dLdrsum);
                const double prefact1j = prefact1*particles[mj].m;
                jerk[i].ax    += dax*prefact2j;
                jerk[i].ay    += day*prefact2j;
                jerk[i].az    += daz*prefact2j;
                jerk[i].ax    -= dx*prefact1j;
                jerk[i].ay    -= dy*prefact1j;
                jerk[i].az    -= dz*prefact1j;
                if (testparticle_type){
                    const double prefact1i = prefact1*particles[mi].m;
                    const double prefact2i = Lsum*prefact2*particles[mi].m;
                    jerk[j].ax    += dx*prefact1i;
                    jerk[j].ay    += dy*prefact1i;
                    jerk[j].az    += dz*prefact1i;
                    jerk[j].ax    -= dax*prefact2i;
                    jerk[j].ay    -= day*prefact2i;
                    jerk[j].az    -= daz*prefact2i;
                }
            }
        }
        for (int i=0;i<N;i++){
            const int mi = map[i];
            particles[mi].vx += y*particles[mi].ax + v*jerk[i].ax;
            particles[mi].vy += y*particles[mi].ay + v*jerk[i].ay;
            particles[mi].vz += y*particles[mi].az + v*jerk[i].az;
        }
    }else{ // No jerk used
        for (int i=0;i<N;i++){
            const int mi = map[i];
            particles[mi].vx += y*particles[mi].ax;
            particles[mi].vy += y*particles[mi].ay;
            particles[mi].vz += y*particles[mi].az;
        }
    }
}

static void reb_integrator_mercurana_preprocessor(struct reb_simulation* const r, double dt, int shell, enum REB_EOS_TYPE type){
    switch(type){
        case REB_EOS_PMLF6:
            for (int i=0;i<6;i++){
                reb_integrator_mercurana_drift_step(r, dt*reb_eos_pmlf6_z[i], shell);
                reb_integrator_mercurana_interaction_step(r, dt*reb_eos_pmlf6_y[i], dt*dt*dt*reb_eos_pmlf6_v[i], shell);
            }
            break;
        case REB_EOS_PMLF4:
            for (int i=0;i<3;i++){
                reb_integrator_mercurana_interaction_step(r, dt*reb_eos_pmlf4_y[i], 0., shell);
                reb_integrator_mercurana_drift_step(r, dt*reb_eos_pmlf4_z[i], shell);
            }
            break;
        case REB_EOS_PLF7_6_4:
            for (int i=0;i<6;i++){
                reb_integrator_mercurana_drift_step(r, dt*reb_eos_plf7_6_4_z[i], shell);
                reb_integrator_mercurana_interaction_step(r, dt*reb_eos_plf7_6_4_y[i], 0., shell);
            }
            break;
        default:
            break;
    }
}
static void reb_integrator_mercurana_postprocessor(struct reb_simulation* const r, double dt, int shell, enum REB_EOS_TYPE type){
    switch(type){
        case REB_EOS_PMLF6:
            for (int i=5;i>=0;i--){
                reb_integrator_mercurana_interaction_step(r, -dt*reb_eos_pmlf6_y[i], -dt*dt*dt*reb_eos_pmlf6_v[i], shell); 
                reb_integrator_mercurana_drift_step(r, -dt*reb_eos_pmlf6_z[i], shell);
             }
            break;
        case REB_EOS_PMLF4:
            for (int i=2;i>=0;i--){
                reb_integrator_mercurana_drift_step(r, -dt*reb_eos_pmlf4_z[i], shell);
                reb_integrator_mercurana_interaction_step(r, -dt*reb_eos_pmlf4_y[i], 0., shell); 
             }
            break;
        case REB_EOS_PLF7_6_4:
            for (int i=5;i>=0;i--){
                reb_integrator_mercurana_interaction_step(r, -dt*reb_eos_plf7_6_4_y[i], 0., shell);
                reb_integrator_mercurana_drift_step(r, -dt*reb_eos_plf7_6_4_z[i], shell);
            }
            break;
        default:
            break;
    }
}

static void reb_integrator_mercurana_step(struct reb_simulation* const r, double dt, int shell, enum REB_EOS_TYPE type){
    switch(type){
        case REB_EOS_LF:
            reb_integrator_mercurana_drift_step(r, dt*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt, 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*0.5, shell);
            break;
        case REB_EOS_LF4:
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_lf4_a, shell);
            reb_integrator_mercurana_interaction_step(r, dt*2.*reb_eos_lf4_a, 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(0.5-reb_eos_lf4_a), shell);
            reb_integrator_mercurana_interaction_step(r, dt*(1.-4.*reb_eos_lf4_a), 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(0.5-reb_eos_lf4_a), shell);
            reb_integrator_mercurana_interaction_step(r, dt*2.*reb_eos_lf4_a, 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_lf4_a, shell);
            break;
        case REB_EOS_LF6:
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_lf6_a[0]*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf6_a[0], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf6_a[0]+reb_eos_lf6_a[1])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf6_a[1], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf6_a[1]+reb_eos_lf6_a[2])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf6_a[2], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf6_a[2]+reb_eos_lf6_a[3])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf6_a[3], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf6_a[3]+reb_eos_lf6_a[4])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf6_a[4], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf6_a[3]+reb_eos_lf6_a[4])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf6_a[3], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf6_a[2]+reb_eos_lf6_a[3])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf6_a[2], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf6_a[1]+reb_eos_lf6_a[2])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf6_a[1], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf6_a[0]+reb_eos_lf6_a[1])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf6_a[0], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_lf6_a[0]*0.5, shell);
            break; 
        case REB_EOS_LF8: 
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_lf8_a[0]*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[0], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[0]+reb_eos_lf8_a[1])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[1], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[1]+reb_eos_lf8_a[2])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[2], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[2]+reb_eos_lf8_a[3])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[3], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[3]+reb_eos_lf8_a[4])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[4], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[4]+reb_eos_lf8_a[5])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[5], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[5]+reb_eos_lf8_a[6])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[6], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[6]+reb_eos_lf8_a[7])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[7], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[7]+reb_eos_lf8_a[8])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[8], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[7]+reb_eos_lf8_a[8])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[7], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[6]+reb_eos_lf8_a[7])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[6], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[5]+reb_eos_lf8_a[6])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[5], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[4]+reb_eos_lf8_a[5])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[4], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[3]+reb_eos_lf8_a[4])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[3], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[2]+reb_eos_lf8_a[3])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[2], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[1]+reb_eos_lf8_a[2])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[1], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*(reb_eos_lf8_a[0]+reb_eos_lf8_a[1])*0.5, shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_lf8_a[0], 0., shell);
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_lf8_a[0]*0.5, shell);
            break;
        case REB_EOS_LF4_2: 
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_lf4_2_a, shell); 
            reb_integrator_mercurana_interaction_step(r, dt*0.5, 0., shell); 
            reb_integrator_mercurana_drift_step(r, dt*(1.-2.*reb_eos_lf4_2_a), shell);
            reb_integrator_mercurana_interaction_step(r, dt*0.5, 0., shell); 
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_lf4_2_a, shell); 
            break;
        case REB_EOS_LF8_6_4:
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_lf8_6_4_a[0], shell);   
            reb_integrator_mercurana_interaction_step(r, reb_eos_lf8_6_4_b[0]*dt,0, shell);
            reb_integrator_mercurana_drift_step(r, reb_eos_lf8_6_4_a[1]*dt, shell);   
            reb_integrator_mercurana_interaction_step(r, reb_eos_lf8_6_4_b[1]*dt,0, shell);
            reb_integrator_mercurana_drift_step(r, reb_eos_lf8_6_4_a[2]*dt, shell);   
            reb_integrator_mercurana_interaction_step(r, reb_eos_lf8_6_4_b[2]*dt,0, shell);
            reb_integrator_mercurana_drift_step(r, reb_eos_lf8_6_4_a[3]*dt, shell);   
            reb_integrator_mercurana_interaction_step(r, reb_eos_lf8_6_4_b[3]*dt,0, shell);
            reb_integrator_mercurana_drift_step(r, reb_eos_lf8_6_4_a[3]*dt, shell);   
            reb_integrator_mercurana_interaction_step(r, reb_eos_lf8_6_4_b[2]*dt,0, shell);
            reb_integrator_mercurana_drift_step(r, reb_eos_lf8_6_4_a[2]*dt, shell);   
            reb_integrator_mercurana_interaction_step(r, reb_eos_lf8_6_4_b[1]*dt,0, shell);
            reb_integrator_mercurana_drift_step(r, reb_eos_lf8_6_4_a[1]*dt, shell);   
            reb_integrator_mercurana_interaction_step(r, reb_eos_lf8_6_4_b[0]*dt,0, shell);
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_lf8_6_4_a[0], shell);   
            break;
        case REB_EOS_PMLF4:
            reb_integrator_mercurana_drift_step(r, dt*0.5, shell); 
            reb_integrator_mercurana_interaction_step(r, dt, dt*dt*dt/24., shell); 
            reb_integrator_mercurana_drift_step(r, dt*0.5, shell); 
            break;
        case REB_EOS_PMLF6:
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_pmlf6_a[0], shell); 
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_pmlf6_b[0], dt*dt*dt*reb_eos_pmlf6_c[0], shell); 
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_pmlf6_a[1], shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_pmlf6_b[1], dt*dt*dt*reb_eos_pmlf6_c[1], shell); 
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_pmlf6_a[1], shell);
            reb_integrator_mercurana_interaction_step(r, dt*reb_eos_pmlf6_b[0], dt*dt*dt*reb_eos_pmlf6_c[0], shell);
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_pmlf6_a[0], shell); 
            break;
        case REB_EOS_PLF7_6_4:
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_plf7_6_4_a[0], shell);   
            reb_integrator_mercurana_interaction_step(r, reb_eos_plf7_6_4_b[0]*dt,0, shell);
            reb_integrator_mercurana_drift_step(r, reb_eos_plf7_6_4_a[1]*dt, shell);   
            reb_integrator_mercurana_interaction_step(r, reb_eos_plf7_6_4_b[1]*dt,0, shell);
            reb_integrator_mercurana_drift_step(r, reb_eos_plf7_6_4_a[1]*dt, shell);   
            reb_integrator_mercurana_interaction_step(r, reb_eos_plf7_6_4_b[0]*dt,0, shell);
            reb_integrator_mercurana_drift_step(r, dt*reb_eos_plf7_6_4_a[0], shell);   
            break;
    }
}

void reb_integrator_mercurana_part1(struct reb_simulation* r){
    if (r->var_config_N){
        reb_warning(r,"Mercurana does not work with variational equations.");
    }
    
    struct reb_simulation_integrator_mercurana* const rim = &(r->ri_mercurana);
    const int N = r->N;
    
    if (rim->allocatedN<N){
        // dcrit
        if (rim->dcrit){
            for (int i=0;i<rim->Nmaxshells;i++){
                free(rim->dcrit[i]);
            }
        }
        rim->dcrit = realloc(rim->dcrit, sizeof(double*)*(rim->Nmaxshells));
        for (int i=0;i<rim->Nmaxshells;i++){
            rim->dcrit[i] = malloc(sizeof(double)*N);
        }
        // map
        if (rim->map){
            for (int i=0;i<rim->Nmaxshells;i++){
                free(rim->map[i]);
            }
        }
        rim->map = realloc(rim->map, sizeof(unsigned int*)*rim->Nmaxshells);
        for (int i=0;i<rim->Nmaxshells;i++){
            rim->map[i] = malloc(sizeof(unsigned int)*N);
        }
        // inshell
        rim->inshell = realloc(rim->inshell, sizeof(unsigned int)*N);
        // jerk
        rim->jerk = realloc(rim->jerk, sizeof(struct reb_particle)*N);
        // shellN
        rim->shellN = realloc(rim->shellN, sizeof(unsigned int)*rim->Nmaxshells);
        // shellN_active
        rim->shellN_active = realloc(rim->shellN_active, sizeof(unsigned int)*rim->Nmaxshells);

        rim->allocatedN = N;
        // If particle number increased (or this is the first step), need to calculate critical radii
        rim->recalculate_dcrit_this_timestep = 1;
    }

    if (rim->recalculate_dcrit_this_timestep){
        rim->recalculate_dcrit_this_timestep = 0;
        if (rim->is_synchronized==0){
            reb_integrator_mercurana_synchronize(r);
            reb_warning(r,"MERCURANA: Recalculating dcrit but pos/vel were not synchronized before.");
        }
        double dt_shell = r->dt;
        for (int s=0;s<rim->Nmaxshells;s++){ // innermost shell has no dcrit
            for (int i=0;i<N;i++){
                // distance where dt/dt_frac is equal to dynamical timescale
                // Note: particle radius not needed here.
                double T = dt_shell/(rim->dt_frac*2.*M_PI);
                double dcrit = sqrt3(T*T*r->G*r->particles[i].m);
                rim->dcrit[s][i] = dcrit;
            }
            double longest_drift_step_in_shell = 0.5;                        // 2nd + 4th order
            // TODO: think about readding the following.
            //if ((s==0 && rim->order==6) || (s>0 && rim->phi1==6)){  // 6th order
            //    longest_drift_step_in_shell = a_6[1];
            //}
            dt_shell *= longest_drift_step_in_shell;
            dt_shell /= rim->n;
            // Initialize shell numbers to zero (not needed, but helps debugging)
            rim->shellN[s] = 0;
            rim->shellN_active[s] = 0;
        }
        for (int i=0;i<N;i++){
            // Set map to identity for outer-most shell
            rim->map[0][i] = i;
        }

    }
    
    // Calculate collisions only with DIRECT method
    if (r->collision != REB_COLLISION_NONE && r->collision != REB_COLLISION_DIRECT){
        reb_warning(r,"Mercurana only works with a direct collision search.");
    }
    
    // Calculate gravity with special function
    if (r->gravity != REB_GRAVITY_BASIC && r->gravity != REB_GRAVITY_NONE){
        reb_warning(r,"Mercurana has it's own gravity routine. Gravity routine set by the user will be ignored.");
    }
    r->gravity = REB_GRAVITY_NONE;
    
    if (rim->L == NULL){
        // Setting default switching function
        rim->L = reb_integrator_mercurana_L_infinity;
        rim->dLdr = reb_integrator_mercurana_dLdr_infinity;
    }
}

void reb_integrator_mercurana_part2(struct reb_simulation* const r){
    struct reb_simulation_integrator_mercurana* const rim = &(r->ri_mercurana);
    rim->shellN[0] = r->N;
    rim->shellN_active[0] = r->N_active==-1?r->N:r->N_active;

    if (rim->is_synchronized){
        reb_integrator_mercurana_preprocessor(r, r->dt, 0, rim->phi0);
    }
    reb_integrator_mercurana_step(r, r->dt, 0, rim->phi0);

    rim->is_synchronized = 0;
    if (rim->safe_mode){
        reb_integrator_mercurana_synchronize(r);
    }

    r->t+=r->dt;
    r->dt_last_done = r->dt;
}

void reb_integrator_mercurana_synchronize(struct reb_simulation* r){
    struct reb_simulation_integrator_mercurana* const rim = &(r->ri_mercurana);
    if (rim->is_synchronized == 0){
        r->gravity = REB_GRAVITY_NONE; // needed here again for SimulationArchive
        if (rim->L == NULL){
            // Setting default switching function
            rim->L = reb_integrator_mercurana_L_infinity;
            rim->dLdr = reb_integrator_mercurana_dLdr_infinity;
        }
        reb_integrator_mercurana_postprocessor(r, r->dt, 0, rim->phi0);
        rim->is_synchronized = 1;
    }
}

void reb_integrator_mercurana_reset(struct reb_simulation* r){
    if (r->ri_mercurana.allocatedN){
        for (int i=0;i<r->ri_mercurana.Nmaxshells;i++){
            free(r->ri_mercurana.map[i]);
            free(r->ri_mercurana.dcrit[i]);
        }
        free(r->ri_mercurana.map);
        free(r->ri_mercurana.dcrit);
        free(r->ri_mercurana.inshell);
        free(r->ri_mercurana.shellN);
        free(r->ri_mercurana.shellN_active);
        free(r->ri_mercurana.jerk);
    }
    r->ri_mercurana.allocatedN = 0;
    r->ri_mercurana.map = NULL;
    r->ri_mercurana.dcrit = NULL;
    r->ri_mercurana.inshell = NULL;
    r->ri_mercurana.shellN = NULL;
    r->ri_mercurana.shellN_active = NULL;
    r->ri_mercurana.jerk = NULL;
    
    r->ri_mercurana.phi0 = REB_EOS_LF;
    r->ri_mercurana.phi1 = REB_EOS_LF;
    r->ri_mercurana.n = 10;
    r->ri_mercurana.whsplitting = 1;
    r->ri_mercurana.safe_mode = 1;
    r->ri_mercurana.dt_frac = 0.1;
    r->ri_mercurana.Nmaxshells = 10;
    r->ri_mercurana.Nmaxshellused = 1;
    r->ri_mercurana.recalculate_dcrit_this_timestep = 0;
    r->ri_mercurana.is_synchronized = 1;
    r->ri_mercurana.L = NULL;
    r->ri_mercurana.dLdr = NULL;
    
}

