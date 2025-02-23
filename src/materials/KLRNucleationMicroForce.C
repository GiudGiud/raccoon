//* This file is part of the RACCOON application
//* being developed at Dolbow lab at Duke University
//* http://dolbow.pratt.duke.edu

#include "Function.h"
#include "KLRNucleationMicroForce.h"

registerADMooseObject("raccoonApp", KLRNucleationMicroForce);

InputParameters
KLRNucleationMicroForce::validParams()
{
  InputParameters params = Material::validParams();
  params += BaseNameInterface::validParams();

  params.addClassDescription("This class computes the external driving force for nucleation given "
                             "a Drucker-Prager strength envelope developed by Kumar et al. (2022)");

  params.addParam<MaterialPropertyName>(
      "fracture_toughness", "Gc", "energy release rate or fracture toughness");
  params.addParam<MaterialPropertyName>(
      "normalization_constant", "c0", "The normalization constant $c_0$");
  params.addParam<MaterialPropertyName>(
      "regularization_length", "l", "the phase field regularization length");

  params.addParam<MaterialPropertyName>("lambda", "lambda", "Lame's first parameter lambda");
  params.addParam<MaterialPropertyName>("shear_modulus", "G", "shear modulus mu or G");

  params.addRequiredParam<MaterialPropertyName>(
      "tensile_strength", "The tensile strength of the material beyond which the material fails.");

  params.addRequiredParam<MaterialPropertyName>(
      "compressive_strength",
      "The compressive strength of the material beyond which the material fails.");

  params.addRequiredParam<MaterialPropertyName>("delta", "delta");
  params.addParam<MaterialPropertyName>(
      "external_driving_force_name",
      "ex_driving",
      "Name of the material that holds the external_driving_force");
  params.addParam<MaterialPropertyName>(
      "stress_balance_name",
      "stress_balance",
      "Name of the stress balance function $F= \\dfrac{J_2}{\\mu} + \\dfrac{I_1^2}{9\\kappa} - c_e "
      "-\\dfrac{3\\Gc}{8\\delta}=0 $. This value tells how close the material is to stress "
      "surface.");
  params.addParam<MaterialPropertyName>("stress_name", "stress", "Name of the stress tensor");
  params.addRequiredCoupledVar("phase_field", "Name of the phase-field (damage) variable");

  params.addParam<MaterialPropertyName>("degradation_function", "g", "The degradation function");
  return params;
}

KLRNucleationMicroForce::KLRNucleationMicroForce(const InputParameters & parameters)
  : Material(parameters),
    BaseNameInterface(parameters),
    _ex_driving(declareADProperty<Real>(prependBaseName("external_driving_force_name", true))),
    _Gc(getADMaterialProperty<Real>(prependBaseName("fracture_toughness", true))),
    _c0(getADMaterialProperty<Real>(prependBaseName("normalization_constant", true))),
    _L(getADMaterialProperty<Real>(prependBaseName("regularization_length", true))),
    _lambda(getADMaterialProperty<Real>(prependBaseName("lambda", true))),
    _mu(getADMaterialProperty<Real>(prependBaseName("shear_modulus", true))),
    _sigma_ts(getADMaterialProperty<Real>(prependBaseName("tensile_strength", true))),
    _sigma_cs(getADMaterialProperty<Real>(prependBaseName("compressive_strength", true))),
    _delta(getADMaterialProperty<Real>(prependBaseName("delta", true))),
    _stress(getADMaterialProperty<RankTwoTensor>(prependBaseName("stress_name", true))),
    _stress_balance(declareADProperty<Real>(prependBaseName("stress_balance_name", true))),
    _druck_prager_balance(declareADProperty<Real>("druck_prager_balance")),
    _d_name(getVar("phase_field", 0)->name()),
    _g_name(prependBaseName("degradation_function", true)),
    _g(getADMaterialProperty<Real>(_g_name)),
    _dg_dd(getADMaterialProperty<Real>(derivativePropertyName(_g_name, {_d_name})))
{
}

void
KLRNucleationMicroForce::computeQpProperties()
{
  // The bulk modulus
  ADReal K = _lambda[_qp] + 2 * _mu[_qp] / 3;

  // The mobility
  ADReal M = _Gc[_qp] / _L[_qp] / _c0[_qp];

  // Invariants of the stress
  ADReal I1 = _stress[_qp].trace();
  ADRankTwoTensor stress_dev = _stress[_qp].deviatoric();
  ADReal J2 = 0.5 * stress_dev.doubleContraction(stress_dev);

  // Just to be extra careful... J2 is for sure non-negative but descritization and interpolation
  // might bring surprise
  mooseAssert(J2 >= 0, "Negative J2");

  // define zero J2's derivative
  if (MooseUtils::absoluteFuzzyEqual(J2, 0))
    J2.value() = libMesh::TOLERANCE * libMesh::TOLERANCE;

  if (MooseUtils::absoluteFuzzyEqual(I1, 0))
    I1.value() = libMesh::TOLERANCE;

  // Parameters in the strength surface
  ADReal gamma_0 = _sigma_ts[_qp] / 6.0 / (3.0 * _lambda[_qp] + 2.0 * _mu[_qp]) +
                   _sigma_ts[_qp] / 6.0 / _mu[_qp];
  ADReal gamma_1 = (1.0 + _delta[_qp]) / (2.0 * _sigma_ts[_qp] * _sigma_cs[_qp]);
  ADReal beta_0 = _delta[_qp] * M;
  ADReal beta_1 = -gamma_1 * M * (_sigma_cs[_qp] - _sigma_ts[_qp]) + gamma_0;
  ADReal beta_2 = std::sqrt(3.0) * (-gamma_1 * M * (_sigma_cs[_qp] + _sigma_ts[_qp]) + gamma_0);
  ADReal beta_3 = _L[_qp] * _sigma_ts[_qp] / _mu[_qp] / K / _Gc[_qp];

  // Compute the external driving force required to recover the desired strength envelope.
  _ex_driving[_qp] =
      (beta_2 * std::sqrt(J2) + beta_1 * I1 + beta_0) +
      (1.0 - std::sqrt(I1 * I1) / I1) / std::pow(_g[_qp], 1.5) *
          (J2 / 2.0 / _mu[_qp] + I1 * I1 / 6.0 / (3.0 * _lambda[_qp] + 2.0 * _mu[_qp]));

  _stress_balance[_qp] = J2 / _mu[_qp] + std::pow(I1, 2) / 9.0 / K - _ex_driving[_qp] - M;

  _druck_prager_balance[_qp] =
      std::sqrt(J2) +
      (_sigma_cs[_qp] - _sigma_ts[_qp]) / std::sqrt(3.0) / (_sigma_cs[_qp] + _sigma_ts[_qp]) * I1 -
      2.0 * _sigma_ts[_qp] * _sigma_cs[_qp] / std::sqrt(3.0) / (_sigma_cs[_qp] + _sigma_ts[_qp]);
}
