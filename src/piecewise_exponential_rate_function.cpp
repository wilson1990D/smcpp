#include "piecewise_exponential_rate_function.h"

const double T_MAX = 15.0;

template <typename T>
PiecewiseExponentialRateFunction<T>::PiecewiseExponentialRateFunction(const std::vector<std::vector<double>> params,
        const std::vector<double> hidden_states) : 
    PiecewiseExponentialRateFunction(params, std::vector<std::pair<int, int>>(), hidden_states) 
{
}

std::vector<std::pair<int, int>> derivatives_from_params(const std::vector<std::vector<double>> params)
{
    std::vector<std::pair<int, int>> ret;
    for (size_t i = 0; i < params.size(); ++i)
        for (size_t j = 0; j < params[0].size(); ++j)
            ret.emplace_back(i, j);
    return ret;
} 

template <>
PiecewiseExponentialRateFunction<adouble>::PiecewiseExponentialRateFunction(
        const std::vector<std::vector<double>> params, 
        const std::vector<double> hidden_states) :
    PiecewiseExponentialRateFunction(params, derivatives_from_params(params), hidden_states) {}

template <>
adouble PiecewiseExponentialRateFunction<adouble>::init_derivative(double x)
{
    return adouble(x, Vector<double>::Zero(derivatives.size()));
}

template <>
double PiecewiseExponentialRateFunction<double>::init_derivative(double x)
{
    return x;
}

template <typename T>
inline void vec_insert(std::vector<T> &v, const int pos, const T &x)
{
    v.insert(v.begin() + pos, x);
}

template <typename T>
PiecewiseExponentialRateFunction<T>::PiecewiseExponentialRateFunction(
        const std::vector<std::vector<double>> params, 
        const std::vector<std::pair<int, int>> derivatives,
        const std::vector<double> hidden_states) :
    params(params),
    derivatives(derivatives), K(params[0].size()), ada(params[0].begin(), params[0].end()), 
    adb(params[1].begin(), params[1].end()), ads(params[2].begin(), params[2].end()),
    ts(K + 1), Rrng(K), _reg(0.0), 
    zero(init_derivative(0.0)), one(init_derivative(1.0)),
    hidden_states(hidden_states)
{
    for (auto &pp : params)
        if (pp.size() != params[0].size())
            throw std::runtime_error("all params must have same size");
    // Final piece is required to be flat.
    T adatmp;
    ts[0] = zero;
    Rrng[0] = zero;
    // These constant values need to have compatible derivative shape
    // with the calculated values.
    initialize_derivatives();
    for (int k = 0; k < K; ++k)
    {
        ada[k] = 1. / ada[k];
        adb[k] = 1. / adb[k];
        ts[k + 1] = ts[k] + ads[k];
        adb[k] = (log(adb[k]) - log(ada[k])) / (ts[k + 1] - ts[k]);
    }
    // ts[K] = INFINITY;
    adb[K - 1] = zero;
    ts[K] = T_MAX;

    int ip;
    for (double h : hidden_states)
    {
        ip = insertion_point(h, ts, 0, ts.size());
        if (ts[ip] == h)
            hs_indices.push_back(ip);
        else
        {
            vec_insert<T>(ts, ip + 1, (T)h);
            if (adb[ip] == 0)
            {
                vec_insert<T>(ada, ip + 1, ada[ip]);
                vec_insert<T>(adb, ip + 1, adb[ip]);
            }
            else
            {
                vec_insert<T>(ada, ip + 1, ada[ip] * exp(adb[ip] * (one * h - ts[ip])));
                vec_insert<T>(adb, ip + 1, (log(ada[ip] / ada[ip + 1]) + adb[ip] * (ts[ip + 2] - ts[ip])) / (ts[ip + 2] - (T)h));
            }
            check_nan(ada[ip + 1]);
            check_nan(adb[ip + 1]);
            check_nan(ts[ip + 1]);
            hs_indices.push_back(ip + 1);
        }
    }
    K = ada.size();
    for (int k = 0; k < K; ++k)
        if (myabs(adb[k]) < 1e-2)
            adb[k] = zero;
    Rrng.resize(K + 1);
    compute_antiderivative();

    _eta.reset(new PExpEvaluator<T>(ada, adb, ts, Rrng));
    _R.reset(new PExpIntegralEvaluator<T>(ada, adb, ts, Rrng));
    _Rinv.reset(new PExpInverseIntegralEvaluator<T>(ada, adb, ts, Rrng));
    T elast, xx, etax, tmp;
    elast = 1. / eta(ts[0]);
    const int delta = 50;
    for (int k = 0; k < K - 1; ++k)
    {
        for (int i = 1; i < delta + 1; ++i)
        {
            xx = (i / delta) * (ts[k + 1] - ts[k]) + ts[k];
            etax = 1. / eta(xx);
            tmp = etax - elast;
            _reg += myabs(tmp);
            elast = etax;
        }
    }
}

