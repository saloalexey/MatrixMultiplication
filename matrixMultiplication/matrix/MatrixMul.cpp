
#include "matrixMultiplication/matrix/MatrixMul.hpp"
#include "matrixMultiplication/utils/utils.hpp"

#include "openblas/cblas.h"

#include <immintrin.h>
#include <future>
#include <new>
#include <vector>
#include <numeric>
#include <array>

constexpr auto nthreads = 4u;
constexpr auto SM       = (64 / sizeof(double));

// TODO:
// 2. initMatrix with diff values
// 3. add support of arbitrary matrix size

/*****************     KERNELS     *******************/

void kernelMulMatrix_BL_NV(double*       m_res,
                           const double* m_mul1,
                           const double* m_mul2,
                           std::size_t   block_size)
{
    const double *a, *b;
    double*       r;
    int           i2{}, k2{}, j2{};
    for (i2 = 0, r = m_res, a = m_mul1; i2 < block_size; ++i2, r += N, a += N)
        for (k2 = 0, b = m_mul2; k2 < block_size; ++k2, b += N)
            for (j2 = 0; j2 < block_size; ++j2)
                r[j2] += a[k2] * b[j2];
}

void kernelMulMatrix_TP_BL_NV(double*       r,
                              const double* a,
                              const double* m_mul2,
                              std::size_t   block_size)
{
    const double* b;

    for (auto i = 0; i < block_size; ++i, r += N, a += N)
    {
        b = m_mul2;
        for (auto j = 0; j < block_size; ++j, b += N)
        {
            double t = 0;
            for (auto k = 0; k < block_size; ++k)
            {
                t += a[k] * b[k];
            }
            r[j] += t;
        }
    }
}

void kernelMulMatrix_VT_BL_TP(double*       r,
                              const double* a,
                              const double* m_mul2,
                              std::size_t   block_size)
{
    // DOUBLE version only
    // block_size == 1 to disable block opt
    // block size affect which func we can use

    const double* b;
    for (auto i = 0; i < block_size; ++i, r += N, a += N)
    {
        b = m_mul2;
        for (auto j = 0; j < block_size; ++j, b += N)
        {
            //_mm_prefetch(&b[N], _MM_HINT_NTA);
            __m256d rk = _mm256_setzero_pd();
            for (auto k = 0; k < block_size; k += 4)
            {
                __m256d m1 = _mm256_loadu_pd(&a[k]);
                __m256d m2 = _mm256_loadu_pd(&b[k]);
                rk         = _mm256_add_pd(rk, _mm256_mul_pd(m2, m1));
            }
            std::array<double, 4> arrK{0, 0, 0, 0};
            _mm256_storeu_pd(arrK.data(), rk);
            r[j] += arrK[0] + arrK[1] + arrK[2] + arrK[3];
        }
    }
}

static void mulMatrix_128VL_BL(double*           rres,
                               const double*     rmul1,
                               const double*     m_mul2,
                               const std::size_t block_size)
{
    const double* rmul2 = m_mul2;
    for (int i2 = 0; i2 < block_size; ++i2, rres += N, rmul1 += N)
    {
        _mm_prefetch(&rmul1[block_size], _MM_HINT_NTA);
        rmul2 = m_mul2;

        __m128d r20 = _mm_load_pd(&rres[0]);
        __m128d r21 = _mm_load_pd(&rres[2]);
        __m128d r22 = _mm_load_pd(&rres[4]);
        __m128d r23 = _mm_load_pd(&rres[6]);

        for (int k2 = 0; k2 < block_size; ++k2, rmul2 += N)
        {
            __m128d m20 = _mm_load_pd(&rmul2[0]);
            __m128d m21 = _mm_load_pd(&rmul2[2]);
            __m128d m22 = _mm_load_pd(&rmul2[4]);
            __m128d m23 = _mm_load_pd(&rmul2[6]);
            __m128d m1d = _mm_load_sd(&rmul1[k2]);
            m1d         = _mm_unpacklo_pd(m1d, m1d);

            r20 = _mm_fmadd_pd(m20, m1d, r20);
            r21 = _mm_fmadd_pd(m21, m1d, r21);
            r22 = _mm_fmadd_pd(m22, m1d, r22);
            r23 = _mm_fmadd_pd(m23, m1d, r23);
        }
        _mm_store_pd(&rres[0], r20);
        _mm_store_pd(&rres[2], r21);
        _mm_store_pd(&rres[4], r22);
        _mm_store_pd(&rres[6], r23);
    }
}

