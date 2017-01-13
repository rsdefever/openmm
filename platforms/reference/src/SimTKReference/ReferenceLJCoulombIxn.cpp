
/* Portions copyright (c) 2006-2013 Stanford University and Simbios.
 * Contributors: Pande Group
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <sstream>
#include <complex>
#include <algorithm>

#include "SimTKOpenMMUtilities.h"
#include "ReferenceLJCoulombIxn.h"
#include "ReferenceForce.h"
#include "ReferencePME.h"
#include "openmm/OpenMMException.h"

// In case we're using some primitive version of Visual Studio this will
// make sure that erf() and erfc() are defined.
#include "openmm/internal/MSVC_erfc.h"

using std::set;
using std::vector;
using namespace OpenMM;

/**---------------------------------------------------------------------------------------

   ReferenceLJCoulombIxn constructor

   --------------------------------------------------------------------------------------- */

ReferenceLJCoulombIxn::ReferenceLJCoulombIxn() : cutoff(false), useSwitch(false), periodic(false), ewald(false), pme(false) {
}

/**---------------------------------------------------------------------------------------

   ReferenceLJCoulombIxn destructor

   --------------------------------------------------------------------------------------- */

ReferenceLJCoulombIxn::~ReferenceLJCoulombIxn() {
}

  /**---------------------------------------------------------------------------------------

     Set the force to use a cutoff.

     @param distance            the cutoff distance
     @param neighbors           the neighbor list to use
     @param solventDielectric   the dielectric constant of the bulk solvent

     --------------------------------------------------------------------------------------- */

  void ReferenceLJCoulombIxn::setUseCutoff(double distance, const OpenMM::NeighborList& neighbors, double solventDielectric) {

    cutoff = true;
    cutoffDistance = distance;
    neighborList = &neighbors;
    krf = pow(cutoffDistance, -3.0)*(solventDielectric-1.0)/(2.0*solventDielectric+1.0);
    crf = (1.0/cutoffDistance)*(3.0*solventDielectric)/(2.0*solventDielectric+1.0);
  }

/**---------------------------------------------------------------------------------------

   Set the force to use a switching function on the Lennard-Jones interaction.

   @param distance            the switching distance

   --------------------------------------------------------------------------------------- */

void ReferenceLJCoulombIxn::setUseSwitchingFunction(double distance) {
    useSwitch = true;
    switchingDistance = distance;
}

  /**---------------------------------------------------------------------------------------

     Set the force to use periodic boundary conditions.  This requires that a cutoff has
     also been set, and the smallest side of the periodic box is at least twice the cutoff
     distance.

     @param vectors    the vectors defining the periodic box

     --------------------------------------------------------------------------------------- */

  void ReferenceLJCoulombIxn::setPeriodic(OpenMM::Vec3* vectors) {

    assert(cutoff);
    assert(vectors[0][0] >= 2.0*cutoffDistance);
    assert(vectors[1][1] >= 2.0*cutoffDistance);
    assert(vectors[2][2] >= 2.0*cutoffDistance);
    periodic = true;
    periodicBoxVectors[0] = vectors[0];
    periodicBoxVectors[1] = vectors[1];
    periodicBoxVectors[2] = vectors[2];
  }

  /**---------------------------------------------------------------------------------------

     Set the force to use Ewald summation.

     @param alpha  the Ewald separation parameter
     @param kmaxx  the largest wave vector in the x direction
     @param kmaxy  the largest wave vector in the y direction
     @param kmaxz  the largest wave vector in the z direction

     --------------------------------------------------------------------------------------- */

  void ReferenceLJCoulombIxn::setUseEwald(double alpha, int kmaxx, int kmaxy, int kmaxz) {
      alphaEwald = alpha;
      numRx = kmaxx;
      numRy = kmaxy;
      numRz = kmaxz;
      ewald = true;
  }

  /**---------------------------------------------------------------------------------------

     Set the force to use Particle-Mesh Ewald (PME) summation.

     @param alpha  the Ewald separation parameter
     @param gridSize the dimensions of the mesh

     --------------------------------------------------------------------------------------- */

  void ReferenceLJCoulombIxn::setUsePME(double alpha, int meshSize[3]) {
      alphaEwald = alpha;
      meshDim[0] = meshSize[0];
      meshDim[1] = meshSize[1];
      meshDim[2] = meshSize[2];
      pme = true;
  }