template <typename T>
void PiecewiseExponentialRateFunction<T>::initialize_derivatives(void) {}

static int nd;

template <>
void PiecewiseExponentialRateFunction<adouble>::initialize_derivatives(void)
{
    nd = derivatives.size();
    Eigen::VectorXd z = Eigen::VectorXd::Zero(nd);
    Eigen::MatrixXd I = Eigen::MatrixXd::Identity(nd, nd);
    for (int k = 0; k < K; ++k)
    {
        ada[k].derivatives() = z;
        adb[k].derivatives() = z;
        ads[k].derivatives() = z;
    }
    std::vector<adouble>* dl[3] = {&ada, &adb, &ads};
    int d = 0;
    for (auto p : derivatives)
        (*dl[p.first])[p.second].derivatives() = I.col(d++);
    ts[0].derivatives() = z;
    Rrng[0].derivatives() = z;
}

template <typename T>
inline T _double_integral_below_helper(const int rate, const T &tsm, const T &tsm1, const T &ada, const T &Rrng)
{
    const int l1r = 1 + rate;
    T _tsm = tsm, _tsm1 = tsm1, _ada = ada, _Rrng = Rrng; // don't ask
    T z = _tsm - _tsm;
    const T l1rinv = 1 / (z + l1r);
    T diff = _tsm1 - _tsm;
    T _adadiff = _ada * diff;
    if (rate == 0)
    {
        T e1 = exp(-_adadiff);
        if (tsm1 == INFINITY)
            return exp(-_Rrng) / _ada;
        else
            return exp(-_Rrng) * (1 - exp(-_adadiff) * (1 + _adadiff)) / _ada;
    }
    if (tsm1 == INFINITY)
        return exp(-l1r * _Rrng) * (1 - l1rinv) / (rate * _ada);
    return exp(-l1r * _Rrng) * (expm1(-l1r * _adadiff) * l1rinv - expm1(-_adadiff)) / (rate * _ada);
}

template <typename U>
inline U _double_integral_above_helper(const int rate, const int lam, const U &_tsm, const U &_tsm1, const U &_ada, const U &_Rrng)
{
    U diff = _tsm1 - _tsm;
    U adadiff = _ada * diff;
    long l1 = lam + 1;
    if (rate == 0)
        return exp(-l1 * _Rrng) * (expm1(-l1 * adadiff) + l1 * adadiff) / l1 / l1 / _ada;
    if (l1 == rate)
    {
        if (_tsm1 == INFINITY)
            return exp(-rate * _Rrng) / rate / rate / _ada;
        return exp(-rate * _Rrng) * (1 - exp(-rate * adadiff) * (1 + rate * adadiff)) / rate / rate / _ada;
    }
    if (_tsm1 == INFINITY)
        return exp(-l1 * _Rrng) / l1 / rate / _ada;
    return -exp(-l1 * _Rrng) * (expm1(-l1 * adadiff) / l1 + (exp(-rate * adadiff) - exp(-l1 * adadiff)) / (l1 - rate)) / rate / _ada;
}

