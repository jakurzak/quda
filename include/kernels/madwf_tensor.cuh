#pragma once

#include <color_spinor_field.h>
#include <color_spinor_field_order.h>
#include <shared_memory_cache_helper.cuh>
#include <kernels/madwf_transfer.cuh>
#include <block_reduce_helper.h>

/**
  @file This file contains the argument and kernel that forms a tensor outer product in the fifth
  dimension given two input vectors,
      T_{st} = out_s * in_t,
  where s and t are fifth dimension indices. T_{st} is a spin matrix so T has shape Ls*4-by-Ls*4.
*/

namespace quda
{

  namespace madwf_ml
  {
    template <class T> __device__ __host__ inline void block_reduce_x(T &f)
    {
      int lane_id_x = target::thread_idx().x % device::warp_size();
      int warp_id_x = target::thread_idx().x / device::warp_size();
      int warp_ct_x = target::block_dim().x / device::warp_size();

      // pad = false, dynamic = false, basically to get a __shared__ T smem[32];
      SharedMemoryCache<T, 32, 1, false, false> cache;
      T *smem = cache.data();

      WarpReduce<T, device::warp_size()> warp_reduce;
      f = warp_reduce.Sum(f);
      // Now lane 0 of each warp holds the reduced value

      if (warp_ct_x > 1) {
        if (lane_id_x == 0) { smem[target::thread_idx().y * warp_ct_x + warp_id_x] = f; }
        cache.sync();
        if (warp_id_x == 0) {
          f = (lane_id_x < warp_ct_x) ? smem[target::thread_idx().y * warp_ct_x + lane_id_x] : 0;
          f = warp_reduce.Sum(f);
        }
      }
      // Now the first thread in the x direction holds the reduction result.
    }

    /**
      @brief Form a 4-by-4 tensor from v and w (color d.o.f. is contracted)
      @param[in] mp The buffer to accumulate on
      @param[in] v, w input vectors
     */
    template <class real>
    __device__ __host__ inline void vector_tensor_matrix(SpinMatrix<real> *mp, int m_index, const WilsonVector<real> &v,
                                                         const WilsonVector<real> &w)
    {
      complex<real> *p = reinterpret_cast<complex<real> *>(mp);

#pragma unroll
      for (int a = 0; a < spin_dim; a++) {
#pragma unroll
        for (int b = 0; b < spin_dim; b++) {
          int cs = a * spin_dim + b;
          complex<real> z = conj(innerProduct(v, w, a, b));

          // Perform a block reduction across the x direction
          block_reduce_x(z);

          if (target::thread_idx().x == 0) {
            p[(m_index * spin_dim * spin_dim + cs) * target::grid_dim().x + target::block_idx().x] = z;
          }
        }
      }
    }

    template <class storage_t, class matrix_t_, int block_dim_x_> struct Tensor5DArg : kernel_param<> {

      using F = typename colorspinor_mapper<storage_t, 4, 3>::type;
      using real = typename mapper<storage_t>::type;
      using Vector = ColorSpinor<real, 3, 4>;
      using matrix_t = matrix_t_;

      static constexpr int block_dim_x = block_dim_x_;

      const F out; // output vector field
      const F in;  // input vector field

      const int Ls_out; // length of 5th dimension of the out field
      const int Ls_in;  // length of 5th dimension of the in field

      const int volume_4d_cb;

      matrix_t *reduce_buffer = nullptr;

      const int nParity;

      matrix_t *result_d;

      int batch;

      Tensor5DArg(const ColorSpinorField &out, const ColorSpinorField &in, int num_x_blocks, matrix_t *result_d) :
        kernel_param(dim3(out.VolumeCB() / out.X(4), out.X(4), out.SiteSubset())),
        out(out),
        in(in),
        Ls_out(out.X(4)),
        Ls_in(in.X(4)),
        volume_4d_cb(in.VolumeCB() / in.X(4)),
        nParity(in.SiteSubset()),
        result_d(result_d),
        batch(num_x_blocks)
      {

        if (volume_4d_cb != static_cast<int>(out.VolumeCB() / Ls_out)) {
          errorQuda("Input and Output fields should have the same 4d volume: %d neq %d.\n", volume_4d_cb,
                    static_cast<int>(out.VolumeCB() / Ls_out));
        }

        if (in.Nspin() != 4) errorQuda("nSpin = %d not supported", in.Nspin());
        if (in.Ncolor() != 3) errorQuda("nColor = %d not supported", in.Ncolor());
        if (out.Nspin() != 4) errorQuda("nSpin = %d not supported", out.Nspin());
        if (out.Ncolor() != 3) errorQuda("nColor = %d not supported", out.Ncolor());

        checkNative(in, out);

        size_t reduce_bytes = num_x_blocks * sizeof(matrix_t) * out.X(4) * in.X(4);
        reduce_buffer = reinterpret_cast<matrix_t *>(device_malloc(reduce_bytes));
      }

      ~Tensor5DArg()
      {
        if (reduce_buffer) { device_free(reduce_buffer); }
      }
    };

    template <class Arg> struct Tensor5D {

      const Arg &arg;
      constexpr Tensor5D(const Arg &arg) : arg(arg) { }
      static constexpr const char *filename() { return KERNEL_FILE; }

      /**
        @brief Form a Ls_in-by-Ls_out tensor product from the two input vectors. The type of reduce_buffer
                determines if the resulting tensor has spin and/or color d.o.f, or just two chiral d.o.f
        @param[in] parity Parity we are on
        @param[in] x_b Checkerboarded 4-d space-time index
        @param[in] s Ls dimension coordinate
       */
      __device__ __host__ inline void operator()(int x_cb, int s, int parity)
      {
        using Vector = typename Arg::Vector;

        const int Ls_in = arg.Ls_in;
        const int volume_4d_cb = arg.volume_4d_cb;
        auto reduce_buffer = arg.reduce_buffer;

        SharedMemoryCache<Vector> cache({target::block_dim().x, static_cast<unsigned int>(Ls_in), 1});

        int t = s;
        while (t < Ls_in) {
          cache.save_y(arg.in(t * volume_4d_cb + x_cb, parity), t);
          t += target::block_dim().y;
        }
        cache.sync();

        // t -> s_in, s-> s_out
        const Vector v = arg.out(s * volume_4d_cb + x_cb, parity);
        for (t = 0; t < Ls_in; t++) {
          const Vector w = cache.load_y(t);
          int wm_index = s * Ls_in + t;
          vector_tensor_matrix(reduce_buffer, wm_index, v, w);
        }
      }
    };

    template <class Arg> struct Tensor5DReduce {

      const Arg &arg;
      constexpr Tensor5DReduce(const Arg &arg) : arg(arg) { }
      static constexpr const char *filename() { return KERNEL_FILE; }

      __device__ __host__ inline void operator()(int, int, int)
      {
        using T = complex<typename Arg::real>;

        int thread_idx = target::thread_idx().x;
        int batch = arg.batch;

        T *in = reinterpret_cast<T *>(arg.reduce_buffer);
        T z = 0;
        while (thread_idx < batch) {
          z += in[target::block_idx().x * batch + thread_idx];
          thread_idx += target::block_dim().x;
        }

        BlockReduce<T, Arg::block_dim_x> block_reduce;
        z = block_reduce.Sum(z);

        T *out = reinterpret_cast<T *>(arg.result_d);
        if (target::thread_idx().x == 0) { out[target::block_idx().x] = z; }
      }
    };
  } // namespace madwf_ml

} // namespace quda
