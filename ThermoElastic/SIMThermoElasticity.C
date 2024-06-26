// $Id$
//==============================================================================
//!
//! \file SIMThermoElasticity.C
//!
//! \date Aug 05 2014
//!
//! \author Arne Morten Kvarving / SINTEF
//!
//! \brief Wrapper equipping the elasticity solver with temperature coupling.
//!
//==============================================================================

#include "SIMThermoElasticity.h"

#include "ElasticityUtils.h"
#include "Linear/AnalyticSolutions.h"
#include "SIMElasticityWrap.h"
#include "ThermoElasticity.h"

#include "ASMstruct.h"
#include "Profiler.h"
#include "IFEM.h"
#include "SIM2D.h"
#include "SIM3D.h"
#include "TimeStep.h"
#include "Utilities.h"

#include "tinyxml2.h"


template<class Dim>
SIMThermoElasticity<Dim>::SIMThermoElasticity ()
{
  SIMElasticity<Dim>::myContext = "thermoelasticity";
  Dim::myHeading = "Thermo-Elasticity solver";
  startT = 0.0;
}


template<class Dim>
SIMThermoElasticity<Dim>::~SIMThermoElasticity ()
{
  this->setVTF(nullptr);
}


template<class Dim>
bool SIMThermoElasticity<Dim>::saveStep (const TimeStep& tp, int& nBlock)
{
  if (tp.time.t+0.001*tp.time.dt < startT)
    return true;

  PROFILE1("SIMThermoElasticity::saveStep");

  double old_tol = utl::zero_print_tol;
  utl::zero_print_tol = 1e-16;
  bool ok = this->savePoints(this->getSolution(),tp.time.t,tp.step);
  utl::zero_print_tol = old_tol;

  if (Dim::opt.format < 0 || !ok)
    return ok;

  int iDump = 1 + tp.step/Dim::opt.saveInc;
  return this->writeGlvS(this->getSolution(),iDump,nBlock);
}


template<class Dim>
bool SIMThermoElasticity<Dim>::solveStep (TimeStep& tp)
{
  if (tp.time.t+0.001*tp.time.dt < startT)
    return true;

  PROFILE1("SIMThermoElasticity::solveStep");

  this->setMode(SIM::STATIC);
  this->setQuadratureRule(Dim::opt.nGauss[0]);
  if (!this->assembleSystem())
    return false;

  if (!this->solveSystem(SIMsolution::solution.front(),1))
    return false;

  Vectors gNorm;
  this->setMode(SIM::RECOVERY);
  this->setQuadratureRule(Dim::opt.nGauss[1]);
  if (!this->solutionNorms(tp.time,this->getSolutions(),gNorm))
    return false;
  else if (gNorm.empty())
    return true;

  IFEM::cout <<"Energy norm |u^h| = a(u^h,u^h)^0.5   : "<< gNorm[0](1);
  if (gNorm[0](2) != 0.0)
    IFEM::cout <<"\nExternal energy ((f,u^h)+(t,u^h)^0.5 : "<< gNorm[0](2);
  if (this->haveAnaSol() && gNorm[0].size() >= 4)
    IFEM::cout <<"\nExact norm  |u|   = a(u,u)^0.5       : "<< gNorm[0](3)
               <<"\nExact error a(e,e)^0.5, e=u-u^h      : "<< gNorm[0](4)
               <<"\nExact relative error (%) : "
               << gNorm[0](4)/gNorm[0](3)*100.0;
  IFEM::cout << std::endl;
  return true;
}


template<class Dim>
const RealFunc* SIMThermoElasticity<Dim>::getInitialTemperature () const
{
  ThermoElasticity* thelp = dynamic_cast<ThermoElasticity*>(Dim::myProblem);
  return thelp ? thelp->getInitialTemperature() : nullptr;
}


template<class Dim>
bool SIMThermoElasticity<Dim>::parse (const tinyxml2::XMLElement* elem)
{
  if (strcasecmp(elem->Value(),"thermoelasticity"))
    return this->Dim::parse(elem);

  const tinyxml2::XMLElement* child = elem->FirstChildElement();
  for (; child; child = child->NextSiblingElement())
    if (!strcasecmp(child->Value(),"start"))
      if (utl::getAttribute(child,"time",startT))
        IFEM::cout <<"\tStart time elasticity solver: "<< startT << std::endl;

  return this->SIMElasticityWrap<Dim>::parse(elem);
}


template<class Dim>
bool SIMThermoElasticity<Dim>::parseAnaSol (const tinyxml2::XMLElement* elem)
{
  IFEM::cout <<"  Parsing <"<< elem->Value() <<">"<< std::endl;

  std::string type;
  utl::getAttribute(elem,"type",type,true);
  if (type == "pipe")
  {
    double Ri = 0.0, Ro = 0.0, Ti = 0.0, To = 0.0, T0 = 273.0;
    double E = 0.0, nu = 0.0, alpha = 0.0;
    bool polar = false;
    utl::getAttribute(elem,"Ri",Ri);
    utl::getAttribute(elem,"Ro",Ro);
    utl::getAttribute(elem,"Ti",Ti);
    utl::getAttribute(elem,"To",To);
    utl::getAttribute(elem,"Tref",T0);
    utl::getAttribute(elem,"E",E);
    utl::getAttribute(elem,"nu",nu);
    utl::getAttribute(elem,"alpha",alpha);
    utl::getAttribute(elem,"polar",polar);
    IFEM::cout <<"\tAnalytical solution: Pipe Ri="<< Ri <<" Ro="<< Ro
               <<" Ti="<< Ti <<" To="<< To << std::endl;
    if (!Dim::mySol)
      Dim::mySol = new AnaSol(new Pipe(Ri,Ro,Ti,To,T0,E,nu,alpha,
                                       Dim::dimension == 3, polar));
  }

  return true;
}


template<class Dim>
ElasticBase* SIMThermoElasticity<Dim>::getIntegrand ()
{
  if (!Dim::myProblem)
  {
    if (Dim::dimension == 2)
      Dim::myProblem = new ThermoElasticity(2,Elastic::axiSymmetry);
    else
      Dim::myProblem = new ThermoElasticity(Dim::dimension);
  }
  return dynamic_cast<ElasticBase*>(Dim::myProblem);
}


template<class Dim>
int SolverConfigurator< SIMThermoElasticity<Dim> >::
setup (SIMThermoElasticity<Dim>& elasim, const bool&, char* infile)
{
  // Read the input file
  ASMstruct::resetNumbering();
  if (!elasim.readModel(infile))
    return 2;

  // Preprocess the model and establish FE data structures
  elasim.opt.print(IFEM::cout) << std::endl;
  if (!elasim.preprocess())
    return 3;

  // Initialize the linear equation system solver and solution vectors
  if (!elasim.init(TimeStep()))
    return 4;

  return 0;
}


template class SIMThermoElasticity< SIM2D >;
template struct SolverConfigurator< SIMThermoElasticity<SIM2D> >;
template class SIMThermoElasticity< SIM3D >;
template struct SolverConfigurator< SIMThermoElasticity<SIM3D> >;