template <typename T>
void PiecewiseExponentialRateFunction<T>::print_debug() const
{
    std::vector<std::pair<std::string, std::vector<T>>> arys = 
    {{"ada", ada}, {"adb", adb}, {"ads", ads}, {"ts", ts}, {"Rrng", Rrng}};
    std::cout << std::endl;
    for (auto p : arys)
    {
        std::cout << p.first << std::endl;
        for (adouble x : p.second)
            std::cout << x.value() << "::" << x.derivatives().transpose() << std::endl;
        std::cout << std::endl;
    }
    std::cout << "reg: " << toDouble(_reg) << std::endl << std::endl;
}

template <typename T>
void PiecewiseExponentialRateFunction<T>::compute_antiderivative()
{
    Rrng[0] = zero;
    for (int k = 0; k < K; ++k)
    {
        if (adb[k] == 0.0)
            Rrng[k + 1] = Rrng[k] + ada[k] * (ts[k + 1] - ts[k]);
        else
            Rrng[k + 1] = Rrng[k] + (ada[k] / adb[k]) * expm1(adb[k] * (ts[k + 1] - ts[k]));
    }
}

template <typename T>
T PiecewiseExponentialRateFunction<T>::R_integral(const T x, const T y, const int m) const
{
    // int_0^x exp(m * R(t) + y) dt
    if (x == 0)
        return zero;
    if (x < 1e-6)
    {
        return x * exp(y);
    }
    int ip = insertion_point(x, ts, 0, ts.size());
    T ret = zero, tmp, r;
    if (x == 0)
        return zero;
    for (int i = 0; i < ip + 1; ++i)
    {
        tmp = dmin(x, ts[i + 1]) - ts[i];
        if (adb[i] == 0)
        {
            r = exp(m * Rrng[i] + y) * expm1(m * tmp * ada[i]);
            r /= m * ada[i];
        }
        else
        {
            T adab = ada[i] / adb[i];
            T c1 = m * adab * exp(adb[i] * tmp);
            T c2 = m * adab;
            T c3 = m * (Rrng[i] - adab) + y;
            /*
            T r1 = expintei(c1);
            T r2 = expintei(c2);
            r = r1 - r2;
            r *= exp(2 * (Rrng[i] - adab) + y) / adb[i];
            */
            r = eintdiff(c2, c1, c3) / adb[i];
            check_negative(r);
            check_nan(r);
        }
        if (r > 100.)
            throw std::domain_error("what");
        check_negative(r);
        check_nan(r);
        ret += r;
    }
    return ret;
}


template <typename T>
inline T _single_integral(const int rate, const T &tsm, const T &tsm1, const T &ada, const T &adb, const T &Rrng, const T &log_coef)
{
    // = int_ts[m]^ts[m+1] exp(-rate * R(t)) dt
    const int c = rate;
    if (rate == 0)
        return exp(log_coef) * (tsm1 - tsm);
    if (adb == 0.)
    {
        T ret = exp(-c * Rrng + log_coef);
        if (tsm1 < INFINITY)
            ret *= -expm1(-c * ada * (tsm1 - tsm));
        return ret / ada / c;
    }
    T e1 = -c * ada / adb;
    T e2 = -c * exp(adb * (tsm1 - tsm)) * ada / adb;
    T e3 =  c * (ada / adb - Rrng) + log_coef;
    T ret = eintdiff(e1, e2, e3) / adb;
    check_nan(ret);
    check_negative(ret);
    return ret;
}

