// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <initializer_list>
#include "Test.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <wiiuse/wpad.h>
#include <ogcsys.h>
#include "ppu_intrinsics.h"
#include <limits>

double fres_expected(double val)
{
	static const int estimate_base[] = {
		0x7ff800, 0x783800, 0x70ea00, 0x6a0800,
		0x638800, 0x5d6200, 0x579000, 0x520800,
		0x4cc800, 0x47ca00, 0x430800, 0x3e8000,
		0x3a2c00, 0x360800, 0x321400, 0x2e4a00,
		0x2aa800, 0x272c00, 0x23d600, 0x209e00,
		0x1d8800, 0x1a9000, 0x17ae00, 0x14f800,
		0x124400, 0x0fbe00, 0x0d3800, 0x0ade00,
		0x088400, 0x065000, 0x041c00, 0x020c00,
	};
	static const int estimate_dec[] = {
		0x3e1, 0x3a7, 0x371, 0x340,
		0x313, 0x2ea, 0x2c4, 0x2a0,
		0x27f, 0x261, 0x245, 0x22a,
		0x212, 0x1fb, 0x1e5, 0x1d1,
		0x1be, 0x1ac, 0x19b, 0x18b,
		0x17c, 0x16e, 0x15b, 0x15b,
		0x143, 0x143, 0x12d, 0x12d,
		0x11a, 0x11a, 0x108, 0x106,
	};

	union
	{
		double valf;
		s64 vali;
	};
	valf = val;
	s64 mantissa = vali & ((1LL << 52) - 1);
	s64 sign = vali & (1ULL << 63);
	s64 exponent = vali & (0x7FFLL << 52);

	// Special case 0
	if (mantissa == 0 && exponent == 0)
		return sign ? -std::numeric_limits<double>::infinity() :
		std::numeric_limits<double>::infinity();
	// Special case NaN-ish numbers
	if (exponent == (0x7FFLL << 52))
	{
		if (mantissa == 0)
			return sign ? -0.0 : 0.0;
		return 0.0 + valf;
	}
	// Special case small inputs
	if (exponent < (895LL << 52))
		return sign ? -FLT_MAX : FLT_MAX;
	// Special case large inputs
	if (exponent >= (1149LL << 52))
		return sign ? -0.0f : 0.0f;

	exponent = (0x7FDLL << 52) - exponent;

	int i = (int)(mantissa >> 37);
	vali = sign | exponent;
	vali |= (s64)(estimate_base[i / 1024] - (estimate_dec[i / 1024] * (i % 1024) + 1) / 2) << 29;
	return valf;
}

double frsqrte_expected(double val)
{
	static const int estimate_base[] = {
		0x3ffa000, 0x3c29000, 0x38aa000, 0x3572000,
		0x3279000, 0x2fb7000, 0x2d26000, 0x2ac0000,
		0x2881000, 0x2665000, 0x2468000, 0x2287000,
		0x20c1000, 0x1f12000, 0x1d79000, 0x1bf4000,
		0x1a7e800, 0x17cb800, 0x1552800, 0x130c000,
		0x10f2000, 0x0eff000, 0x0d2e000, 0x0b7c000,
		0x09e5000, 0x0867000, 0x06ff000, 0x05ab800,
		0x046a000, 0x0339800, 0x0218800, 0x0105800,
	};
	static const int estimate_dec[] = {
		0x7a4, 0x700, 0x670, 0x5f2,
		0x584, 0x524, 0x4cc, 0x47e,
		0x43a, 0x3fa, 0x3c2, 0x38e,
		0x35e, 0x332, 0x30a, 0x2e6,
		0x568, 0x4f3, 0x48d, 0x435,
		0x3e7, 0x3a2, 0x365, 0x32e,
		0x2fc, 0x2d0, 0x2a8, 0x283,
		0x261, 0x243, 0x226, 0x20b,
	};

	union
	{
		double valf;
		s64 vali;
	};
	valf = val;
	s64 mantissa = vali & ((1LL << 52) - 1);
	s64 sign = vali & (1ULL << 63);
	s64 exponent = vali & (0x7FFLL << 52);

	// Special case 0
	if (mantissa == 0 && exponent == 0)
		return sign ? -std::numeric_limits<double>::infinity() :
		std::numeric_limits<double>::infinity();
	// Special case NaN-ish numbers
	if (exponent == (0x7FFLL << 52))
	{
		if (mantissa == 0)
		{
			if (sign)
				return std::numeric_limits<double>::quiet_NaN();
			return 0.0;
		}
		return 0.0 + valf;
	}
	// Negative numbers return NaN
	if (sign)
		return std::numeric_limits<double>::quiet_NaN();

	if (!exponent)
	{
		// "Normalize" denormal values
		do
		{
			exponent -= 1LL << 52;
			mantissa <<= 1;
		} while (!(mantissa & (1LL << 52)));
		mantissa &= (1LL << 52) - 1;
		exponent += 1LL << 52;
	}

	bool odd_exponent = !(exponent & (1LL << 52));
	exponent = ((0x3FFLL << 52) - ((exponent - (0x3FELL << 52)) / 2)) & (0x7FFLL << 52);

	int i = (int)(mantissa >> 37);
	vali = sign | exponent;
	int index = i / 2048 + (odd_exponent ? 16 : 0);
	vali |= (s64)(estimate_base[index] - estimate_dec[index] * (i % 2048)) << 26;
	return valf;
}

