#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <omp.h>

/* 
   hard-wire simulation parameters
*/
#define NUM 20000
#define TS  10

int init(double*, double*, double*, double*, double*, double*, double*, int);
void output_particles(double*, double*, double*, double*, double*, double*, double*, int);
void calc_centre_mass(double*, double*, double*, double*, double*, double, int);

int main(int argc, char* argv[]) {
  int i, j;
  int num;     // user defined (argv[1]) total number of gas molecules in simulation
  int time, timesteps; // for time stepping, including user defined (argv[2]) number of timesteps to integrate
  int rc;      // return code
  double *mass, *x, *y, *z, *vx, *vy, *vz;  // 1D array for mass, (x,y,z) position, (vx, vy, vz) velocity
  double dx, dy, dz, d, F, GRAVCONST=0.001;
  double ax, ay, az;
  double *old_x, *old_y, *old_z, *old_mass;            // save previous values whilst doing global updates
  double totalMass;  
  double com[3];
  double start=omp_get_wtime();   // we make use of the simple wall clock timer available in OpenMP 

  // MPI variables
  int myThread, numPEs;
  
  /* determine size of system */
  num = NUM;
  timesteps = TS;

  printf("Initializing for %d particles in x,y,z space...", num);

  /* malloc arrays and pass ref to init(). NOTE: init() uses random numbers */
  mass = (double *) malloc(num * sizeof(double));
  x =  (double *) malloc(num * sizeof(double));
  y =  (double *) malloc(num * sizeof(double));
  z =  (double *) malloc(num * sizeof(double));
  vx = (double *) malloc(num * sizeof(double));
  vy = (double *) malloc(num * sizeof(double));
  vz = (double *) malloc(num * sizeof(double));
  old_x = (double *) malloc(num * sizeof(double));
  old_y = (double *) malloc(num * sizeof(double));
  old_z = (double *) malloc(num * sizeof(double));
  old_mass = (double *) malloc(num * sizeof(double));

  // should check all rc but let's just see if last malloc worked
  if (old_mass == NULL) {
    printf("\n ERROR in malloc for (at least) old_mass - aborting\n");
    return -99;
  } 
  else {
    printf("  (malloc-ed)  ");
  }

  // initialise
  rc = init(mass, x, y, z, vx, vy, vz, num);
  if (rc != 0) {
    printf("\n ERROR during init() - aborting\n");
    return -99;
  }
  else {
    printf("  INIT COMPLETE\n");
  }

  totalMass = 0.0;
  #pragma omp parallel for reduction(+:totalMass)
  for (i=0; i<num; i++) {
    totalMass += mass[i];
  }
  // DEBUG: output_particles(x,y,z, vx,vy,vz, mass, num);
  // output a metric (centre of mass) for checking
  calc_centre_mass(com, x,y,z,mass,totalMass,num);
  printf("At t=0, centre of mass = (%g,%g,%g)\n", com[0], com[1], com[2]);


  /* 
     MAIN TIME STEPPING LOOP
  */


  printf("Now to integrate for %d timesteps\n", timesteps);

 
  // time=0 was initial conditions
  for (time=1; time<=timesteps; time++) {

    // LOOP1: take snapshot to use on RHS when looping for updates
    // all MPI processes do this
    for (i=0; i<num; i++) {
      old_x[i] = x[i];
      old_y[i] = y[i];
      old_z[i] = z[i];
      old_mass[i] = mass[i];
    }

    double temp_d, temp_z;

    // LOOP2: update position etc per particle (based on old data)
    // we split this loop between OpenMP threads
#pragma omp parallel for default(none) \
  private(i,j,dx,dy,dz,temp_d,d,F,ax,ay,az)	\
  shared(vx,vy,vz,num,old_x,old_y,old_z,x,y,z,mass,old_mass,GRAVCONST)
    for(i=0; i<num; i++) {
      double vxLocal = 0.0;
      double vyLocal = 0.0;
      double vzLocal = 0.0;
      // calc forces on body i due to particles (j != i)
      for (j=0; j<num; j++) {
        if (j != i) {
          dx = old_x[j] - x[i];
          dy = old_y[j] - y[i];
	  dz = old_z[j] - z[i];
          temp_d =sqrt(dx*dx + dy*dy + dz*dz);
          d = temp_d>0.01 ? temp_d : 0.01;
          F = GRAVCONST * mass[i] * old_mass[j] / (d*d);
	  // calculate acceleration due to the force, F
          ax = (F/mass[i]) * dx/d;
          ay = (F/mass[i]) * dy/d;
	  az = (F/mass[i]) * dz/d;
	  // approximate LOCAL velocities in "unit time"
          vxLocal += ax;
          vyLocal += ay;
	  vzLocal += az;
        }
      } 
      //update GLOBAL velocities
      vx[i] += vxLocal;
      vy[i] += vyLocal;
      vz[i] += vzLocal;

      // calc new position 
      x[i] = old_x[i] + vx[i];
      y[i] = old_y[i] + vy[i];
      z[i] = old_z[i] + vz[i];

    } // end of LOOP 2

    // at this point each MPI process has
    // updated some x,y,z & some vx,vy,vz
    // so we use MPI_Allgather to ensure all MPI processes have all updates
    

    
    //DEBUG: output_particles(x,y,z, vx,vy,vz, mass, num);    
    // output a metric (centre of mass) for checking
    calc_centre_mass(com, x,y,z,mass,totalMass,num);
    printf("End of timestep %d, centre of mass = (%.3f,%.3f,%.3f)\n", time, com[0], com[1], com[2]);
  } // time steps

  printf("Time to init+solve %d molecules for %d timesteps is %g seconds\n", num, timesteps, omp_get_wtime()-start);
  // output a metric (centre of mass) for checking
  calc_centre_mass(com, x,y,z,mass,totalMass,num);
  printf("Centre of mass = (%.5f,%.5f,%.5f)\n", com[0], com[1], com[2]);
} // main