template <typename T>
inline T _double_integral_below_helper_ei(const int rate, const T &tsm, const T &tsm1, 
        const T &ada, const T &adb, const T &Rrng)
{
    // We needn't cover the tsm1==INFINITY case here as the last piece is assumed flat (i.e., adb=0).
    long c = rate;
    T eadb = exp(adb * (tsm1 - tsm));
    T adadb = ada / adb;
    if (c == 0)
    {
        T a1 = -adadb;
        T b1 = -eadb * adadb;
        T cons1 = adadb - Rrng;
        T int1 = eintdiff(a1, b1, cons1);
        int1 /= adb;
        int1 += exp(adadb * (1. - eadb) - Rrng) * (tsm - tsm1);
        check_negative(int1);
        check_nan(int1);
        return int1;
    }
    T cons1 = (2 + c) * adadb;
    T cons2 = adadb * (2 + c + eadb);
    T a1 = -c * adadb * eadb;
    T b1 = -c * adadb;
    T int1 = eintdiff(a1, b1, cons1);
    T a2 = -(c + 1) * adadb;
    T b2 = -(c + 1) * adadb * eadb;
    T int2 = eintdiff(a2, b2, cons2);
    T cons3 = exp(-(ada * (1 + eadb) / adb + (1 + c) * Rrng));
    T ret = cons3 * (int1 + int2) / adb;
    check_negative(ret);
    check_nan(ret);
    return ret;
}

template <typename T>
inline T _double_integral_above_helper_ei(const int rate, const int lam, const T &tsm, const T &tsm1, 
        const T &ada, const T &adb, const T &Rrng)
{
    long d = lam + 1;
    long c = rate;
    T eadb = exp(adb * (tsm1 - tsm));
    T cons1 = ada * c / adb;
    T a1 = -cons1 * eadb;
    T b1 = -cons1;
    T c1 = cons1 - d * Rrng;
    T ed1 = eintdiff(a1, b1, c1);
    if (c != d)
    {
        T cons2 = ada * d / adb;
        T a2 = -cons2;
        T b2 = -cons2 * eadb;
        T c2 = cons2 - d * Rrng;
        return (ed1 + eintdiff(a2, b2, c2)) / adb / (c - d);
    }
    T ret = (exp(-d * Rrng) * (-adb * expm1(-ada / adb * d * expm1(adb * (tsm1 - tsm)))) + ada * d * ed1) / (adb * adb * d);
    check_negative(ret);
    check_nan(ret);
    return ret;
}

