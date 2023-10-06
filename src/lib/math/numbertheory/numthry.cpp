/*
* Number Theory Functions
* (C) 1999-2011,2016,2018,2019 Jack Lloyd
* (C) 2007,2008 Falko Strenzke, FlexSecure GmbH
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include <botan/numthry.h>

#include <botan/reducer.h>
#include <botan/rng.h>
#include <botan/internal/ct_utils.h>
#include <botan/internal/divide.h>
#include <botan/internal/monty.h>
#include <botan/internal/monty_exp.h>
#include <botan/internal/mp_core.h>
#include <botan/internal/primality.h>
#include <algorithm>

namespace Botan {

namespace {

void sub_abs(BigInt& z, const BigInt& x, const BigInt& y) {
   const size_t x_sw = x.sig_words();
   const size_t y_sw = y.sig_words();
   z.resize(std::max(x_sw, y_sw));

   bigint_sub_abs(z.mutable_data(), x.data(), x_sw, y.data(), y_sw);
}

}  // namespace

/*
* Tonelli-Shanks algorithm
*/
BigInt sqrt_modulo_prime(const BigInt& a, const BigInt& p) {
   BOTAN_ARG_CHECK(p > 1, "invalid prime");
   BOTAN_ARG_CHECK(a < p, "value to solve for must be less than p");
   BOTAN_ARG_CHECK(a >= 0, "value to solve for must not be negative");

   // some very easy cases
   if(p == 2 || a <= 1) {
      return a;
   }

   BOTAN_ARG_CHECK(p.is_odd(), "invalid prime");

   if(jacobi(a, p) != 1) {  // not a quadratic residue
      return BigInt::from_s32(-1);
   }

   Modular_Reducer mod_p(p);
   auto monty_p = std::make_shared<Montgomery_Params>(p, mod_p);

   // If p == 3 (mod 4) there is a simple solution
   if(p % 4 == 3) {
      return monty_exp_vartime(monty_p, a, ((p + 1) >> 2));
   }

   // Otherwise we have to use Shanks-Tonelli
   size_t s = low_zero_bits(p - 1);
   BigInt q = p >> s;

   q -= 1;
   q >>= 1;

   BigInt r = monty_exp_vartime(monty_p, a, q);
   BigInt n = mod_p.multiply(a, mod_p.square(r));
   r = mod_p.multiply(r, a);

   if(n == 1) {
      return r;
   }

   // find random quadratic nonresidue z
   word z = 2;
   for(;;) {
      if(jacobi(BigInt::from_word(z), p) == -1) {  // found one
         break;
      }

      z += 1;  // try next z

      /*
      * The expected number of tests to find a non-residue modulo a
      * prime is 2. If we have not found one after 256 then almost
      * certainly we have been given a non-prime p.
      */
      if(z >= 256) {
         return BigInt::from_s32(-1);
      }
   }

   BigInt c = monty_exp_vartime(monty_p, BigInt::from_word(z), (q << 1) + 1);

   while(n > 1) {
      q = n;

      size_t i = 0;
      while(q != 1) {
         q = mod_p.square(q);
         ++i;

         if(i >= s) {
            return BigInt::from_s32(-1);
         }
      }

      BOTAN_ASSERT_NOMSG(s >= (i + 1));  // No underflow!
      c = monty_exp_vartime(monty_p, c, BigInt::power_of_2(s - i - 1));
      r = mod_p.multiply(r, c);
      c = mod_p.square(c);
      n = mod_p.multiply(n, c);

      // s decreases as the algorithm proceeds
      BOTAN_ASSERT_NOMSG(s >= i);
      s = i;
   }

   return r;
}

/*
* Calculate the Jacobi symbol
*/
int32_t jacobi(const BigInt& a, const BigInt& n) {
   if(n.is_even() || n < 2) {
      throw Invalid_Argument("jacobi: second argument must be odd and > 1");
   }

   BigInt x = a % n;
   BigInt y = n;
   int32_t J = 1;

   while(y > 1) {
      x %= y;
      if(x > y / 2) {
         x = y - x;
         if(y % 4 == 3) {
            J = -J;
         }
      }
      if(x.is_zero()) {
         return 0;
      }

      size_t shifts = low_zero_bits(x);
      x >>= shifts;
      if(shifts % 2) {
         word y_mod_8 = y % 8;
         if(y_mod_8 == 3 || y_mod_8 == 5) {
            J = -J;
         }
      }

      if(x % 4 == 3 && y % 4 == 3) {
         J = -J;
      }
      std::swap(x, y);
   }
   return J;
}

/*
* Square a BigInt
*/
BigInt square(const BigInt& x) {
   BigInt z = x;
   secure_vector<word> ws;
   z.square(ws);
   return z;
}

/*
* Return the number of 0 bits at the end of n
*/
size_t low_zero_bits(const BigInt& n) {
   size_t low_zero = 0;

   auto seen_nonempty_word = CT::Mask<word>::cleared();

   for(size_t i = 0; i != n.size(); ++i) {
      const word x = n.word_at(i);

      // ctz(0) will return sizeof(word)
      const size_t tz_x = ctz(x);

      // if x > 0 we want to count tz_x in total but not any
      // further words, so set the mask after the addition
      low_zero += seen_nonempty_word.if_not_set_return(tz_x);

      seen_nonempty_word |= CT::Mask<word>::expand(x);
   }

   // if we saw no words with x > 0 then n == 0 and the value we have
   // computed is meaningless. Instead return BigInt::zero() in that case.
   return static_cast<size_t>(seen_nonempty_word.if_set_return(low_zero));
}

