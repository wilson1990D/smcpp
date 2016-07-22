#ifndef TRANSITION_H
#define TRANSITION_H

#include <random>
#include <map>

#include "common.h"
#include "piecewise_constant_rate_function.h"

template <typename T>
class Transition
{
    public:
    Transition(const PiecewiseConstantRateFunction<T> &eta, const double rho) : 
        eta(eta), M(eta.hidden_states.size()), Phi(M - 1, M - 1), rho(rho) {}
    Matrix<T>& matrix(void) { return Phi; }

    protected:
    // Variables
    const PiecewiseConstantRateFunction<T> eta;
    const int M;
    Matrix<T> Phi;
    const double rho;
};

template <typename T>
class HJTransition : public Transition<T>
{
    public:
    HJTransition(const PiecewiseConstantRateFunction<T> &eta, const double rho);
};

template <typename T>
Matrix<T> compute_transition(const PiecewiseConstantRateFunction<T> &, const double);

#endif
