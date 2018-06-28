#include <invert_quda.h>
#include <blas_quda.h>

#include <Eigen/Dense>

namespace quda {

  CAGCR::CAGCR(DiracMatrix &mat, DiracMatrix &matSloppy, SolverParam &param, TimeProfile &profile)
    : Solver(param, profile), mat(mat), matSloppy(matSloppy), init(false),
      alpha(nullptr), rp(nullptr), tmpp(nullptr), tmp_sloppy(nullptr) { }

  CAGCR::~CAGCR() {
    if (!param.is_preconditioner) profile.TPSTART(QUDA_PROFILE_FREE);

    if (init) {
      if (alpha) delete []alpha;
      for (int i=0; i<param.Nkrylov; i++) delete p[i];
      for (int i=0; i<param.Nkrylov; i++) delete q[i];
      if (tmp_sloppy) delete tmp_sloppy;
      if (tmpp) delete tmpp;
      if (rp) delete rp;
    }

    if (!param.is_preconditioner) profile.TPSTOP(QUDA_PROFILE_FREE);
  }

  void CAGCR::create(const ColorSpinorField &meta)
  {
    if (!init) {
      if (!param.is_preconditioner) {
        blas::flops = 0;
        profile.TPSTART(QUDA_PROFILE_INIT);
      }

      alpha = new Complex[param.Nkrylov];

      bool mixed = param.precision != param.precision_sloppy;

      ColorSpinorParam csParam(meta);
      csParam.create = QUDA_NULL_FIELD_CREATE;

      // Source needs to be preserved if we're computing the true residual
      rp = mixed ? ColorSpinorField::Create(csParam) : nullptr;
      tmpp = ColorSpinorField::Create(csParam);

      // now allocate sloppy fields
      csParam.setPrecision(param.precision_sloppy);

      p.resize(param.Nkrylov);
      q.resize(param.Nkrylov);
      for (int i=0; i<param.Nkrylov; i++) {
        p[i] = ColorSpinorField::Create(csParam);
        q[i] = ColorSpinorField::Create(csParam);
      }

      //sloppy temporary for mat-vec
      tmp_sloppy = mixed ? ColorSpinorField::Create(csParam) : nullptr;

      if (!param.is_preconditioner) profile.TPSTOP(QUDA_PROFILE_INIT);

      init = true;
    } // init
  }

  void CAGCR::solve(Complex *psi_, std::vector<ColorSpinorField*> &p, std::vector<ColorSpinorField*> &q, ColorSpinorField &b)
  {
    using namespace Eigen;
    typedef Matrix<Complex, Dynamic, Dynamic> matrix;
    typedef Matrix<Complex, Dynamic, 1> vector;

    const int N = p.size();
    vector phi(N), psi(N);
    matrix A(N,N);

#if 1
    // only a single reduction but requires using the full dot product 
    // compute rhs vector phi = P* b = (p_i, b)
    std::vector<ColorSpinorField*> Q;
    for (int i=0; i<N; i++) Q.push_back(q[i]);
    Q.push_back(&b);

    // Construct the matrix P A P = (p_i, A p_j) = (p_i, q_j)
    Complex *A_ = new Complex[N*(N+1)];
    blas::cDotProduct(A_, q, Q);
    for (int i=0; i<N; i++) {
      phi(i) = A_[i*(N+1)+N];
      for (int j=0; j<N; j++) {
        A(i,j) = A_[i*(N+1)+j];
      }
    }
#else
    // two reductions but uses the Hermitian block dot product
    // compute rhs vector phi = P* b = (p_i, b)
    std::vector<ColorSpinorField*> B;
    B.push_back(&b);
    Complex *phi_ = new Complex[N];
    blas::cDotProduct(phi_,q, B);
    for (int i=0; i<N; i++) phi(i) = phi_[i];
    delete phi_;

    // Construct the matrix AP AP = (A p_i, A p_j) = (q_i, q_j)
    Complex *A_ = new Complex[N*N];
    blas::hDotProduct(A_, q, q);
    for (int i=0; i<N; i++)
      for (int j=0; j<N; j++)
        A(i,j) = A_[i*N+j];
#endif
    delete A_;

    if (!param.is_preconditioner) {
      profile.TPSTOP(QUDA_PROFILE_COMPUTE);
      param.secs += profile.Last(QUDA_PROFILE_COMPUTE);
      profile.TPSTART(QUDA_PROFILE_EIGEN);
    }

    // use partial pivoted LU since this seems plenty stable
#if 1
    PartialPivLU<matrix> lu(A);
    psi = lu.solve(phi);
#else
    JacobiSVD<matrix> svd(A, ComputeThinU | ComputeThinV);
    psi = svd.solve(phi);
#endif

    for (int i=0; i<N; i++) psi_[i] = psi(i);

    if (!param.is_preconditioner) {
      profile.TPSTOP(QUDA_PROFILE_EIGEN);
      param.secs += profile.Last(QUDA_PROFILE_EIGEN);
      profile.TPSTART(QUDA_PROFILE_COMPUTE);
    }

  }