/**---------------------------------------------------------------------------------------

   Calculate Ewald ixn

   @param numberOfAtoms    number of atoms
   @param atomCoordinates  atom coordinates
   @param atomParameters   atom parameters                             atomParameters[atomIndex][paramterIndex]
   @param exclusions       atom exclusion indices
                           exclusions[atomIndex] contains the list of exclusions for that atom
   @param fixedParameters  non atom parameters (not currently used)
   @param forces           force array (forces added)
   @param energyByAtom     atom energy
   @param totalEnergy      total energy
   @param includeDirect      true if direct space interactions should be included
   @param includeReciprocal  true if reciprocal space interactions should be included

   --------------------------------------------------------------------------------------- */

void ReferenceLJCoulombIxn::calculateEwaldIxn(int numberOfAtoms, vector<Vec3>& atomCoordinates,
                                             double** atomParameters, vector<set<int> >& exclusions,
                                             double* fixedParameters, vector<Vec3>& forces,
                                             double* energyByAtom, double* totalEnergy, bool includeDirect, bool includeReciprocal) const {
    typedef std::complex<double> d_complex;

    static const double epsilon     =  1.0;

    int kmax                            = (ewald ? std::max(numRx, std::max(numRy,numRz)) : 0);
    double factorEwald              = -1 / (4*alphaEwald*alphaEwald);
    double SQRT_PI                  = sqrt(PI_M);
    double TWO_PI                   = 2.0 * PI_M;
    double recipCoeff               = ONE_4PI_EPS0*4*PI_M/(periodicBoxVectors[0][0] * periodicBoxVectors[1][1] * periodicBoxVectors[2][2]) /epsilon;

    double totalSelfEwaldEnergy     = 0.0;
    double realSpaceEwaldEnergy     = 0.0;
    double recipEnergy              = 0.0;
    double totalRecipEnergy         = 0.0;
    double vdwEnergy                = 0.0;

// **************************************************************************************
// SELF ENERGY
// **************************************************************************************

    if (includeReciprocal) {
        for (int atomID = 0; atomID < numberOfAtoms; atomID++) {
            double selfEwaldEnergy       = ONE_4PI_EPS0*atomParameters[atomID][QIndex]*atomParameters[atomID][QIndex] * alphaEwald/SQRT_PI;
            totalSelfEwaldEnergy            -= selfEwaldEnergy;
            if (energyByAtom) {
                energyByAtom[atomID]        -= selfEwaldEnergy;
            }
        }
    }

    if (totalEnergy) {
        *totalEnergy += totalSelfEwaldEnergy;
    }

// **************************************************************************************
// RECIPROCAL SPACE EWALD ENERGY AND FORCES
// **************************************************************************************
    // PME

  if (pme && includeReciprocal) {
    pme_t          pmedata; /* abstract handle for PME data */

    pme_init(&pmedata,alphaEwald,numberOfAtoms,meshDim,5,1);

    vector<double> charges(numberOfAtoms);
    for (int i = 0; i < numberOfAtoms; i++)
        charges[i] = atomParameters[i][QIndex];
    pme_exec(pmedata,atomCoordinates,forces,charges,periodicBoxVectors,&recipEnergy);

    if (totalEnergy)
       *totalEnergy += recipEnergy;

    if (energyByAtom)
        for (int n = 0; n < numberOfAtoms; n++)
            energyByAtom[n] += recipEnergy;

        pme_destroy(pmedata);
  }

    // Ewald method

  else if (ewald && includeReciprocal) {

    // setup reciprocal box

         double recipBoxSize[3] = { TWO_PI / periodicBoxVectors[0][0], TWO_PI / periodicBoxVectors[1][1], TWO_PI / periodicBoxVectors[2][2]};


    // setup K-vectors

  #define EIR(x, y, z) eir[(x)*numberOfAtoms*3+(y)*3+z]
  vector<d_complex> eir(kmax*numberOfAtoms*3);
  vector<d_complex> tab_xy(numberOfAtoms);
  vector<d_complex> tab_qxyz(numberOfAtoms);

  if (kmax < 1)
      throw OpenMMException("kmax for Ewald summation < 1");

  for (int i = 0; (i < numberOfAtoms); i++) {
    for (int m = 0; (m < 3); m++)
      EIR(0, i, m) = d_complex(1,0);

    for (int m=0; (m<3); m++)
      EIR(1, i, m) = d_complex(cos(atomCoordinates[i][m]*recipBoxSize[m]),
                               sin(atomCoordinates[i][m]*recipBoxSize[m]));

    for (int j=2; (j<kmax); j++)
      for (int m=0; (m<3); m++)
        EIR(j, i, m) = EIR(j-1, i, m) * EIR(1, i, m);
  }

    // calculate reciprocal space energy and forces

    int lowry = 0;
    int lowrz = 1;

    for (int rx = 0; rx < numRx; rx++) {

      double kx = rx * recipBoxSize[0];

      for (int ry = lowry; ry < numRy; ry++) {

        double ky = ry * recipBoxSize[1];

        if (ry >= 0) {
          for (int n = 0; n < numberOfAtoms; n++)
            tab_xy[n] = EIR(rx, n, 0) * EIR(ry, n, 1);
        }

        else {
          for (int n = 0; n < numberOfAtoms; n++)
            tab_xy[n]= EIR(rx, n, 0) * conj (EIR(-ry, n, 1));
        }

        for (int rz = lowrz; rz < numRz; rz++) {

          if (rz >= 0) {
           for (int n = 0; n < numberOfAtoms; n++)
             tab_qxyz[n] = atomParameters[n][QIndex] * (tab_xy[n] * EIR(rz, n, 2));
          }

          else {
            for (int n = 0; n < numberOfAtoms; n++)
              tab_qxyz[n] = atomParameters[n][QIndex] * (tab_xy[n] * conj(EIR(-rz, n, 2)));
          }

          double cs = 0.0f;
          double ss = 0.0f;

          for (int n = 0; n < numberOfAtoms; n++) {
            cs += tab_qxyz[n].real();
            ss += tab_qxyz[n].imag();
          }

          double kz = rz * recipBoxSize[2];
          double k2 = kx * kx + ky * ky + kz * kz;
          double ak = exp(k2*factorEwald) / k2;

          for (int n = 0; n < numberOfAtoms; n++) {
            double force = ak * (cs * tab_qxyz[n].imag() - ss * tab_qxyz[n].real());
            forces[n][0] += 2 * recipCoeff * force * kx ;
            forces[n][1] += 2 * recipCoeff * force * ky ;
            forces[n][2] += 2 * recipCoeff * force * kz ;
          }

          recipEnergy       = recipCoeff * ak * (cs * cs + ss * ss);
          totalRecipEnergy += recipEnergy;

          if (totalEnergy)
             *totalEnergy += recipEnergy;

          if (energyByAtom)
             for (int n = 0; n < numberOfAtoms; n++)
               energyByAtom[n] += recipEnergy;

          lowrz = 1 - numRz;
        }
        lowry = 1 - numRy;
      }
    }
  }

// **************************************************************************************
// SHORT-RANGE ENERGY AND FORCES
// **************************************************************************************

    if (!includeDirect)
        return;
    double totalVdwEnergy            = 0.0f;
    double totalRealSpaceEwaldEnergy = 0.0f;

    for (int i = 0; i < (int) neighborList->size(); i++) {
       OpenMM::AtomPair pair = (*neighborList)[i];
       int ii = pair.first;
       int jj = pair.second;

       double deltaR[2][ReferenceForce::LastDeltaRIndex];
       ReferenceForce::getDeltaRPeriodic(atomCoordinates[jj], atomCoordinates[ii], periodicBoxVectors, deltaR[0]);
       double r         = deltaR[0][ReferenceForce::RIndex];
       double inverseR  = 1.0/(deltaR[0][ReferenceForce::RIndex]);
       double switchValue = 1, switchDeriv = 0;
       if (useSwitch && r > switchingDistance) {
           double t = (r-switchingDistance)/(cutoffDistance-switchingDistance);
           switchValue = 1+t*t*t*(-10+t*(15-t*6));
           switchDeriv = t*t*(-30+t*(60-t*30))/(cutoffDistance-switchingDistance);
       }
       double alphaR    = alphaEwald * r;


       double dEdR      = ONE_4PI_EPS0 * atomParameters[ii][QIndex] * atomParameters[jj][QIndex] * inverseR * inverseR * inverseR;
              dEdR      = dEdR * (erfc(alphaR) + 2 * alphaR * exp (- alphaR * alphaR) / SQRT_PI);

       double sig       = atomParameters[ii][SigIndex] +  atomParameters[jj][SigIndex];
       double sig2      = inverseR*sig;
              sig2     *= sig2;
       double sig6      = sig2*sig2*sig2;
       double eps       = atomParameters[ii][EpsIndex]*atomParameters[jj][EpsIndex];
                  dEdR     += switchValue*eps*(12.0*sig6 - 6.0)*sig6*inverseR*inverseR;
       vdwEnergy = eps*(sig6-1.0)*sig6;
       if (useSwitch) {
           dEdR -= vdwEnergy*switchDeriv*inverseR;
           vdwEnergy *= switchValue;
       }

       // accumulate forces

       for (int kk = 0; kk < 3; kk++) {
          double force = dEdR*deltaR[0][kk];
          forces[ii][kk] += force;
          forces[jj][kk] -= force;
       }

       // accumulate energies

       realSpaceEwaldEnergy        = ONE_4PI_EPS0*atomParameters[ii][QIndex]*atomParameters[jj][QIndex]*inverseR*erfc(alphaR);

       totalVdwEnergy             += vdwEnergy;
       totalRealSpaceEwaldEnergy  += realSpaceEwaldEnergy;

        if (energyByAtom) {
           energyByAtom[ii] += realSpaceEwaldEnergy + vdwEnergy;
           energyByAtom[jj] += realSpaceEwaldEnergy + vdwEnergy;
        }

    }

    if (totalEnergy)
        *totalEnergy += totalRealSpaceEwaldEnergy + totalVdwEnergy;

    // Now subtract off the exclusions, since they were implicitly included in the reciprocal space sum.

    double totalExclusionEnergy = 0.0f;
    const double TWO_OVER_SQRT_PI = 2/sqrt(PI_M);
    for (int i = 0; i < numberOfAtoms; i++)
        for (set<int>::const_iterator iter = exclusions[i].begin(); iter != exclusions[i].end(); ++iter) {
            if (*iter > i) {
               int ii = i;
               int jj = *iter;

               double deltaR[2][ReferenceForce::LastDeltaRIndex];
               ReferenceForce::getDeltaR(atomCoordinates[jj], atomCoordinates[ii], deltaR[0]);
               double r         = deltaR[0][ReferenceForce::RIndex];
               double inverseR  = 1.0/(deltaR[0][ReferenceForce::RIndex]);
               double alphaR    = alphaEwald * r;
               if (erf(alphaR) > 1e-6) {
                   double dEdR      = ONE_4PI_EPS0 * atomParameters[ii][QIndex] * atomParameters[jj][QIndex] * inverseR * inverseR * inverseR;
                          dEdR      = dEdR * (erf(alphaR) - 2 * alphaR * exp (- alphaR * alphaR) / SQRT_PI);

                   // accumulate forces

                   for (int kk = 0; kk < 3; kk++) {
                      double force  = dEdR*deltaR[0][kk];
                      forces[ii][kk]   -= force;
                      forces[jj][kk]   += force;
                   }

                   // accumulate energies

                   realSpaceEwaldEnergy = ONE_4PI_EPS0*atomParameters[ii][QIndex]*atomParameters[jj][QIndex]*inverseR*erf(alphaR);
               }
               else {
                   realSpaceEwaldEnergy = alphaEwald*TWO_OVER_SQRT_PI*ONE_4PI_EPS0*atomParameters[ii][QIndex]*atomParameters[jj][QIndex];
               }

               totalExclusionEnergy += realSpaceEwaldEnergy;
               if (energyByAtom) {
                   energyByAtom[ii] -= realSpaceEwaldEnergy;
                   energyByAtom[jj] -= realSpaceEwaldEnergy;
               }
            }
        }

    if (totalEnergy)
        *totalEnergy -= totalExclusionEnergy;
}