int init(double *mass, double *x, double *y, double *z, double *vx, double *vy, double *vz, int num) {
  // random numbers to set initial conditions - do not parallelise or amend order of random number usage
  int i;
  double comp;
  double min_pos = -50.0, mult = +100.0, maxVel = +5.0;
  double recip = 1.0 / (double)RAND_MAX;

  for (i=0; i<num; i++) {
    x[i] = min_pos + mult*(double)rand() * recip;  
    y[i] = min_pos + mult*(double)rand() * recip;  
    z[i] = 0.0 + mult*(double)rand() * recip;   
    vx[i] = -maxVel + 2.0*maxVel*(double)rand() * recip;   
    vy[i] = -maxVel + 2.0*maxVel*(double)rand() * recip;   
    vz[i] = -maxVel + 2.0*maxVel*(double)rand() * recip;
    mass[i] = 0.1 + 10*(double)rand() * recip;  // mass is 0.1 up to 10.1
  }
  return 0;
} // init


void output_particles(double *x, double *y, double *z, double *vx, double *vy, double *vz, double *mass, int n) {
  int i;
  printf("num \t position (x,y,z) \t velocity (vx, vy, vz)\t mass \n");
  for (i=0; i<n; i++) {
    printf("%d \t %f %f %f \t %f %f %f \t %f \n", i, x[i], y[i], z[i], vx[i], vy[i], vz[i], mass[i]);
  }
}


void calc_centre_mass(double *com, double *x, double *y, double *z, double *mass, double totalMass, int N) {
  int i, axis;
   // calculate the centre of mass, com(x,y,z)
  for (axis=0; axis<2; axis++) {
    com[0] = 0.0;     com[1] = 0.0;     com[2] = 0.0; 
    for (i=0; i<N; i++) {
      com[0] += mass[i]*x[i];
      com[1] += mass[i]*y[i];
      com[2] += mass[i]*z[i];
    }
    com[0] /= totalMass;
    com[1] /= totalMass;
    com[2] /= totalMass;
  }
  return;
}