  /*
    We want to find the best initial guess of the solution of
    A x = b, and we have N previous solutions x_i.
    The method goes something like this:
    
    1. Orthonormalise the p_i and q_i
    2. Form the matrix G_ij = x_i^dagger A x_j
    3. Form the vector B_i = x_i^dagger b
    4. solve A_ij a_j  = B_i
    5. x = a_i p_i
  */
  void CAGCR::operator()(ColorSpinorField &x, ColorSpinorField &b)
  {
    const int nKrylov = param.Nkrylov;

    if (checkPrecision(x,b) != param.precision) errorQuda("Precision mismatch %d %d", checkPrecision(x,b), param.precision);

    if (param.maxiter == 0 || nKrylov == 0) {
      if (param.use_init_guess == QUDA_USE_INIT_GUESS_NO) blas::zero(x);
      return;
    }

    create(x);

    ColorSpinorField &r = rp ? *rp : *p[0];
    ColorSpinorField &tmp = *tmpp;
    ColorSpinorField &tmpSloppy = tmp_sloppy ? *tmp_sloppy : tmp;

    if (!param.is_preconditioner) profile.TPSTART(QUDA_PROFILE_PREAMBLE);

    double b2 = blas::norm2(b);  //Save norm of b
    double r2 = 0.0; // if zero source then we will exit immediately doing no work

    // compute intitial residual depending on whether we have an initial guess or not
    if (param.use_init_guess == QUDA_USE_INIT_GUESS_YES) {
      mat(r, x, tmp);
      r2 = blas::xmyNorm(b, r);   //r = b - Ax0
    } else {
      r2 = b2;
      blas::copy(r, b);
      blas::zero(x);
    }

    // Check to see that we're not trying to invert on a zero-field source
    if (b2 == 0) {
      if (param.compute_null_vector == QUDA_COMPUTE_NULL_VECTOR_NO) {
	warningQuda("inverting on zero-field source\n");
	x = b;
	param.true_res = 0.0;
	param.true_res_hq = 0.0;
	return;
      } else {
	b2 = r2;
      }
    }

    double stop = stopping(param.tol, b2, param.residual_type); // stopping condition of solver

    const bool use_heavy_quark_res = (param.residual_type & QUDA_HEAVY_QUARK_RESIDUAL) ? true : false;

    // this parameter determines how many consective reliable update
    // reisudal increases we tolerate before terminating the solver,
    // i.e., how long do we want to keep trying to converge
    const int maxResIncrease = param.max_res_increase; // check if we reached the limit of our tolerance
    const int maxResIncreaseTotal = param.max_res_increase_total;

    double heavy_quark_res = 0.0; // heavy quark residual
    if(use_heavy_quark_res) heavy_quark_res = sqrt(blas::HeavyQuarkResidualNorm(x,r).z);

    int resIncrease = 0;
    int resIncreaseTotal = 0;

    if (!param.is_preconditioner) {
      blas::flops = 0;
      profile.TPSTOP(QUDA_PROFILE_PREAMBLE);
      profile.TPSTART(QUDA_PROFILE_COMPUTE);
    }
    int total_iter = 0;
    int restart = 0;
    double r2_old = r2;
    bool l2_converge = false;

    blas::copy(*p[0], r); // no op if uni-precision

    PrintStats("CA-GCR", total_iter, r2, b2, heavy_quark_res);
    while ( !convergence(r2, heavy_quark_res, stop, param.tol_hq) && total_iter < param.maxiter) {

      // build up a space of size nKrylov
      for (int k=0; k<nKrylov; k++) {
        matSloppy(*q[k], *p[k], tmpSloppy);
        if (k<nKrylov-1) blas::copy(*p[k+1], *q[k]);
      }

      solve(alpha, p, q, *p[0]);

      // update the solution vector
      std::vector<ColorSpinorField*> X;
      X.push_back(&x);
      blas::caxpy(alpha, p, X);

      // update the residual vector
      std::vector<ColorSpinorField*> R;
      R.push_back(p[0]);
      for (int i=0; i<nKrylov; i++) alpha[i] = -alpha[i];
      blas::caxpy(alpha, q, R);

      r2 = blas::norm2(*p[0]);
      total_iter+=nKrylov;

      PrintStats("CA-GCR", total_iter, r2, b2, heavy_quark_res);

      // update since nKrylov or maxiter reached, converged or reliable update required
      // note that the heavy quark residual will by definition only be checked every nKrylov steps
      if (1 || total_iter>=param.maxiter || (r2 < stop && !l2_converge) || sqrt(r2/r2_old) < param.delta) {

	if (r2 < stop && param.sloppy_converge) break;
	mat(r, x, tmp);
	r2 = blas::xmyNorm(b, r);  
	if (use_heavy_quark_res) heavy_quark_res = sqrt(blas::HeavyQuarkResidualNorm(x, r).z);

        // break-out check if we have reached the limit of the precision
	if (r2 > r2_old) {
	  resIncrease++;
	  resIncreaseTotal++;
	  warningQuda("CA-GCR: new reliable residual norm %e is greater than previous reliable residual norm %e (total #inc %i)",
		      sqrt(r2), sqrt(r2_old), resIncreaseTotal);
	  if (resIncrease > maxResIncrease or resIncreaseTotal > maxResIncreaseTotal) {
	    warningQuda("CA-GCR: solver exiting due to too many true residual norm increases");
	    break;
	  }
	} else {
	  resIncrease = 0;
	}

	if ( !convergence(r2, heavy_quark_res, stop, param.tol_hq) ) {
	  restart++; // restarting if residual is still too great

	  PrintStats("CA-GCR (restart)", restart, r2, b2, heavy_quark_res);
	  blas::copy(*p[0], r);

	  r2_old = r2;

	  // prevent ending the Krylov space prematurely if other convergence criteria not met 
	  if (r2 < stop) l2_converge = true; 
	}

	r2_old = r2;
      }

    }

    if (total_iter>param.maxiter && getVerbosity() >= QUDA_SUMMARIZE)
      warningQuda("Exceeded maximum iterations %d", param.maxiter);

    if (getVerbosity() >= QUDA_VERBOSE) printfQuda("CA-GCR: number of restarts = %d\n", restart);

    if (param.compute_true_res) {
      // Calculate the true residual
      mat(r, x, tmp);
      double true_res = blas::xmyNorm(b, r);
      param.true_res = sqrt(true_res / b2);
      param.true_res_hq = (param.residual_type & QUDA_HEAVY_QUARK_RESIDUAL) ? sqrt(blas::HeavyQuarkResidualNorm(x,r).z) : 0.0;
      if (param.preserve_source == QUDA_PRESERVE_SOURCE_NO) blas::copy(b, r);
    } else {
      if (param.preserve_source == QUDA_PRESERVE_SOURCE_NO) blas::copy(b, *p[0]);
    }

    if (!param.is_preconditioner) {
      qudaDeviceSynchronize(); // ensure solver is complete before ending timing
      profile.TPSTOP(QUDA_PROFILE_COMPUTE);
      profile.TPSTART(QUDA_PROFILE_EPILOGUE);
      param.secs += profile.Last(QUDA_PROFILE_COMPUTE);

      // store flops and reset counters
      double gflops = (blas::flops + mat.flops() + matSloppy.flops())*1e-9;

      param.gflops += gflops;
      param.iter += total_iter;

      // reset the flops counters
      blas::flops = 0;
      mat.flops();
      matSloppy.flops();

      profile.TPSTOP(QUDA_PROFILE_EPILOGUE);
    }

    PrintSummary("CA-GCR", total_iter, r2, b2);
  }

} // namespace quda
