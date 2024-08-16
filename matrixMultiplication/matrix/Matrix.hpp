#pragma once

#include <vector>
#include <boost/align/aligned_allocator.hpp>

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
using std::hardware_destructive_interference_size;
#else
// 64 bytes on x86-64 │ L1_CACHE_BYTES │ L1_CACHE_SHIFT │ __cacheline_aligned │ ...
constexpr std::size_t hardware_constructive_interference_size = 64;
constexpr std::size_t hardware_destructive_interference_size  = 64;
#endif

constexpr std::size_t N = 256;

template<typename T>
using aligned_vector =
  std::vector<T, boost::alignment::aligned_allocator<T, hardware_destructive_interference_size>>;

template<typename Fun>
constexpr aligned_vector<double> initAlignedVector(const std::size_t row_size,
                                                   const std::size_t col_size,
                                                   Fun&&             fun)
{
    aligned_vector<double> matrix;
    matrix.resize(row_size * col_size);
    for (auto col = 0; col < col_size; ++col)
    {
        for (auto row = 0; row < row_size; ++row)
        {
            matrix[col * col_size + row] = fun(row, col);
        }
    }
    return matrix;
}

template<typename T>
class Matrix
{
  public:
    using value_type = T;

    Matrix()
      : _row_cnt(N)
      , _col_cnt(N)
      , _matrix(_row_cnt * _col_cnt, 0)
    {
    }

    Matrix(std::size_t row_cnt, std::size_t col_cnt)
      : _row_cnt(row_cnt)
      , _col_cnt(col_cnt)
      , _matrix(_row_cnt * _col_cnt, 0)
    {
    }

    [[__nodiscard__]] double operator[](std::size_t idx) const noexcept
    {
        return _matrix[idx];
    }

    [[__nodiscard__]] double& operator[](std::size_t idx) noexcept
    {
        return _matrix[idx];
    }

    [[__nodiscard__]] constexpr double* data() noexcept
    {
        return _matrix.data();
    }

    [[__nodiscard__]] constexpr const double* data() const noexcept
    {
        return _matrix.data();
    }

    [[__nodiscard__]] std::size_t size() const noexcept
    {
        return _col_cnt * _row_cnt;
    }

    [[__nodiscard__]] std::size_t col() const noexcept
    {
        return _col_cnt;
    }

    [[__nodiscard__]] std::size_t row() const noexcept
    {
        return _row_cnt;
    }

  private:
    std::size_t _row_cnt;
    std::size_t _col_cnt;

    aligned_vector<double> _matrix;
};

template<typename T>
Matrix<T> transpose(const Matrix<T> m)
{
    auto col_cnt = m.col();
    auto row_cnt = m.row();

    Matrix<T> transposed(col_cnt, row_cnt);

    for (auto i = 0; i < row_cnt; ++i)
    {
        for (auto j = 0; j < col_cnt; ++j)
        {
            // TODO: vectorize transpose
            //_mm_prefetch(&ms.b[(j + 1) * N], _MM_HINT_NTA);
            // we don't need b in cache, transposed shuold be in cache,
            // reading from b can be streamed
            transposed[i * col_cnt + j] = m[j * row_cnt + i];
        }
    }
    return transposed;
}

template<typename Stream, typename T>
Stream& operator<<(Stream& os, Matrix<T>& m)
{
    for (auto i = 0; i < m.row(); ++i)
    {
        for (auto j = 0; j < m.col(); ++j)
        {
            os << m[i * m.col() + j] << ", ";
        }
        os << "\n";
    }
    return os;
}

template<typename T>
bool operator==(const Matrix<T>& s1, const Matrix<T>& s2)
{
    auto row_cnt = s1.row();
    auto col_cnt = s1.col();

    if (col_cnt != s2.col())
        return false;

    if (row_cnt != s2.row())
        return false;

    auto N = row_cnt * col_cnt;
    for (auto idx = 0; idx < N; ++idx)
    {
        if (std::abs(s1[idx] - s2[idx]) > __DBL_EPSILON__)
        {
            return false;
        }
    }
    return true;
}

struct MatrixSet
{
    using value_type = double;
    Matrix<value_type> a;
    Matrix<value_type> b;
    Matrix<value_type> res;
};

MatrixSet initMatrix();

template<typename Stream>
Stream& operator<<(Stream& os, MatrixSet& s1)
{
    os << s1.res;
    return os;
}

bool operator==(const MatrixSet&, const MatrixSet&);