/**---------------------------------------------------------------------------------------

   Calculate LJ Coulomb pair ixn

   @param numberOfAtoms    number of atoms
   @param atomCoordinates  atom coordinates
   @param atomParameters   atom parameters                             atomParameters[atomIndex][paramterIndex]
   @param exclusions       atom exclusion indices
                           exclusions[atomIndex] contains the list of exclusions for that atom
   @param fixedParameters  non atom parameters (not currently used)
   @param forces           force array (forces added)
   @param energyByAtom     atom energy
   @param totalEnergy      total energy
   @param includeDirect      true if direct space interactions should be included
   @param includeReciprocal  true if reciprocal space interactions should be included

   --------------------------------------------------------------------------------------- */

void ReferenceLJCoulombIxn::calculatePairIxn(int numberOfAtoms, vector<Vec3>& atomCoordinates,
                                             double** atomParameters, vector<set<int> >& exclusions,
                                             double* fixedParameters, vector<Vec3>& forces,
                                             double* energyByAtom, double* totalEnergy, bool includeDirect, bool includeReciprocal) const {

   if (ewald || pme) {
       calculateEwaldIxn(numberOfAtoms, atomCoordinates, atomParameters, exclusions, fixedParameters, forces, energyByAtom,
               totalEnergy, includeDirect, includeReciprocal);
       return;
   }
   if (!includeDirect)
       return;
   if (cutoff) {
       for (int i = 0; i < (int) neighborList->size(); i++) {
           OpenMM::AtomPair pair = (*neighborList)[i];
           calculateOneIxn(pair.first, pair.second, atomCoordinates, atomParameters, forces, energyByAtom, totalEnergy);
       }
   }
   else {
       for (int ii = 0; ii < numberOfAtoms; ii++) {
          // loop over atom pairs

          for (int jj = ii+1; jj < numberOfAtoms; jj++)
              if (exclusions[jj].find(ii) == exclusions[jj].end())
                  calculateOneIxn(ii, jj, atomCoordinates, atomParameters, forces, energyByAtom, totalEnergy);
       }
   }
}

  /**---------------------------------------------------------------------------------------

     Calculate LJ Coulomb pair ixn between two atoms

     @param ii               the index of the first atom
     @param jj               the index of the second atom
     @param atomCoordinates  atom coordinates
     @param atomParameters   atom parameters (charges, c6, c12, ...)     atomParameters[atomIndex][paramterIndex]
     @param forces           force array (forces added)
     @param energyByAtom     atom energy
     @param totalEnergy      total energy

     --------------------------------------------------------------------------------------- */

