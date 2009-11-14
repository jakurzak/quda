#ifndef _ENUM_QUDA_H
#define _ENUM_QUDA_H

#ifdef __cplusplus
extern "C" {
#endif

  typedef enum QudaGaugeFieldOrder_s {
    QUDA_QDP_GAUGE_ORDER, // expect *gauge[4], even-odd, row-column colour
    QUDA_CPS_WILSON_GAUGE_ORDER, // expect *gauge, even-odd, mu inside, column-row colour
  } QudaGaugeFieldOrder;

  typedef enum QudaDiracFieldOrder_s {
    QUDA_DIRAC_ORDER, // even-odd, colour inside spin
    QUDA_QDP_DIRAC_ORDER, // even-odd, spin inside colour
    QUDA_CPS_WILSON_DIRAC_ORDER, // odd-even, colour inside spin
    QUDA_LEX_DIRAC_ORDER // lexicographical order, colour inside spin
  } QudaDiracFieldOrder;  

  typedef enum QudaCloverFieldOrder_s {
    QUDA_PACKED_CLOVER_ORDER, // even-odd, packed
    QUDA_LEX_PACKED_CLOVER_ORDER // lexicographical order, packed
  } QudaCloverFieldOrder;

  typedef enum QudaDslashType_s {
    QUDA_WILSON_DSLASH,
    QUDA_CLOVER_WILSON_DSLASH
  } QudaDslashType;

  typedef enum QudaInverterType_s {
    QUDA_CG_INVERTER,
    QUDA_BICGSTAB_INVERTER
  } QudaInverterType;

  typedef enum QudaPrecision_s {
    QUDA_HALF_PRECISION = 2,
    QUDA_SINGLE_PRECISION = 4,
    QUDA_DOUBLE_PRECISION = 8
  } QudaPrecision;

  // Whether the preconditioned matrix is (1-k^2 Deo Doe) or (1-k^2 Doe Deo)
  //
  // For the clover-improved Wilson Dirac operator, QUDA_MATPC_EVEN_EVEN
  // defaults to the "symmetric" form, (1 - k^2 A_ee^-1 D_eo A_oo^-1 D_oe),
  // and likewise for QUDA_MATPC_ODD_ODD.
  //
  // For the "asymmetric" form, (A_ee - k^2 D_eo A_oo^-1 D_oe), select
  // QUDA_MATPC_EVEN_EVEN_ASYMMETRIC.
  //
  typedef enum QudaMatPCType_s {
    QUDA_MATPC_EVEN_EVEN,
    QUDA_MATPC_ODD_ODD,
    QUDA_MATPC_EVEN_EVEN_ASYMMETRIC,
    QUDA_MATPC_ODD_ODD_ASYMMETRIC
  } QudaMatPCType;

  // The different solutions supported
  typedef enum QudaSolutionType_s {
    QUDA_MAT_SOLUTION,
    QUDA_MATPC_SOLUTION,
    QUDA_MATPCDAG_SOLUTION, // not implemented
    QUDA_MATPCDAG_MATPC_SOLUTION,
  } QudaSolutionType;

  typedef enum QudaMassNormalization_s {
    QUDA_KAPPA_NORMALIZATION,
    QUDA_MASS_NORMALIZATION,
    QUDA_ASYMMETRIC_MASS_NORMALIZATION
  } QudaMassNormalization;

  typedef enum QudaPreserveSource_s {
    QUDA_PRESERVE_SOURCE_NO, // use the source for the residual
    QUDA_PRESERVE_SOURCE_YES // keep the source intact
  } QudaPreserveSource;

  typedef enum QudaReconstructType_s {
    QUDA_RECONSTRUCT_NO, // store all 18 real numbers explicitly
    QUDA_RECONSTRUCT_8, // reconstruct from 8 real numbers
    QUDA_RECONSTRUCT_12 // reconstruct from 12 real numbers
  } QudaReconstructType;

  typedef enum QudaGaugeFixed_s {
    QUDA_GAUGE_FIXED_NO, // No gauge fixing
    QUDA_GAUGE_FIXED_YES // Gauge field stored in temporal gauge
  } QudaGaugeFixed;

  typedef enum QudaDagType_s {
    QUDA_DAG_NO,
    QUDA_DAG_YES
  } QudaDagType;
  
  typedef enum QudaTboundary_s {
    QUDA_ANTI_PERIODIC_T = -1,
    QUDA_PERIODIC_T = 1
  } QudaTboundary;

  typedef enum QudaVerbosity_s {
    QUDA_SILENT,
    QUDA_SUMMARIZE,
    QUDA_VERBOSE
  } QudaVerbosity;

  typedef struct double3_s {
    double x;
    double y;
    double z;
  } double3;

#ifdef __cplusplus
}
#endif

#endif // _ENUM_QUDA_H