static inline double
fres_intrinsic(double val)
{
	double estimate;
	__asm__("fres %0,%1"
		/* outputs:  */ : "=f" (estimate)
		/* inputs:   */ : "f" (val));
	return estimate;
}

void ReciprocalTest()
{
	START_TEST();

	for (unsigned long long i = 0; i < 0x100000000LL; i += 1)
	{
		union {
			long long testi;
			double testf;
		};
		union {
			double expectedf;
			long long expectedi;
		};
		testi = i << 32;
		expectedf = frsqrte_expected(testf);
		testf = __frsqrte(testf);
		DO_TEST(testi == expectedi, "Bad frsqrte %lld %.10f %llx %.10f %llx", i, testf, testi, expectedf, expectedi);
		if (testi != expectedi) break;

		testi = i << 32;
		expectedf = fres_expected(testf);
		testf = fres_intrinsic(testf);
		DO_TEST(testi == expectedi, "Bad fres %lld %.10f %llx %.10f %llx", i, testf, testi, expectedf, expectedi);
		if (testi != expectedi) break;

		if (!(i & ((1 << 22) - 1)))
		{
			network_printf("Progress %lld\n", i);
			WPAD_ScanPads();

			if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
				break;
		}
	}
	END_TEST();
}

void FloatPrecisionTest()
{
	START_TEST();
	double a = 0x1.FFFFFFFFFFFFFp-2;
	double b = 0x1.p23 + 1;
	DO_TEST(0x1.p23f != (0x1.p23f + 1), "a", 0);
	DO_TEST(0x1.p24f == (0x1.p24f + 1), "a", 0);
	DO_TEST(a < 0.5, "a", 0);
	double estimate;
	__asm__("fadds %0,%1,%2"
		: "=f" (estimate)
		: "f" (a), "f" (b));
	DO_TEST(estimate == b, "%f %f %f", a, b, estimate);
	DO_TEST(float(a+b) == b + 1, "%f %f %f", a, b, estimate);
	END_TEST();
}

void IntConversionTest()
{
	START_TEST();
	volatile double x = -3;
	long long r = -3;
	volatile int y;
	for (unsigned i = 0; i < 35; ++i)
	{
		y = (int)x;
		DO_TEST(y == (r < -0x80000000LL ? -0x80000000LL : r), "%x", y);
		x *= 3;
		r *= 3;
	}
	x = 3;
	r = 3;
	for (unsigned i = 0; i < 35; ++i)
	{
		
		y = (int)x;
		DO_TEST(y == (r > 0x7FFFFFFF ? 0x7FFFFFFF : r), "%x", y);
		x *= 3;
		r *= 3;
	}
	x = std::numeric_limits<double>::infinity();
	y = (int)x;
	DO_TEST(y == 0x7FFFFFFFLL, "%x", y);

	x = -std::numeric_limits<double>::infinity();
	y = (int)x;
	DO_TEST(y == -0x80000000LL, "%x", y);

	x = std::numeric_limits<double>::quiet_NaN();
	y = (int)x;
	DO_TEST(y == -0x80000000LL, "%x", y);

	END_TEST();
}