void ReferenceLJCoulombIxn::calculateOneIxn(int ii, int jj, vector<Vec3>& atomCoordinates,
                        double** atomParameters, vector<Vec3>& forces,
                        double* energyByAtom, double* totalEnergy) const {

    double deltaR[2][ReferenceForce::LastDeltaRIndex];

    // get deltaR, R2, and R between 2 atoms

    if (periodic)
        ReferenceForce::getDeltaRPeriodic(atomCoordinates[jj], atomCoordinates[ii], periodicBoxVectors, deltaR[0]);
    else
        ReferenceForce::getDeltaR(atomCoordinates[jj], atomCoordinates[ii], deltaR[0]);

    double r2        = deltaR[0][ReferenceForce::R2Index];
    double inverseR  = 1.0/(deltaR[0][ReferenceForce::RIndex]);
    double switchValue = 1, switchDeriv = 0;
    if (useSwitch) {
        double r = deltaR[0][ReferenceForce::RIndex];
        if (r > switchingDistance) {
            double t = (r-switchingDistance)/(cutoffDistance-switchingDistance);
            switchValue = 1+t*t*t*(-10+t*(15-t*6));
            switchDeriv = t*t*(-30+t*(60-t*30))/(cutoffDistance-switchingDistance);
        }
    }
    double sig       = atomParameters[ii][SigIndex] +  atomParameters[jj][SigIndex];
    double sig2      = inverseR*sig;
           sig2     *= sig2;
    double sig6      = sig2*sig2*sig2;

    double eps       = atomParameters[ii][EpsIndex]*atomParameters[jj][EpsIndex];
    double dEdR      = switchValue*eps*(12.0*sig6 - 6.0)*sig6;
    if (cutoff)
        dEdR += ONE_4PI_EPS0*atomParameters[ii][QIndex]*atomParameters[jj][QIndex]*(inverseR-2.0f*krf*r2);
    else
        dEdR += ONE_4PI_EPS0*atomParameters[ii][QIndex]*atomParameters[jj][QIndex]*inverseR;
    dEdR     *= inverseR*inverseR;
    double energy = eps*(sig6-1.0)*sig6;
    if (useSwitch) {
        dEdR -= energy*switchDeriv*inverseR;
        energy *= switchValue;
    }
    if (cutoff)
        energy += ONE_4PI_EPS0*atomParameters[ii][QIndex]*atomParameters[jj][QIndex]*(inverseR+krf*r2-crf);
    else
        energy += ONE_4PI_EPS0*atomParameters[ii][QIndex]*atomParameters[jj][QIndex]*inverseR;

    // accumulate forces

    for (int kk = 0; kk < 3; kk++) {
       double force = dEdR*deltaR[0][kk];
       forces[ii][kk] += force;
       forces[jj][kk] -= force;
    }

    // accumulate energies

    if (totalEnergy)
       *totalEnergy += energy;
    if (energyByAtom) {
       energyByAtom[ii] += energy;
       energyByAtom[jj] += energy;
    }
  }