static void mulMatrix_256VL_BL(double*           rres,
                               const double*     rmul1,
                               const double*     m_mul2,
                               const std::size_t block_size)
{

    const double* rmul2 = m_mul2;
    for (int i2 = 0; i2 < block_size; ++i2, rres += N, rmul1 += N)
    {
        // _mm_prefetch(&rmul1[SM], _MM_HINT_NTA);
        // _mm_prefetch(&rres[N], _MM_HINT_NTA);
        // _mm_prefetch(&rmul1[N], _MM_HINT_NTA);

        _mm_prefetch(&rres[block_size], _MM_HINT_T0);
        _mm_prefetch(&rmul1[block_size], _MM_HINT_T0);

        rmul2 = m_mul2;

        __m256d r20 = _mm256_loadu_pd(&rres[0]);
        __m256d r22 = _mm256_loadu_pd(&rres[4]);

        for (int k2 = 0; k2 < block_size; ++k2, rmul2 += N)
        {
            __m256d m20 = _mm256_loadu_pd(&rmul2[0]);
            __m256d m22 = _mm256_loadu_pd(&rmul2[4]);
            __m256d m1d = _mm256_broadcast_sd(&rmul1[k2]);
            r20         = _mm256_add_pd(r20, _mm256_mul_pd(m20, m1d));
            r22         = _mm256_add_pd(r22, _mm256_mul_pd(m22, m1d));
        }
        _mm256_storeu_pd(&rres[0], r20);
        _mm256_storeu_pd(&rres[4], r22);
    }
}

void kernelMulMatrix_VT_BL(double* c, const double* a, const double* b, std::size_t block_size)
{

#if defined(__x86_64__)
#ifdef __AVX2__
    mulMatrix_256VL_BL(c, a, b, block_size);
#elif __SSE2__
    mulMatrix_128VL_BL(c, a, b, block_size);
#else
#error "Manual vectorization is not supported for current cpu"
#endif
#elif defined(__ARM_ARCH) // TODO: Verify macro
    kernelMulMatrix_BL_NV(c, a, b, block_size);
#endif
}

// ************      NEW KERNELS      *********************/
void Kernels::kernelMulMatrix_BL_NV(double* m_res, double* m_mul1, double* m_mul2)
{
    // TODO: How to Help compiler to vectorize as for static c arrays?
    // check opt compiler applied
    const double *a, *b;
    double*       c;
    int           i2, k2, j2;

    for (i2 = 0, c = m_res, a = m_mul1; i2 < _block_size; ++i2, c += _j_size, a += _k_size)
        for (k2 = 0, b = m_mul2; k2 < _block_size; ++k2, b += _j_size)
            for (j2 = 0; j2 < _block_size; ++j2)
                c[j2] += a[k2] * b[j2];
}

void Kernels::kernelMulMatrix_TP_BL_NV(double* r, double* a, double* m_mul2)
{
    const double* b;

    for (auto i = 0; i < _block_size; ++i, r += _j_size, a += _k_size)
    {
        b = m_mul2;
        for (auto j = 0; j < _block_size; ++j, b += _k_size)
        {
            double t = 0;
            for (auto k = 0; k < _block_size; ++k)
            {
                t += a[k] * b[k];
            }
            r[j] += t;
        }
    }
}

void Kernels::kernelMulMatrix_VT_BL_TP(double* r, double* a, double* m_mul2)
{
    // DOUBLE version only
    // block_size == 1 to disable block opt
    // block size affect which func we can use

    const double* b;
    for (auto i = 0; i < _block_size; ++i, r += _j_size, a += _k_size)
    {
        b = m_mul2;
        for (auto j = 0; j < _block_size; ++j, b += _k_size)
        {
            //_mm_prefetch(&b[N], _MM_HINT_NTA);
            __m256d rk = _mm256_setzero_pd();
            for (auto k = 0; k < _block_size; k += 4)
            {
                __m256d m1 = _mm256_loadu_pd(&a[k]);
                __m256d m2 = _mm256_loadu_pd(&b[k]);
                rk         = _mm256_add_pd(rk, _mm256_mul_pd(m2, m1));
            }
            std::array<double, 4> arrK{0, 0, 0, 0};
            _mm256_storeu_pd(arrK.data(), rk);
            r[j] += arrK[0] + arrK[1] + arrK[2] + arrK[3];
        }
    }
}