void AddSingleTest()
{
	START_TEST();
	asm volatile("mtfsb0 29");
	struct test_elem
	{
		double a;
		double b;
		double result;
	};
	test_elem tests[] = {
		{ 1, 1, 2 },
		{ 3, 3, 6 },
		{ 3, -2.5, 0.5 },
		{ 0x1.FFFFFFFFFFFFFp-2, 0x1.p23 + 1, 0x1.p23 + 1 },
		{ 0x0.FFFFFFp-127, 0, 0x1p-127 },
		{ 0x0.FFFFFFp-126, 0, 0x1p-126 },
		{ 0x0.FFFFFFp-125, 0, 0x0.FFFFFFp-125 },
		{ 1, -1, 0 },
		{ -1, 1, 0 },
		{ 0, -0x1.p-1024, -0. },
		{ 0x1p0, 0x1.p-24, 1 },
		{ 0x1.FFFFFEp0, 0x1.p-24, 2 },
		{ 0x1p126, 0x1p126, 0x1p127 },
		{ 0x1p127, 0x1p127, std::numeric_limits<double>::infinity() },
		{ __builtin_nan("1"), __builtin_nan("2"), __builtin_nan("1") },
		{ -1, __builtin_nan("2"), __builtin_nan("2") },
		{ 0, 0, 0 },
		{ 0x0.FFFFFFp-125, 0, 0x0.FFFFFFp-125 },
		{ __builtin_nan("1"), __builtin_nan("2"), __builtin_nan("1") },
		{ -1, __builtin_nan("2"), __builtin_nan("2") },
	};
	for (unsigned i = 0; i < sizeof(tests) / sizeof(test_elem); ++i)
	{
		long long zero = 0;
		if (memcmp(&tests[i].a, &zero, 8) == 0 && memcmp(&tests[i].b, &zero, 8) == 0)
			asm volatile("mtfsb1 29");

		double result;
		__asm__("fadds %0,%1,%2"
			: "=f" (result)
			: "f" (tests[i].a), "f" (tests[i].b));
		long long x[4];
		memcpy(x, &tests[i], 24);
		memcpy(&x[3], &result, 8);
		DO_TEST(memcmp(&result, &tests[i].result, 8) == 0, "%llx %llx %llx %llx", x[0], x[1], x[2], x[3]);
	}
	END_TEST();
}

void SubSingleTest()
{
	START_TEST();
	asm volatile("mtfsb0 29");
	struct test_elem
	{
		double a;
		double b;
		double result;
	};
	test_elem tests[] = {
		{ 2, 1, 1 },
		{ 3, 3, 0 },
		{ 3, 2.5, 0.5 },
		{ std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() },
		{ 0, 0, 0 },
		{ __builtin_nan("1"), __builtin_nan("2"), __builtin_nan("1") },
	};
	for (unsigned i = 0; i < sizeof(tests) / sizeof(test_elem); ++i)
	{
		long long zero = 0;
		if (memcmp(&tests[i].a, &zero, 8) == 0 && memcmp(&tests[i].b, &zero, 8) == 0)
			asm volatile("mtfsb1 29");

		double result;
		__asm__("fsubs %0,%1,%2"
			: "=f" (result)
			: "f" (tests[i].a), "f" (tests[i].b));
		long long x[4];
		memcpy(x, &tests[i], 24);
		memcpy(&x[3], &result, 8);
		DO_TEST(memcmp(&result, &tests[i].result, 8) == 0, "%llx %llx %llx %llx", x[0], x[1], x[2], x[3]);
	}
	END_TEST();
}

void DivDoubleTest()
{
	START_TEST();
	asm volatile("mtfsb0 29");
	struct test_elem
	{
		double a;
		double b;
		double result;
	};
	test_elem tests[] = {
		{ 2, 1, 2 },
		{ 33, 11, 3 },
		{ 1, 3, 1. / 3. },
		{ 2, 3, 2. / 3. },
		{ 4134341, 814193264, 4134341. / 814193264 },
		{ 10, std::numeric_limits<double>::infinity(), 0},
	};
	for (unsigned i = 0; i < sizeof(tests) / sizeof(test_elem); ++i)
	{
		long long zero = 0;
		if (memcmp(&tests[i].a, &zero, 8) == 0 && memcmp(&tests[i].b, &zero, 8) == 0)
			asm volatile("mtfsb1 29");

		double result;
		__asm__("fdiv %0,%1,%2"
			: "=f" (result)
			: "f" (tests[i].a), "f" (tests[i].b));
		long long x[4];
		memcpy(x, &tests[i], 24);
		memcpy(&x[3], &result, 8);
		DO_TEST(memcmp(&result, &tests[i].result, 8) == 0, "%llx %llx %llx %llx", x[0], x[1], x[2], x[3]);
	}
	END_TEST();
}

void MaddSingleTest()
{
	START_TEST();
	asm volatile("mtfsb0 29");
	struct test_elem
	{
		double a;
		double b;
		double c;
		double result;
	};
	test_elem tests[] = {
		{ 0, 0, 0, 0 },
		{ 0x0.FFFFFFp-125, .5, 0, 0 },
	};
	for (unsigned i = 0; i < sizeof(tests) / sizeof(test_elem); ++i)
	{
		long long zero = 0;
		if (memcmp(&tests[i].a, &zero, 8) == 0 && memcmp(&tests[i].b, &zero, 8) == 0)
			asm volatile("mtfsb1 29");

		double result;
		__asm__("fmadds %0,%1,%2,%3"
			: "=f" (result)
			: "f" (tests[i].a), "f" (tests[i].b), "f" (tests[i].c));
		long long x[5];
		memcpy(x, &tests[i], 32);
		memcpy(&x[4], &result, 8);
		DO_TEST(memcmp(&result, &tests[i].result, 8) == 0, "%llx %llx %llx %llx %llx", x[0], x[1], x[2], x[3], x[4]);
	}
	END_TEST();
}