namespace {

size_t safegcd_loop_bound(size_t f_bits, size_t g_bits) {
   const size_t d = std::max(f_bits, g_bits);
   return 4 + 3 * d;
}

}  // namespace

/*
* Calculate the GCD
*/
BigInt gcd(const BigInt& a, const BigInt& b) {
   if(a.is_zero()) {
      return abs(b);
   }
   if(b.is_zero()) {
      return abs(a);
   }
   if(a == 1 || b == 1) {
      return BigInt::one();
   }

   // See https://gcd.cr.yp.to/safegcd-20190413.pdf fig 1.2

   BigInt f = a;
   BigInt g = b;
   f.const_time_poison();
   g.const_time_poison();

   f.set_sign(BigInt::Positive);
   g.set_sign(BigInt::Positive);

   const size_t common2s = std::min(low_zero_bits(f), low_zero_bits(g));
   CT::unpoison(common2s);

   f >>= common2s;
   g >>= common2s;

   f.ct_cond_swap(f.is_even(), g);

   int32_t delta = 1;

   const size_t loop_cnt = safegcd_loop_bound(f.bits(), g.bits());

   BigInt newg, t;
   for(size_t i = 0; i != loop_cnt; ++i) {
      sub_abs(newg, f, g);

      const bool need_swap = (g.is_odd() && delta > 0);

      // if(need_swap) { delta *= -1 } else { delta *= 1 }
      delta *= CT::Mask<uint8_t>::expand(need_swap).if_not_set_return(2) - 1;
      f.ct_cond_swap(need_swap, g);
      g.ct_cond_swap(need_swap, newg);

      delta += 1;

      g.ct_cond_add(g.is_odd(), f);
      g >>= 1;
   }

   f <<= common2s;

   f.const_time_unpoison();
   g.const_time_unpoison();

   BOTAN_ASSERT_NOMSG(g.is_zero());

   return f;
}

/*
* Calculate the LCM
*/
BigInt lcm(const BigInt& a, const BigInt& b) {
   if(a == b) {
      return a;
   }

   auto ab = a * b;
   ab.set_sign(BigInt::Positive);  // ignore the signs of a & b
   const auto g = gcd(a, b);
   return ct_divide(ab, g);
}

/*
* Modular Exponentiation
*/
BigInt power_mod(const BigInt& base, const BigInt& exp, const BigInt& mod) {
   if(mod.is_negative() || mod == 1) {
      return BigInt::zero();
   }

   if(base.is_zero() || mod.is_zero()) {
      if(exp.is_zero()) {
         return BigInt::one();
      }
      return BigInt::zero();
   }

   Modular_Reducer reduce_mod(mod);

   const size_t exp_bits = exp.bits();

   if(mod.is_odd()) {
      auto monty_params = std::make_shared<Montgomery_Params>(mod, reduce_mod);
      return monty_exp(monty_params, reduce_mod.reduce(base), exp, exp_bits);
   }

   /*
   Support for even modulus is just a convenience and not considered
   cryptographically important, so this implementation is slow ...
   */
   BigInt accum = BigInt::one();
   BigInt g = reduce_mod.reduce(base);
   BigInt t;

   for(size_t i = 0; i != exp_bits; ++i) {
      t = reduce_mod.multiply(g, accum);
      g = reduce_mod.square(g);
      accum.ct_cond_assign(exp.get_bit(i), t);
   }
   return accum;
}

BigInt is_perfect_square(const BigInt& C) {
   if(C < 1) {
      throw Invalid_Argument("is_perfect_square requires C >= 1");
   }
   if(C == 1) {
      return BigInt::one();
   }

   const size_t n = C.bits();
   const size_t m = (n + 1) / 2;
   const BigInt B = C + BigInt::power_of_2(m);

   BigInt X = BigInt::power_of_2(m) - 1;
   BigInt X2 = (X * X);

   for(;;) {
      X = (X2 + C) / (2 * X);
      X2 = (X * X);

      if(X2 < B) {
         break;
      }
   }

   if(X2 == C) {
      return X;
   } else {
      return BigInt::zero();
   }
}

/*
* Test for primality using Miller-Rabin
*/
bool is_prime(const BigInt& n, RandomNumberGenerator& rng, size_t prob, bool is_random) {
   if(n == 2) {
      return true;
   }
   if(n <= 1 || n.is_even()) {
      return false;
   }

   const size_t n_bits = n.bits();

   // Fast path testing for small numbers (<= 65521)
   if(n_bits <= 16) {
      const uint16_t num = static_cast<uint16_t>(n.word_at(0));

      return std::binary_search(PRIMES, PRIMES + PRIME_TABLE_SIZE, num);
   }

   Modular_Reducer mod_n(n);

   if(rng.is_seeded()) {
      const size_t t = miller_rabin_test_iterations(n_bits, prob, is_random);

      if(is_miller_rabin_probable_prime(n, mod_n, rng, t) == false) {
         return false;
      }

      if(is_random) {
         return true;
      } else {
         return is_lucas_probable_prime(n, mod_n);
      }
   } else {
      return is_bailie_psw_probable_prime(n, mod_n);
   }
}

}  // namespace Botan
