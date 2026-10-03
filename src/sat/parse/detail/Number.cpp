#include "sat/parse/detail/Number.h"

#include <cmath>

namespace sat {
namespace detail {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isSpace(char c) { return c == ' '; }

// 10^n; exact for n <= 22, correctly rounded division/multiplication by it keeps the result exact
// for mantissas below 2^53.
double pow10(int n) {
    static const double table[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                   1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    return n >= 0 && n <= 22 ? table[n] : std::pow(10.0, n);
}

double scaleBy(double mantissa, int exponent10) {
    return exponent10 < 0 ? mantissa / pow10(-exponent10) : mantissa * pow10(exponent10);
}

} // namespace

bool parseDecimal(const char* text, std::size_t length, double& out) {
    std::size_t i = 0;
    while (i < length && isSpace(text[i])) {
        ++i;
    }
    bool negative = false;
    if (i < length && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    unsigned long long mantissa = 0;
    int digits = 0;
    int decimals = 0;
    bool any = false;
    while (i < length && isDigit(text[i])) {
        if (++digits > 18) {
            return false;
        }
        mantissa = mantissa * 10 + static_cast<unsigned>(text[i] - '0');
        any = true;
        ++i;
    }
    if (i < length && text[i] == '.') {
        ++i;
        while (i < length && isDigit(text[i])) {
            if (++digits > 18) {
                return false;
            }
            mantissa = mantissa * 10 + static_cast<unsigned>(text[i] - '0');
            ++decimals;
            any = true;
            ++i;
        }
    }
    if (!any) {
        return false;
    }
    int exponent = 0;
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool expNegative = false;
        if (i < length && (text[i] == '+' || text[i] == '-')) {
            expNegative = text[i] == '-';
            ++i;
        }
        int expDigits = 0;
        while (i < length && isDigit(text[i])) {
            if (++expDigits > 3) {
                return false;
            }
            exponent = exponent * 10 + (text[i] - '0');
            ++i;
        }
        if (expDigits == 0) {
            return false;
        }
        exponent = expNegative ? -exponent : exponent;
    }
    while (i < length && isSpace(text[i])) {
        ++i;
    }
    if (i != length) {
        return false;
    }
    const double value = scaleBy(static_cast<double>(mantissa), exponent - decimals);
    out = negative ? -value : value;
    return true;
}

bool parseImplied(const char* text, std::size_t length, double& out) {
    std::size_t i = 0;
    while (i < length && isSpace(text[i])) {
        ++i;
    }
    while (length > i && isSpace(text[length - 1])) {
        --length;
    }
    bool negative = false;
    if (i < length && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    unsigned long long mantissa = 0;
    int digits = 0;
    while (i < length && isDigit(text[i])) {
        if (++digits > 8) {
            return false;
        }
        mantissa = mantissa * 10 + static_cast<unsigned>(text[i] - '0');
        ++i;
    }
    if (digits == 0 || i + 2 != length || (text[i] != '+' && text[i] != '-') || !isDigit(text[i + 1])) {
        return false;
    }
    const int exponent = (text[i] == '-' ? -1 : 1) * (text[i + 1] - '0');
    const double value = scaleBy(static_cast<double>(mantissa), exponent - digits);
    out = negative ? -value : value;
    return true;
}

bool parseUint(const char* text, std::size_t length, unsigned long& out) {
    std::size_t i = 0;
    while (i < length && isSpace(text[i])) {
        ++i;
    }
    unsigned long value = 0;
    int digits = 0;
    while (i < length && isDigit(text[i])) {
        if (++digits > 9) {
            return false;
        }
        value = value * 10 + static_cast<unsigned long>(text[i] - '0');
        ++i;
    }
    while (i < length && isSpace(text[i])) {
        ++i;
    }
    if (digits == 0 || i != length) {
        return false;
    }
    out = value;
    return true;
}

} // namespace detail
} // namespace sat