template <typename T>
void PiecewiseExponentialRateFunction<T>::tjj_double_integral_above(const int n, long jj, std::vector<Matrix<T> > &C) const
{
    long lam = nC2(jj) - 1;
    Matrix<T> ts_integrals(K, n);
    ts_integrals.fill(zero);
    std::vector<T> single_integrals;
    T e1, e2;
    for (int m = 0; m < K; ++m)
    {
        e1 = exp(-Rrng[m]);
        if (m < K - 1)
            e1 -= exp(-Rrng[m + 1]);
        single_integrals.push_back(e1);
    }

    for (int m = 0; m < K; ++m)
    {
        for (int j = 2; j < n + 2; ++j)
        {
            long rate = nC2(j);
            if (adb[m] == 0)
                ts_integrals(m, j - 2) = _double_integral_above_helper<T>(rate, lam, ts[m], ts[m + 1], ada[m], Rrng[m]);
            else
                ts_integrals(m, j - 2) = _double_integral_above_helper_ei<T>(rate, lam, ts[m], ts[m + 1], ada[m], adb[m], Rrng[m]);
            check_nan(ts_integrals(m, j - 2));
            T tmp = zero, log_coef = zero, fac;
            long rp = lam + 1 - rate;
            if (rp == 0)
                fac = Rrng[m + 1] - Rrng[m];
            else
            {
                // * exp(-rp * Rrng[m]) - exp(-rp * Rrng[m+1]) / rp
                // if rp >> 1 * Rrng[m] then this is approx exp(-rp * Rrng[m])
                // if rp << -1 then approx = -exp(-rp * Rrng[m+1])
                if (rp < 0)
                {
                    // exp(-rp * Rrng[m]) - exp(-rp * Rrng[m+1]) / rp
                    // = exp(-rp * Rrng[m]) * (1 - exp(-rp * (Rrng[m + 1] - Rrng[m]))) / rp
                    if (-rp * (Rrng[m + 1] - Rrng[m]) > 20)
                    {
                        log_coef = -rp * Rrng[m + 1];
                        fac = -one / rp;
                    }
                    else
                    {
                        log_coef = -rp * Rrng[m];
                        fac = -expm1(-rp * (Rrng[m + 1] - Rrng[m])) / rp;
                    }
                }
                else
                {
                    // exp(-rp * Rrng[m]) - exp(-rp * Rrng[m+1]) / rp
                    // = exp(-rp * Rrng[m + 1]) * (exp(-rp * (Rrng[m] - Rrng[m + 1]) - 1)) / rp
                    if (-rp * (Rrng[m] - Rrng[m + 1]) > 20)
                    {
                        log_coef = -rp * Rrng[m];
                        fac = one / rp;
                    }
                    else
                    {
                        log_coef = -rp * Rrng[m + 1];
                        fac = expm1(-rp * (Rrng[m] - Rrng[m + 1])) / rp;
                    }
                }
            }
            for (int k = m + 1; k < K; ++k)
            {
                ts_integrals(m, j - 2) += _single_integral(rate, ts[k], ts[k + 1], ada[k], adb[k], Rrng[k], log_coef) * fac;
                check_nan(ts_integrals(m, j - 2));
            }
            /*
                tmp += _single_integral(rate, ts[k], ts[k + 1], ada[k], adb[k], Rrng[k], zero);
            if (m + 1 < K)
            {
                long rp = lam + 1 - rate;
                if (rp == 0)
                    ts_integrals(m, j - 2) += (Rrng[m + 1] - Rrng[m]) * tmp; 
                else
                    ts_integrals(m, j - 2) += (exp(-rp * Rrng[m]) - exp(-rp * Rrng[m + 1])) * tmp / rp;
            }
            */
            check_nan(ts_integrals(m, j - 2));
            check_negative(ts_integrals(m, j - 2));
        }
    }
    // Now calculate with hidden state integration limits
    size_t H = hidden_states.size();
    Matrix<T> last = ts_integrals.topRows(hs_indices[0]).colwise().sum(), next;
    last *= one;
    for (int h = 1; h < hs_indices.size(); ++h)
    {
        next = ts_integrals.topRows(hs_indices[h]).colwise().sum();
        C[h - 1].row(jj - 2) = next - last;
        last = next;
    }
}

template <typename T>
void PiecewiseExponentialRateFunction<T>::tjj_double_integral_below(
        const int n, const int m, Matrix<T> &tgt) const
{
    Vector<T> ts_integrals(n + 1);
    T log_coef = -Rrng[m];
    T fac = one;
    if (m < K - 1)
        fac = -expm1(-(Rrng[m + 1] - Rrng[m]));
    for (int j = 2; j < n + 3; ++j)
    {
        long rate = nC2(j) - 1;
        if (adb[m] == 0.)
            ts_integrals(j - 2) = _double_integral_below_helper<T>(rate, ts[m], ts[m + 1], ada[m], Rrng[m]);
        else
            ts_integrals(j - 2) = _double_integral_below_helper_ei(rate, ts[m], ts[m + 1], ada[m], adb[m], Rrng[m]);
        for (int k = 0; k < m; ++k)
            ts_integrals(j - 2) += fac * _single_integral(rate, ts[k], ts[k + 1], ada[k], adb[k], Rrng[k], log_coef);
        check_negative(ts_integrals(j - 2));
    }
    tgt.row(m) = ts_integrals.transpose();
}

template class PiecewiseExponentialRateFunction<double>;
template class PiecewiseExponentialRateFunction<adouble>;