void MaddDoubleTest()
{
	START_TEST();
	asm volatile("mtfsb0 29");
	struct test_elem
	{
		double a;
		double b;
		double c;
		double result;
	};
	test_elem tests[] = {
		{ 1, 1, 1, 2 },
		{ 3, 3, 10, 19 },
		{ 0x1p1022, 0x1p-1030, 0x1p-5, 0x12p-9 },
		{ 0x1.FFFFFFFFFFFFFp-2, 0x1.FFFFFFFFFFFFFp-2, 0x1.FFFFFFFFFFFFFp-56, 0x1.FFFFFFFFFFFFFp-3 },
		{ 0x1.FFFFFFFFFFFFFp-2, 0x1.FFFFFFFFFFFFFp-2, 0x1.FFFFFFFFFFFFFp-55, 0x1p-2 },
		{ 0x1.FFFFFFFFFFFFFp-2, 0x1.FFFFFFFFFFFFFp-2, 0x1.FFFFFFFFFFFFFp-200, 0x1.FFFFFFFFFFFFEp-3 },
		{ 0x1.FFFFFFFFFFFFFp-2, 0x1.FFFFFFFFFFFFEp-2, 0x1.FFFFFFFFFFFFFp-200, 0x1.FFFFFFFFFFFFDp-3 },
		{ 0x1.FFFFFFFFFFFFFp-2, 0x1.FFFFFFFFFFFFEp-2, 0x1.FFFFFFFFFFFFFp-57, 0x1.FFFFFFFFFFFFEp-3 },
		{ 0x1.FFFFFFFFFFFFFp-2, 0x1.FFFFFFFFFFFFEp-2, -0x1.FFFFFFFFFFFFDp-3, 0x1.p-107 },
		{ std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() },
		{ 0x1.p1023, 0x1.p1023, -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() },
		{ 0x1.p1023, 0x1.p1023, 0, std::numeric_limits<double>::infinity() },
		{ __builtin_nan("1"), __builtin_nan("2"), __builtin_nan("3"), __builtin_nan("1") },
		{ -1, __builtin_nan("2"), __builtin_nan("3"), __builtin_nan("3") },
		{ -1, __builtin_nan("2"), -1, __builtin_nan("2") },
		{ 0, 0, 0, 0 },
		{ 0x1.FFFFFFFFFFFFFp-1023, 1, 0, 0x1.FFFFFFFFFFFFFp-1023 },
		{ 0x1.FFFFFFFFFFFFFp-1023, .5, 0, 0 },
		{ 0x1p1022, 0x1p-1030, 0x1p-5, 0x1p-5 },
		{ __builtin_nan("1"), __builtin_nan("2"), __builtin_nan("3"), __builtin_nan("1") },
		{ -1, __builtin_nan("2"), __builtin_nan("3"), __builtin_nan("3") },
		{ -1, __builtin_nan("2"), -1, __builtin_nan("2") },
	};
	for (unsigned i = 0; i < sizeof(tests) / sizeof(test_elem); ++i)
	{
		long long zero = 0;
		if (memcmp(&tests[i].a, &zero, 8) == 0 && memcmp(&tests[i].b, &zero, 8) == 0)
			asm volatile("mtfsb1 29");

		double result;
		__asm__("fmadd %0,%1,%2,%3"
			: "=f" (result)
			: "f" (tests[i].a), "f" (tests[i].b), "f" (tests[i].c));
		long long x[5];
		memcpy(x, &tests[i], 32);
		memcpy(&x[4], &result, 8);
		DO_TEST(memcmp(&result, &tests[i].result, 8) == 0, "%llx %llx %llx %llx %llx", x[0], x[1], x[2], x[3], x[4]);
	}
	END_TEST();
}

int main()
{
	network_init();
	WPAD_Init();

	//ReciprocalTest();
	//FloatPrecisionTest();
	//IntConversionTest();
	AddSingleTest();
	SubSingleTest();
	DivDoubleTest();
	MaddSingleTest();
	MaddDoubleTest();

	network_printf("Shutting down...\n");
	network_shutdown();

	return 0;
}