void Kernels::mulMatrix_128VL_BL(double* rres, const double* a, const double* m_mul2)
{
    const double* b = m_mul2;
    for (int i2 = 0; i2 < _block_size; ++i2, rres += _j_size, a += _k_size)
    {
        _mm_prefetch(&a[_block_size], _MM_HINT_NTA);
        b = m_mul2;

        __m128d r20 = _mm_load_pd(&rres[0]);
        __m128d r21 = _mm_load_pd(&rres[2]);
        __m128d r22 = _mm_load_pd(&rres[4]);
        __m128d r23 = _mm_load_pd(&rres[6]);

        for (int k2 = 0; k2 < _block_size; ++k2, b += _j_size)
        {
            __m128d m20 = _mm_load_pd(&b[0]);
            __m128d m21 = _mm_load_pd(&b[2]);
            __m128d m22 = _mm_load_pd(&b[4]);
            __m128d m23 = _mm_load_pd(&b[6]);
            __m128d m1d = _mm_load_sd(&a[k2]);
            m1d         = _mm_unpacklo_pd(m1d, m1d);

            r20 = _mm_fmadd_pd(m20, m1d, r20);
            r21 = _mm_fmadd_pd(m21, m1d, r21);
            r22 = _mm_fmadd_pd(m22, m1d, r22);
            r23 = _mm_fmadd_pd(m23, m1d, r23);
        }
        _mm_store_pd(&rres[0], r20);
        _mm_store_pd(&rres[2], r21);
        _mm_store_pd(&rres[4], r22);
        _mm_store_pd(&rres[6], r23);
    }
}

void Kernels::mulMatrix_256VL_BL(double* rres, const double* rmul1, const double* m_mul2)
{
    //    assert(N % block_size == 0);
    //    assert(block_size % 4 == 0);
    assert(block_size % SM == 0);
    const double* rmul2 = m_mul2;
    for (int i2 = 0; i2 < _block_size; ++i2, rres += _j_size, rmul1 += _k_size)
    {
        // _mm_prefetch(&rmul1[SM], _MM_HINT_NTA);
        // _mm_prefetch(&rres[N], _MM_HINT_NTA);
        // _mm_prefetch(&rmul1[N], _MM_HINT_NTA);

        _mm_prefetch(&rres[_block_size], _MM_HINT_T0);
        _mm_prefetch(&rmul1[_block_size], _MM_HINT_T0);

        rmul2 = m_mul2;

        __m256d r20 = _mm256_loadu_pd(&rres[0]);
        __m256d r22 = _mm256_loadu_pd(&rres[4]);

        for (int k2 = 0; k2 < _block_size; ++k2, rmul2 += _j_size)
        {
            __m256d m20 = _mm256_loadu_pd(&rmul2[0]);
            __m256d m22 = _mm256_loadu_pd(&rmul2[4]);
            __m256d m1d = _mm256_broadcast_sd(&rmul1[k2]);
            r20         = _mm256_add_pd(r20, _mm256_mul_pd(m20, m1d));
            r22         = _mm256_add_pd(r22, _mm256_mul_pd(m22, m1d));
        }
        _mm256_storeu_pd(&rres[0], r20);
        _mm256_storeu_pd(&rres[4], r22);
    }
}

void Kernels::kernelMulMatrix_VT_BL(double* c, double* a, double* b)
{

#if defined(__x86_64__)
#ifdef __AVX2__
    mulMatrix_256VL_BL(c, a, b);
#elif __SSE2__
    mulMatrix_128VL_BL(c, a, b);
#else
#error "Manual vectorization is not supported for current cpu"
#endif
#elif defined(__ARM_ARCH)
    kernelMulMatrix_BL_NV(c, a, b);
#endif
}
