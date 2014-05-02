// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.
#include "Test.h"
#include <stdlib.h>
#include <string.h>
#include <wiiuse/wpad.h>
#include <float.h>
#include <math.h>
#include "cgx.h"
#include "cgx_defaults.h"
#include "gxtest_util.h"

void BitfieldTest()
{
	START_TEST();

	TevReg reg;
	reg.hex = 0;
	reg.low = 0x345678;
	DO_TEST(reg.alpha == 837, "Values don't match (have: %d %d)", (s32)reg.alpha, (s32)reg.alpha);
	DO_TEST(reg.red == -392, "Values don't match (have: %d %d)", (s32)reg.red, (s32)reg.red);
	reg.low = 0x4BC6A8;
	DO_TEST(reg.alpha == -836, "Values don't match (have: %d %d)", (s32)reg.alpha);
	DO_TEST(reg.red == -344, "Values don't match (have: %d %d)", (s32)reg.red);
	reg.alpha = -263;
	reg.red = -345;
	DO_TEST(reg.alpha == -263, "Values don't match (have: %d %d)", (s32)reg.alpha);
	DO_TEST(reg.red == -345, "Values don't match (have: %d %d)", (s32)reg.red);
	reg.alpha = 15;
	reg.red = -619;
	DO_TEST(reg.alpha == 15, "Values don't match (have: %d %d)", (s32)reg.alpha);
	DO_TEST(reg.red == -619, "Values don't match (have: %d %d)", (s32)reg.red);
	reg.alpha = 523;
	reg.red = 176;
	DO_TEST(reg.alpha == 523, "Values don't match (have: %d %d)", (s32)reg.alpha);
	DO_TEST(reg.red == 176, "Values don't match (have: %d %d)", (s32)reg.red);

	END_TEST();
}

int TevCombinerExpectation(int a, int b, int c, int d, int shift, int bias, int op, int clamp)
{
	a &= 255;
	b &= 255;
	c &= 255;

	// TODO: Does not handle compare mode, yet
	c = c+(c>>7);
	u16 lshift = (shift == 1) ? 1 : (shift == 2) ? 2 : 0;
	u16 rshift = (shift == 3) ? 1 : 0;
	int round_bias = (shift==3) ? 0 : ((op==1) ? 127 : 128);
	int expected = (((a*(256-c) + b*c) << lshift)+round_bias)>>8; // lerp
	expected = (d << lshift) + expected * ((op == 1) ? (-1) : 1);
	expected += ((bias == 2) ? -128 : (bias == 1) ? 128 : 0) << lshift;
	expected >>= rshift;
	if (clamp)
		expected = (expected < 0) ? 0 : (expected > 255) ? 255 : expected;
	else
		expected = (expected < -1024) ? -1024 : (expected > 1023) ? 1023 : expected;
	return expected;
}

void TevCombinerTest()
{
	START_TEST();

	CGX_LOAD_BP_REG(CGXDefault<TwoTevStageOrders>(0).hex);

	CGX_BEGIN_LOAD_XF_REGS(0x1009, 1);
	wgPipe->U32 = 1; // 1 color channel

	LitChannel chan;
	chan.hex = 0;
	chan.matsource = 1; // from vertex
	CGX_BEGIN_LOAD_XF_REGS(0x100e, 1); // color channel 1
	wgPipe->U32 = chan.hex;
	CGX_BEGIN_LOAD_XF_REGS(0x1010, 1); // alpha channel 1
	wgPipe->U32 = chan.hex;

	auto ac = CGXDefault<TevStageCombiner::AlphaCombiner>(0);
	CGX_LOAD_BP_REG(ac.hex);

	// Test if we can reliably extract all bits of the tev combiner output...
	auto tevreg = CGXDefault<TevReg>(1, false); // c0
	for (int i = 0; i < 2; ++i)
		for (tevreg.red = -1024; tevreg.red != 1023; tevreg.red = tevreg.red+1)
		{
			CGX_LOAD_BP_REG(tevreg.low);
			CGX_LOAD_BP_REG(tevreg.high);

			auto genmode = CGXDefault<GenMode>();
			genmode.numtevstages = 0; // One stage
			CGX_LOAD_BP_REG(genmode.hex);

			auto cc = CGXDefault<TevStageCombiner::ColorCombiner>(0);
			cc.d = TEVCOLORARG_C0;
			CGX_LOAD_BP_REG(cc.hex);

			PE_CONTROL ctrl;
			ctrl.hex = BPMEM_ZCOMPARE<<24;
			ctrl.zformat = ZC_LINEAR;
			ctrl.early_ztest = 0;
			if (i == 0)
			{
				// 8 bits per channel: No worries about GetTevOutput making
				// mistakes when writing to framebuffer or when performing
				// an EFB copy.
				ctrl.pixel_format = PIXELFMT_RGB8_Z24;
				CGX_LOAD_BP_REG(ctrl.hex);

				int result = GXTest::GetTevOutput(genmode, cc, ac).r;

				DO_TEST(result == tevreg.red, "Got %d, expected %d", result, (s32)tevreg.red);
			}
			else
			{
				// TODO: This doesn't quite work, yet.
				break;

				// 6 bits per channel: Implement GetTevOutput functionality
				// manually, to verify how tev output is truncated to 6 bit
				// and how EFB copies upscale that to 8 bit again.
				ctrl.pixel_format = PIXELFMT_RGBA6_Z24;
				CGX_LOAD_BP_REG(ctrl.hex);

				GXTest::Quad().AtDepth(1.0).ColorRGBA(255,255,255,255).Draw();
				GXTest::CopyToTestBuffer(0, 0, 99, 9);
				CGX_ForcePipelineFlush();
				CGX_WaitForGpuToFinish();
				u16 result = GXTest::ReadTestBuffer(5, 5, 100).r;

				int expected = (((tevreg.red+1)>>2)&0xFF) << 2;
				expected = expected | (expected>>6);
				DO_TEST(result == expected, "Run %d: Got %d, expected %d", (int)tevreg.red, result, expected);
			}
		}

	// Now: Randomized testing of tev combiners.
	for (int i = 0x000000; i < 0x000F000; ++i)
	{
		if ((i & 0xFF00) == i)
			network_printf("progress: %x\n", i);

		auto genmode = CGXDefault<GenMode>();
		genmode.numtevstages = 0; // One stage
		CGX_LOAD_BP_REG(genmode.hex);

		// Randomly configured TEV stage, output in PREV.
		auto cc = CGXDefault<TevStageCombiner::ColorCombiner>(0);
		cc.a = TEVCOLORARG_C0;
		cc.b = TEVCOLORARG_C1;
		cc.c = TEVCOLORARG_C2;
		cc.d = TEVCOLORARG_ZERO; // TEVCOLORARG_CPREV; // NOTE: TEVCOLORARG_CPREV doesn't actually seem to fetch its data from PREV when used in the first stage?
		cc.shift = rand() % 4;
		cc.bias = rand() % 3;
		cc.op = rand()%2;
		cc.clamp = rand() % 2;
		CGX_LOAD_BP_REG(cc.hex);

		int a = -1024 + (rand() % 2048);
		int b = -1024 + (rand() % 2048);
		int c = -1024 + (rand() % 2048);
		int d = 0; //-1024 + (rand() % 2048);
		tevreg = CGXDefault<TevReg>(1, false); // c0
		tevreg.red = a;
		CGX_LOAD_BP_REG(tevreg.low);
		CGX_LOAD_BP_REG(tevreg.high);
		tevreg = CGXDefault<TevReg>(2, false); // c1
		tevreg.red = b;
		CGX_LOAD_BP_REG(tevreg.low);
		CGX_LOAD_BP_REG(tevreg.high);
		tevreg = CGXDefault<TevReg>(3, false); // c2
		tevreg.red = c;
		CGX_LOAD_BP_REG(tevreg.low);
		CGX_LOAD_BP_REG(tevreg.high);
		tevreg = CGXDefault<TevReg>(0, false); // prev
		tevreg.red = d;
		CGX_LOAD_BP_REG(tevreg.low);
		CGX_LOAD_BP_REG(tevreg.high);

		PE_CONTROL ctrl;
		ctrl.hex = BPMEM_ZCOMPARE<<24;
		ctrl.pixel_format = PIXELFMT_RGB8_Z24;
		ctrl.zformat = ZC_LINEAR;
		ctrl.early_ztest = 0;
		CGX_LOAD_BP_REG(ctrl.hex);

		int result = GXTest::GetTevOutput(genmode, cc, ac).r;

		int expected = TevCombinerExpectation(a, b, c, d, cc.shift, cc.bias, cc.op, cc.clamp);
		DO_TEST(result == expected, "Mismatch on a=%d, b=%d, c=%d, d=%d, shift=%d, bias=%d, op=%d, clamp=%d: expected %d, got %d", a, b, c, d, (u32)cc.shift, (u32)cc.bias, (u32)cc.op, (u32)cc.clamp, expected, result);

		WPAD_ScanPads();

		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
			break;
	}

	// Testing compare mode: (a.r > b.r) ? c.a : 0
	// One of the following will be the case for the alpha combiner:
	// (1) a.r will be assigned the value of c2.r (color combiner setting)
	// (2) a.r will be assigned the value of c0.r (alpha combiner setting)
	// If (1) is the case, the first run of this test will return black and
	// the second one will return red. If (2) is the case, the test will
	// always return black.
	// Indeed, this test shows that the alpha combiner reads the a and b
	// inputs from the color combiner setting, i.e. scenario (1) is
	// accurate to hardware behavior.
	for (int i = 0; i < 2; ++i)
	{
		auto genmode = CGXDefault<GenMode>();
		genmode.numtevstages = 0; // One stage
		CGX_LOAD_BP_REG(genmode.hex);

		auto cc = CGXDefault<TevStageCombiner::ColorCombiner>(0);
		cc.a = TEVCOLORARG_C2;
		cc.b = TEVCOLORARG_C1;
		CGX_LOAD_BP_REG(cc.hex);

		auto ac = CGXDefault<TevStageCombiner::AlphaCombiner>(0);
		ac.bias = TevBias_COMPARE;
		ac.a = TEVALPHAARG_A0; // different from color combiner
		ac.b = TEVALPHAARG_A1; // same as color combiner
		ac.c = TEVALPHAARG_A2;
		CGX_LOAD_BP_REG(ac.hex);

		PE_CONTROL ctrl;
		ctrl.hex = BPMEM_ZCOMPARE<<24;
		ctrl.pixel_format = PIXELFMT_RGBA6_Z24;
		ctrl.zformat = ZC_LINEAR;
		ctrl.early_ztest = 0;
		CGX_LOAD_BP_REG(ctrl.hex);

		tevreg = CGXDefault<TevReg>(1, false); // c0
		tevreg.red = 127; // 127 is always NOT less than 127.
		CGX_LOAD_BP_REG(tevreg.low);
		CGX_LOAD_BP_REG(tevreg.high);

		tevreg = CGXDefault<TevReg>(2, false); // c1
		tevreg.red = 127;
		CGX_LOAD_BP_REG(tevreg.low);
		CGX_LOAD_BP_REG(tevreg.high);

		tevreg = CGXDefault<TevReg>(3, false); // c2
		tevreg.red = 127+i; // 127+i is less than 127 iff i>0.
		tevreg.alpha = 255;
		CGX_LOAD_BP_REG(tevreg.low);
		CGX_LOAD_BP_REG(tevreg.high);

		int result = GXTest::GetTevOutput(genmode, cc, ac).a;
		int expected = (i == 1) ? 255 : 0;
		DO_TEST(result == expected, "Mismatch on run %d: expected %d, got %d", i, expected, result);
	}

	END_TEST();
}

void ClipTest()
{
	START_TEST();

	CGX_LOAD_BP_REG(CGXDefault<TwoTevStageOrders>(0).hex);

	CGX_BEGIN_LOAD_XF_REGS(0x1009, 1);
	wgPipe->U32 = 1; // 1 color channel

	LitChannel chan;
	chan.hex = 0;
	chan.matsource = 1; // from vertex
	CGX_BEGIN_LOAD_XF_REGS(0x100e, 1); // color channel 1
	wgPipe->U32 = chan.hex;
	CGX_BEGIN_LOAD_XF_REGS(0x1010, 1); // alpha channel 1
	wgPipe->U32 = chan.hex;

	CGX_LOAD_BP_REG(CGXDefault<TevStageCombiner::AlphaCombiner>(0).hex);

	auto genmode = CGXDefault<GenMode>();
	genmode.numtevstages = 0; // One stage
	CGX_LOAD_BP_REG(genmode.hex);

	PE_CONTROL ctrl;
	ctrl.hex = BPMEM_ZCOMPARE<<24;
	ctrl.pixel_format = PIXELFMT_RGB8_Z24;
	ctrl.zformat = ZC_LINEAR;
	ctrl.early_ztest = 0;
	CGX_LOAD_BP_REG(ctrl.hex);

	for (int step = 0; step < 13; ++step)
	{
		auto zmode = CGXDefault<ZMode>();
		CGX_LOAD_BP_REG(zmode.hex);

		// First off, clear previous screen contents
		CGX_SetViewport(0.0f, 0.0f, 201.0f, 50.0f, 0.0f, 1.0f); // stuff which really should not be filled
		auto cc = CGXDefault<TevStageCombiner::ColorCombiner>(0);
		cc.d = TEVCOLORARG_RASC;
		CGX_LOAD_BP_REG(cc.hex);
		GXTest::Quad().ColorRGBA(0,0,0,0xff).Draw();

		CGX_SetViewport(75.0f, 0.0f, 100.0f, 50.0f, 0.0f, 1.0f); // guardband
		cc = CGXDefault<TevStageCombiner::ColorCombiner>(0);
		cc.d = TEVCOLORARG_RASC;
		CGX_LOAD_BP_REG(cc.hex);
		GXTest::Quad().ColorRGBA(0,0x7f,0,0xff).Draw();

		CGX_SetViewport(100.0f, 0.0f, 50.0f, 50.0f, 0.0f, 1.0f); // viewport
		cc = CGXDefault<TevStageCombiner::ColorCombiner>(0);
		cc.d = TEVCOLORARG_RASC;
		CGX_LOAD_BP_REG(cc.hex);
		GXTest::Quad().ColorRGBA(0,0xff,0,0xff).Draw();

		// Now, enable testing viewport and draw the (red) testing quad
		CGX_SetViewport(100.0f, 0.0f, 50.0f, 50.0f, 0.0f, 1.0f);

		cc.d = TEVCOLORARG_C0;
		CGX_LOAD_BP_REG(cc.hex);

		auto tevreg = CGXDefault<TevReg>(1, false); // c0
		tevreg.red = 0xff;
		CGX_LOAD_BP_REG(tevreg.low);
		CGX_LOAD_BP_REG(tevreg.high);

		CGX_BEGIN_LOAD_XF_REGS(0x1005, 1);
		wgPipe->U32 = 0; // 0 = enable clipping, 1 = disable clipping

		bool expect_quad_to_be_drawn = true;
		int test_x = 125, test_y = 25; // Somewhere within the viewport
		GXTest::Quad test_quad;
		test_quad.ColorRGBA(0xff,0xff,0xff,0xff);

		switch (step)
		{
		// Rendering outside the viewport when scissor rect is bigger than viewport
		// TODO: What about partially covered primitives?

		case 0: // all vertices within viewport
			// Nothing to do
			break;


		case 1: // two vertices outside viewport, but within guardband
			test_quad.VertexTopLeft(-1.8f, 1.0f, 1.0f).VertexBottomLeft(-1.8f, -1.0f, 1.0f);
			test_x = 75; // TODO: Move closer to actual viewport, but debug readback issues first
			break;

		case 2: // two vertices outside viewport and guardband
			test_quad.VertexTopLeft(-2.5f, 1.0f, 1.0f).VertexBottomLeft(-2.5f, -1.0f, 1.0f);
			test_x = 51; // TODO: This is actually outside the guardband
			// TODO: Check x=50, should be green
			break;

		case 3: // all vertices outside viewport, but within guardband and NOT on the same side of the viewport
			test_quad.VertexTopLeft(-1.5f, 1.0f, 1.0f).VertexBottomLeft(-1.5f, -1.0f, 1.0f);
			test_quad.VertexTopRight(1.5f, 1.0f, 1.0f).VertexBottomRight(1.5f, 1.0f, 1.0f);
			test_x = 80; // TODO: MOve closer to actual viewport
			break;

		case 4: // all vertices outside viewport and guardband, but NOT on the same side of the viewport
			test_quad.VertexTopLeft(-2.5f, 1.0f, 1.0f).VertexBottomLeft(-2.5f, -1.0f, 1.0f);
			test_quad.VertexTopRight(2.5f, 1.0f, 1.0f).VertexBottomRight(2.5f, 1.0f, 1.0f);
			test_x = 51; // TODO: This is actually outside the guardband
			// TODO: Check x=50,x=200?,x=201?, 50 and 201 should be green, 200 should be red
			break;

		case 5: // all vertices outside viewport, but within guardband and on the same side of the viewport
			test_quad.VertexTopLeft(-1.8f, 1.0f, 1.0f).VertexBottomLeft(-1.8f, -1.0f, 1.0f);
			test_quad.VertexTopRight(-1.2f, 1.0f, 1.0f).VertexBottomRight(-1.2f, 1.0f, 1.0f);
			test_quad.VertexTopRight(1.5f, 1.0f, 1.0f);
			expect_quad_to_be_drawn = false;
			break;

		case 6: // guardband-clipping test
			// Exceeds the guard-band clipping plane by the viewport width,
			// so the primitive will get clipped such that one edge touches
			// the clipping plane.exactly at the vertical viewport center.
			// ASCII picture of clipped primitive (within guard-band region):
			// |-----  pixel row  0
			// |       pixel row  1
			// |       pixel row  2
			// |       pixel row  3
			// |       pixel row  4
			// \       pixel row  5 <-- vertical viewport center
			//  \      pixel row  6
			//   \     pixel row  7
			//    \    pixel row  8
			//     \   pixel row  9
			//      \  pixel row 10
			test_quad.VertexTopLeft(-4.0f, 1.0f, 1.0f);
			test_x = 51; // TODO: This is actually outside the guardband
			test_y = 1;
			// TODO: Test y roughly equals 60 (there's no good way to test this without relying on pixel-perfect clipping), here should NOT be a quad!
			break;

		// Depth clipping tests
		case 7:  // Everything behind z=w plane, depth clipping enabled
		case 8:  // Everything behind z=w plane, depth clipping disabled
			CGX_BEGIN_LOAD_XF_REGS(0x1005, 1);
			wgPipe->U32 = step - 7; // 0 = enable clipping, 1 = disable clipping

			test_quad.AtDepth(1.1);
			expect_quad_to_be_drawn = false;
			break;

		case 9:  // Everything in front of z=0 plane, depth clipping enabled
		case 10:  // Everything in front of z=0 plane, depth clipping disabled
			CGX_BEGIN_LOAD_XF_REGS(0x1005, 1);
			wgPipe->U32 = step - 9; // 0 = enable clipping, 1 = disable clipping

			test_quad.AtDepth(-0.00001);
			expect_quad_to_be_drawn = false;
			break;

		case 11: // Very slightly behind z=w plane, depth clipping enabled
		case 12: // Very slightly behind z=w plane, depth clipping disabled
			// The GC/Wii GPU doesn't implement IEEE floats strictly, hence
			// the sum of the projected position's z and w is a very small
			// number, which by IEEE would be non-zero but which in fact is
			// treated as zero.
			// In particular, the value by IEEE is -0.00000011920928955078125.
			CGX_BEGIN_LOAD_XF_REGS(0x1005, 1);
			wgPipe->U32 = step - 11; // 0 = enable clipping, 1 = disable clipping

			test_quad.AtDepth(1.0000001);
			break;

		case 13:  // One vertex behind z=w plane, depth clipping enabled
		case 14:  // One vertex behind z=w plane, depth clipping disabled
			CGX_BEGIN_LOAD_XF_REGS(0x1005, 1);
			wgPipe->U32 = step - 13; // 0 = enable clipping, 1 = disable clipping

			test_quad.VertexTopLeft(-1.0f, 1.0f, 1.5f);

			// whole primitive gets clipped away
			expect_quad_to_be_drawn = false;
			break;

		case 15:  // Three vertices with a very large value for z, depth clipping disabled
			CGX_BEGIN_LOAD_XF_REGS(0x1005, 1);
			wgPipe->U32 = 1; // 0 = enable clipping, 1 = disable clipping

			test_quad.VertexTopLeft(-1.0f, 1.0f, 65537.f);
			test_quad.VertexTopRight(1.0f, 1.0f, 65537.f);
			test_quad.VertexBottomLeft(-1.0f, -1.0f, 65537.f);
			break;

		// TODO: One vertex with z < 0, depth clipping enabled, primitive gets properly (!) clipped
		// TODO: One vertex with z < 0, depth clipping disabled, whole primitive gets drawn

		}

		test_quad.Draw();
		GXTest::CopyToTestBuffer(0, 0, 199, 49);
		CGX_WaitForGpuToFinish();

		GXTest::Vec4<u8> result = GXTest::ReadTestBuffer(test_x, test_y, 200);
		if (expect_quad_to_be_drawn)
			DO_TEST(result.r == 0xff, "Clipping test failed at step %d (expected quad to be shown at pixel (%d, %d), but it was not)", step, test_x, test_y);
		else
			DO_TEST(result.r == 0x00, "Clipping test failed at step %d (expected quad to be hidden at pixel (%d, %d), but it was not)", step, test_x, test_y);

		GXTest::DebugDisplayEfbContents();
	}

	END_TEST();
}

void TestDepth_SetViewport(float origin_x, float origin_y, float width, float height, float z_range, float z_far)
{
	CGX_BEGIN_LOAD_XF_REGS(0x101a, 6);
	wgPipe->F32 = width*0.5;
	wgPipe->F32 = -height*0.5;
	wgPipe->F32 = z_range;
	wgPipe->F32 = 342.0 + origin_x + width*0.5;
	wgPipe->F32 = 342.0 + origin_y + height*0.5;
	wgPipe->F32 = z_far;
}

void TestDepth() {
	START_TEST();

	CGX_LOAD_BP_REG(CGXDefault<TwoTevStageOrders>(0).hex);

	CGX_BEGIN_LOAD_XF_REGS(0x1009, 1);
	wgPipe->U32 = 1; // 1 color channel

	LitChannel chan;
	chan.hex = 0;
	chan.matsource = 1; // from vertex
	CGX_BEGIN_LOAD_XF_REGS(0x100e, 1); // color channel 1
	wgPipe->U32 = chan.hex;
	CGX_BEGIN_LOAD_XF_REGS(0x1010, 1); // alpha channel 1
	wgPipe->U32 = chan.hex;

	CGX_LOAD_BP_REG(CGXDefault<TevStageCombiner::AlphaCombiner>(0).hex);

	auto genmode = CGXDefault<GenMode>();
	genmode.numtevstages = 0; // One stage
	CGX_LOAD_BP_REG(genmode.hex);

	PE_CONTROL ctrl;
	ctrl.hex = BPMEM_ZCOMPARE << 24;
	ctrl.pixel_format = PIXELFMT_Z24; //PIXELFMT_RGB8_Z24;
	ctrl.zformat = ZC_LINEAR;
	ctrl.early_ztest = 0;
	CGX_LOAD_BP_REG(ctrl.hex);

	float testvals[] = { 0.25f, 0.5f, 0.75f, 1.0f, 16748210.0f / 16777216,
						29006.0f / 16777216, 29007.0f / 16777216, 16748209.0f / 16777216,
						13822720.0f / 16777216, 8388609.0f / 16777216, 8388611.0f / 16777216,
						1.0f / 16777216, 16777215.0f / 16777216, 8388610.0f / 16777216,
						8388607.0f / 16777216, 8388606.0f / 16777216,

						// Random
						12247796.0f / 16777216,
						488857.0f / 16777216,
						14712663.0f / 16777216,
						12827623.0f / 16777216,
						2608669.0f / 16777216,
						6858126.0f / 16777216,
						7466542.0f / 16777216,
						14121641.0f / 16777216,
						2483886.0f / 16777216,
						1396355.0f / 16777216,
						// Around 1/4
						4194302.0f / 16777216,
						4194303.0f / 16777216,
						4194305.0f / 16777216,
						4194306.0f / 16777216,
						// Around 3/4
						12582910.0f / 16777216,
						12582911.0f / 16777216,
						12582913.0f / 16777216,
						12582914.0f / 16777216,
						// Around 7/8
						14680063.0f / 16777216,
						14680064.0f / 16777216,
						14680065.0f / 16777216,
						// Misc
						12247795.0f / 16777216,
						12247797.0f / 16777216,
	};
	float test_zranges[] = { 0xFFFFFF, 0xFFFFFE, 0x800001, 0x800002, 0x1000000,
						     0x800000, 0x400000, 0x200000, 0xC00000, 0x600000 };
	for (int cur_range = 0; cur_range < int(sizeof(test_zranges) / sizeof(float)); cur_range++)
	for (int step = 0; step < int(sizeof(testvals) / sizeof(float)); ++step)
	//for (int step = 0; step < 2000; ++step)
	{
		auto zmode = CGXDefault<ZMode>();
		zmode.testenable = true;
		zmode.updateenable = true;
		zmode.func = COMPARE_ALWAYS;
		CGX_LOAD_BP_REG(zmode.hex);

		auto cc = CGXDefault<TevStageCombiner::ColorCombiner>(0);

		float zrange = test_zranges[cur_range];
		float zfar = zrange;
		TestDepth_SetViewport(100.0f, 0.0f, 50.0f, 50.0f, zrange, zfar);

		cc.d = TEVCOLORARG_C0;
		CGX_LOAD_BP_REG(cc.hex);

		auto tevreg = CGXDefault<TevReg>(1, false); // c0
		tevreg.red = 0xff;
		CGX_LOAD_BP_REG(tevreg.low);
		CGX_LOAD_BP_REG(tevreg.high);

		CGX_BEGIN_LOAD_XF_REGS(0x1005, 1);
		wgPipe->U32 = 0; // 0 = enable clipping, 1 = disable clipping

		int test_x = 125, test_y = 25; // Somewhere within the viewport
		GXTest::Quad test_quad;
		test_quad.ColorRGBA(0xff, 0, 0, 0xff);
		float testval = testvals[step];
		//float testval = (step + 1 + 0x800000 - 40) / float(0x1000000) / 16;
		//float testval = (rand() & 0xFFFFFF) / float(0x1000000) / 16;
		test_quad.AtDepth(testval);

		test_quad.Draw();
		GXTest::CopyToTestBuffer(0, 0, 199, 49);
		CGX_WaitForGpuToFinish();

		GXTest::Vec4<u8> result = GXTest::ReadTestBuffer(test_x, test_y, 200);
		int depthval = (result.r << 16) + (result.g << 8) + (result.b << 0);

		int guessdepthval;
		if (zrange == 0xFFFFFF && zfar == 0xFFFFFF)
		{
			// Tested down to 0x1.p-28 precision
			int input = int(testval * 0x10000000);
			if (input < 0x800000)
				guessdepthval = 0xFFFFFF - (input + 0xB) / 0x10;
			else if (input < 0x1000000)
				guessdepthval = 0xFFFFFF - (input + 0xA) / 0x10;
			else if (input < 0x2000000)
				guessdepthval = 0xFFFFFF - (input + 0x8) / 0x10;
			else if (input < 0x4000010)
				guessdepthval = 0xFFFFFF - (input + 0x4) / 0x10;
			else if (input < 0x8000020)
				guessdepthval = 0xFFFFFF - (input - 0x8) / 0x10;
			else
				guessdepthval = 0xFFFFFF - (input - 0x20) / 0x10;
		}
		else if (zrange == 0xFFFFFE && zfar == 0xFFFFFE)
		{
			// Tested down to 0x1.p-24 precision
			int input = int(testval * 0x1000000);
			if (input < 0x400001)
				guessdepthval = 0xFFFFFE - input;
			else if (input < 0x800001)
				guessdepthval = 0xFFFFFE - input + 1;
			else if (input < 0x800003)
				guessdepthval = 0xFFFFFE - input + 2;
			else
				guessdepthval = 0xFFFFFE - input + 3;
		}
		else if (zrange == 0x800001 && zfar == 0x800001)
		{
			// Tested down to 0x1.p-24 resolution
			int input = int(testval * 0x1000000);
			guessdepthval = 0x800001 - (input + 1) / 2;
		}
		else if (zrange == 0x800002 && zfar == 0x800002)
		{
			// Tested down to 0x1.p-24 precision
			int input = int(testval * 0x1000000);
			if (input < 0x300001)
				guessdepthval = 0x800002 - (input + 1) / 2;
			else if (input < 0xC00001)
				guessdepthval = 0x800002 - (input + 2) / 2;
			else if (input != 0xFFFFFF)
				guessdepthval = 0x800002 - (input + 3) / 2;
			else
				guessdepthval = 2;
		}
		else if (zrange == 0x1000000 && zfar == 0x1000000)
		{
			// Tested down to 0x1.p-24 precision
			int input = int(testval * 0x1000000);
			if (input < 0x800001)
				guessdepthval = 0x1000000 - (input);
			else
				guessdepthval = 0x1000000 - (input - 1);
		}
		else if (zrange == 0x800000 && zfar == 0x800000)
		{
			// Tested down to 0x1.p-24 precision
			int input = int(testval * 0x1000000);
			if (input < 0x800000)
				guessdepthval = 0x800000 - (input + 1) / 2;
			else
				guessdepthval = 0x800000 - (input) / 2;
		}
		else if (zrange == 0x400000 && zfar == 0x400000)
		{
			// Tested down to 0x1.p-24 precision
			int input = int(testval * 0x1000000);
			if (input < 0x800000)
				guessdepthval = 0x400000 - (input + 3) / 4;
			else
				guessdepthval = 0x400000 - (input + 2) / 4;
		}
		else if (zrange == 0x200000 && zfar == 0x200000)
		{
			// Tested down to 0x1.p-24 precision
			int input = int(testval * 0x1000000);
			if (input < 0x800000)
				guessdepthval = 0x200000 - (input + 7) / 8;
			else
				guessdepthval = 0x200000 - (input + 6) / 8;
		}
		else if (zrange == 0xC00000 && zfar == 0xC00000)
		{
			// Tested down to 0x1.p-28 precision
			int input = int(testval * 0x10000000);
			if (input <= 0x400000)
				guessdepthval = 0xC00000 - ((input * 3 + 0x2F) / 0x40);
			else if (input <= 0x800000)
				guessdepthval = 0xC00000 - ((input * 3 + 0x2E) / 0x40);
			else if (input <= 0x1000000)
				guessdepthval = 0xC00000 - ((input * 3 + 0x2D) / 0x40);
			else if (input <= 0x2000000)
				guessdepthval = 0xC00000 - ((input * 3 + 0x2A) / 0x40);
			else if (input <= 0x4000000)
				guessdepthval = 0xC00000 - ((input * 3 + 0x24) / 0x40);
			else if (input <= 0x5555550)
				guessdepthval = 0xC00000 - ((input * 3 + 0x18) / 0x40);
			else if (input <= 0x8000000)
				guessdepthval = 0xC00000 - ((input * 3 + 0x8) / 0x40);
			else if (input <= 0xaaaaab0)
				guessdepthval = 0xC00000 - ((input * 3 - 0x10) / 0x40);
			else
				guessdepthval = 0xC00000 - ((input * 3 - 0x30) / 0x40);
		}
		else if (zrange == 0x600000 && zfar == 0x600000)
		{
			// Tested down to 0x1.p-24 resolution
			int input = int(testval * 0x1000000);
			if (input <= 0x400000)
				guessdepthval = 0x600000 - (input - (input * 5 + 1) / 8);
			else if (input <= 0x555555)
				guessdepthval = 0x600000 - (input - (input * 5 + 2) / 8);
			else if (input <= 0x800000)
				guessdepthval = 0x600000 - (input - (input * 5 + 3) / 8);
			else if (input <= 0xaaaaab)
				guessdepthval = 0x600000 - (input - (input * 5 + 4) / 8);
			else
				guessdepthval = 0x600000 - (input - (input * 5 + 6) / 8);
		}
		else
		{
			// TODO: General case rounds incorrectly (but always within
			// 2 ulps of hardware answer).
			guessdepthval = int(-testval * zrange + zfar);
		}

		int mindepthval, maxdepthval;
		{
			// Tested down to 0x1.p-24 precision on the test zranges.
			int input = int(testval * 0x1000000);
			mindepthval = (-input * (long long)(zrange)+((long long)(zfar) << 24)) >> 24;
			maxdepthval = (-input * (long long)(zrange)+((long long)(zfar) << 24) + 0x2000000) >> 24;
		}

		if (guessdepthval < 0)
			guessdepthval = 0;
		if (guessdepthval > 0xFFFFFF)
			guessdepthval = 0xFFFFFF;

		DO_TEST(depthval == guessdepthval, "zrange %d, input %f, guess %d, actual %d\n", int(zrange), testval * 0x1000000, guessdepthval, depthval);
		DO_TEST(depthval >= mindepthval, "zrange %d, input %f, guess %d, actual %d\n", int(zrange), testval * 0x1000000, guessdepthval, depthval);
		DO_TEST(depthval <= maxdepthval, "zrange %d, input %f, guess %d, actual %d\n", int(zrange), testval * 0x1000000, guessdepthval, depthval);
		//network_printf("%10d %10d\n", int(testval * 0x1000000), depthval);
		GXTest::DebugDisplayEfbContents();
	}

	END_TEST();
}

int testvecs[][3] = {
	{ 139456800, 8242048, 9504 },
	{ 897646944, 8144544, 8192 },
	{ 139466304, 8373184, 65536 },
	{ 897655136, 8164448, 5120 },
	{ 139531840, 8438752, 6368 },
	{ 902903712, 8144544, 8192 },
	{ 139538208, 8242048, 65536 },
	{ 902235808, 8212416, 5280 },
	{ 909524800, 8634496, 524288 },
	{ 910049088, 9158784, 160288 },
	{ 841463072, 8217728, 8192 },
	{ 139603744, 8307616, 65536 },
	{ 841471264, 9351232, 123584 },
	{ 139669280, 8242048, 65536 },
	{ 1306977152, 8212416, 8192 },
	{ 139734816, 8307616, 65536 },
	{ 1306985344, 9699840, 268096 },
	{ 139800352, 8242048, 65536 },
	{ 1176073280, 9481248, 8192 },
	{ 139865888, 8307616, 65536 },
	{ 1176081472, 9699840, 314944 },
	{ 139931424, 8242048, 65536 },
	{ 1043444896, 9481248, 8192 },
	{ 139996960, 8307616, 65536 },
	{ 1043453088, 9699840, 163296 },
	{ 140062496, 8242048, 65536 },
	{ 947113120, 9481248, 8192 },
	{ 140128032, 8307616, 65536 },
	{ 947121312, 10184640, 251712 },
	{ 140193568, 8242048, 65536 },
	{ 1123569120, 9481248, 8192 },
	{ 140259104, 8307616, 65536 },
	{ 1123577312, 10184640, 75200 },
	{ 140324640, 8242048, 65536 },
	{ 922854848, 9481248, 8192 },
	{ 140390176, 8307616, 65536 },
	{ 922863040, 9829792, 30784 },
	{ 140455712, 8242048, 65536 },
	{ 835202944, 9481248, 8192 },
	{ 140521248, 8307616, 65536 },
	{ 902243904, 8149728, 1920 },
	{ 140586784, 8242048, 61536 },
	{ 140648320, 9481248, 65536 },
	{ 140713856, 9546816, 65536 },
	{ 140779392, 9481248, 65536 },
	{ 140844928, 9546816, 65536 },
	{ 140910464, 9481248, 65536 },
	{ 140976000, 9546816, 65536 },
	{ 141041536, 9481248, 65536 },
	{ 141107072, 9546816, 65536 },
	{ 141172608, 9481248, 65536 },
	{ 141238144, 9546816, 65536 },
	{ 141303680, 9481248, 65536 },
	{ 141369216, 9546816, 65536 },
	{ 141434752, 9481248, 65536 },
	{ 141500288, 9546816, 65536 },
	{ 141565824, 9481248, 65536 },
	{ 141631360, 9546816, 65536 },
	{ 141696896, 9481248, 65536 },
	{ 141762432, 9546816, 65536 },
	{ 141827968, 9481248, 65536 },
	{ 141893504, 9546816, 65536 },
	{ 141959040, 9481248, 65536 },
	{ 142024576, 9546816, 65536 },
	{ 142090112, 9481248, 65536 },
	{ 142155648, 9546816, 65536 },
	{ 142221184, 9481248, 65536 },
	{ 142286720, 9546816, 65536 },
	{ 142352256, 9481248, 65536 },
	{ 142417792, 9546816, 19680 },
	{ 142437472, 8242048, 65536 },
	{ 142503008, 8307616, 65536 },
	{ 142568544, 8242048, 65536 },
	{ 142634080, 8307616, 65536 },
	{ 142699616, 8242048, 65536 },
	{ 142765152, 8307616, 65536 },
	{ 142830688, 8242048, 65536 },
	{ 142896224, 8307616, 65536 },
	{ 142961760, 8242048, 65536 },
	{ 143027296, 8307616, 65536 },
	{ 143092832, 8242048, 65536 },
	{ 143158368, 8307616, 65536 },
	{ 143223904, 8242048, 65536 },
	{ 143289440, 8307616, 4000 },
	{ 143293440, 9481248, 17216 },
	{ 143310656, 8242048, 65536 },
	{ 143376192, 8307616, 65536 },
	{ 143441728, 8242048, 65536 },
	{ 143507264, 8307616, 65536 },
	{ 143572800, 8242048, 65536 },
	{ 143638336, 8307616, 35168 },
	{ 902774944, 9998368, 1600 },
	{ 897572256, 8242048, 74688 },
	{ 1459812544, 10307552, 165696 },
	{ 1306753024, 10473280, 224128 },
	{ 1123453376, 9481248, 115744 },
	{ 1043257984, 10697440, 186912 },
	{ 1175992096, 10884384, 81184 },
	{ 947076416, 8316768, 36704 },
	{ 841460576, 8353504, 2496 },
	{ 897510176, 8359712, 1888 },
	{ 1459597344, 8365920, 3744 },
	{ 1306578304, 9597024, 3648 },
	{ 1123353312, 8369696, 1952 },
	{ 1043135360, 9606816, 3104 },
	{ 1175899872, 8151584, 1152 },
	{ 947042464, 8027136, 320 },
	{ 841456736, 8028384, 64 },
	{ 897509984, 8030336, 192 },
	{ 1459597184, 8037280, 160 },
	{ 1306578112, 8042624, 192 },
	{ 1123353120, 8047776, 192 },
	{ 1043135200, 8052256, 160 },
	{ 1175899680, 8057952, 192 },
	{ 947042272, 8059072, 192 },
	{ 902230848, 9606816, 2944 },
	{ 907890976, 8634496, 524288 },
	{ 908415264, 9158784, 159104 },
	{ 922210720, 8634496, 377600 },
	{ 912473440, 10176448, 524288 },
	{ 912997728, 10700736, 418208 },
	{ 918320736, 9860480, 2144 },
	{ 922844448, 8266336, 10400 },
	{ 902233792, 9860480, 2016 },
	{ 902242496, 9606816, 1408 },
	{ 902241088, 9608256, 1408 },
	{ 902264000, 8073152, 864 },
	{ 902264864, 8089216, 160 },
	{ 902265024, 8089504, 160 },
	{ 902265184, 8089792, 160 },
	{ 902265344, 8090080, 160 },
	{ 902265504, 8090368, 160 },
	{ 902265664, 8090656, 160 },
	{ 902265824, 8090944, 160 },
	{ 902265984, 8091232, 320 },
	{ 902266304, 8091680, 448 },
	{ 902266752, 8092256, 320 },
	{ 902267072, 8092704, 320 },
	{ 902267392, 8093152, 352 },
	{ 902267744, 8093632, 320 },
	{ 902268064, 8094080, 320 },
	{ 902268384, 8094528, 352 },
	{ 902268736, 8095008, 160 },
	{ 902268896, 8095296, 160 },
	{ 902272608, 8281248, 12544 },
	{ 902313056, 8095840, 160 },
	{ 902313216, 8346944, 3424 },
	{ 902316640, 8096256, 160 },
	{ 902316800, 9606432, 3232 },
	{ 902320032, 8096672, 160 },
	{ 902320192, 8151584, 1088 },
	{ 902321280, 8097088, 192 },
	{ 902321472, 8097408, 896 },
	{ 902322368, 8098432, 160 },
	{ 902322528, 8098720, 160 },
	{ 902322688, 8099008, 160 },
	{ 902322848, 8099296, 736 },
	{ 902323584, 8100160, 160 },
	{ 902323744, 8100448, 160 },
	{ 902323904, 9621664, 1216 },
	{ 902325120, 8100864, 160 },
	{ 902325280, 8276768, 1760 },
	{ 902327040, 8101280, 512 },
	{ 902327552, 8101920, 320 },
	{ 902327872, 8102368, 448 },
	{ 902328320, 8102944, 320 },
	{ 902328640, 8103392, 320 },
	{ 902328960, 8103840, 352 },
	{ 902329312, 8104320, 320 },
	{ 902329632, 8104768, 320 },
	{ 902329952, 8105216, 352 },
	{ 902330304, 8220064, 4864 },
	{ 902829376, 8108320, 576 },
	{ 902832800, 8110816, 544 },
	{ 902833344, 8111488, 576 },
	{ 902833920, 8112192, 992 },
	{ 902836576, 8226240, 1056 },
	{ 902837632, 8227328, 1056 },
	{ 902838688, 8228416, 1120 },
	{ 902839808, 8229568, 1088 },
	{ 902840896, 8230688, 1056 },
	{ 902842560, 8116416, 480 },
	{ 902843040, 8293824, 1120 },
	{ 902845344, 8296192, 1248 },
	{ 902846592, 8297472, 1120 },
	{ 902848896, 8299840, 1248 },
	{ 902851392, 9691648, 32480 },
	{ 902335168, 7989472, 160 },
	{ 902335328, 8301120, 4416 },
	{ 902339744, 7990368, 160 },
	{ 902339904, 7993440, 160 },
	{ 902340064, 7994080, 160 },
	{ 902340288, 7995968, 192 },
	{ 902340480, 7996288, 192 },
	{ 902340672, 8305568, 9440 },
	{ 902245824, 7996736, 832 },
	{ 902380832, 8315040, 4128 },
	{ 904905472, 10013728, 524288 },
	{ 905429760, 10538016, 524288 },
	{ 905954048, 11062304, 524288 },
	{ 906478336, 11586592, 385760 },
	{ 120203424, 9547648, 50752 },
	{ 671391104, 9860480, 131072 },
	{ 671522176, 8634496, 131072 },
	{ 671653248, 8765600, 131072 },
	{ 671784320, 8896704, 131072 },
	{ 671915392, 9027808, 131072 },
	{ 672046464, 9158912, 131072 },
	{ 672177536, 10013728, 131072 },
	{ 672308608, 10144832, 131072 },
	{ 672439680, 10275936, 131072 },
	{ 672570752, 10407040, 131072 },
	{ 672701824, 10538144, 131072 },
	{ 672832896, 10669248, 131072 },
	{ 672963968, 10800352, 131072 },
	{ 673095040, 10931456, 131072 },
	{ 673226112, 11062560, 131072 },
	{ 673357184, 8303968, 7616 },
	{ 673364800, 11193664, 131072 },
	{ 673495872, 11324768, 131072 },
	{ 673626944, 11455872, 131072 },
	{ 673758016, 11586976, 131072 },
	{ 673889088, 11718080, 131072 },
	{ 674020160, 11849184, 131072 },
	{ 674151232, 11980288, 131072 },
	{ 674282304, 12111392, 131072 },
	{ 674413376, 12242496, 131072 },
	{ 674544448, 12373600, 131072 },
	{ 674675520, 12504704, 131072 },
	{ 674806592, 12635808, 131072 },
	{ 674937664, 12766912, 64128 },
	{ 143673504, 8113024, 64 },
	{ 143673552, 8113024, 32 },
	{ 143673572, 8113024, 32 },
	{ 143673580, 8311616, 5408 },
	{ 151132768, 8115136, 64 },
	{ 151132816, 8115136, 32 },
	{ 151132836, 8115136, 32 },
	{ 151132844, 12831072, 46752 },
	{ 154326848, 8115584, 64 },
	{ 154326896, 8115584, 32 },
	{ 154326916, 8115584, 32 },
	{ 154326924, 9290016, 21472 },
	{ 175615904, 8116672, 64 },
	{ 175615952, 8116672, 32 },
	{ 175615972, 8116672, 32 },
	{ 175615980, 12877856, 49152 },
	{ 185465760, 8117120, 64 },
	{ 185465808, 8117120, 32 },
	{ 185465828, 8117120, 32 },
	{ 185465836, 9691648, 29472 },
	{ 203551264, 8117568, 64 },
	{ 203551312, 8117568, 32 },
	{ 203551332, 8117568, 32 },
	{ 203551340, 12927040, 18144 },
	{ 209002208, 7997216, 64 },
	{ 209002256, 7997216, 32 },
	{ 209002276, 7997216, 32 },
	{ 209002284, 12945216, 46624 },
	{ 211914304, 7997664, 64 },
	{ 211914352, 7997664, 32 },
	{ 211914372, 7997664, 32 },
	{ 211914380, 12991872, 49120 },
	{ 211914304, 8019776, 64 },
	{ 211914352, 8019776, 32 },
	{ 211914372, 8019776, 32 },
	{ 211914380, 13041024, 49120 },
	{ 143678988, 9311520, 5440 },
	{ 143684428, 14472864, 5568 },
	{ 143689996, 9311520, 5600 },
	{ 143695596, 14472864, 5664 },
	{ 143701260, 9311520, 5728 },
	{ 143706988, 14472864, 5824 },
	{ 143712812, 9311520, 5952 },
	{ 143718764, 14472864, 5952 },
	{ 143724716, 9311520, 5984 },
	{ 143730700, 14472864, 6144 },
	{ 143736844, 9311520, 6304 },
	{ 143743148, 14472864, 6400 },
	{ 143749548, 9311520, 6464 },
	{ 143756012, 14472864, 6560 },
	{ 143762572, 9311520, 6592 },
	{ 143769164, 14472864, 6720 },
	{ 143775884, 9311520, 6784 },
	{ 143782668, 14472864, 6912 },
	{ 143789580, 9311520, 6976 },
	{ 143796556, 14472864, 7072 },
	{ 143803628, 9311520, 7104 },
	{ 143810732, 14472864, 7200 },
	{ 143817932, 9311520, 7296 },
	{ 143825228, 14472864, 7424 },
	{ 143832652, 9311520, 7520 },
	{ 143840172, 14472864, 7584 },
	{ 143847756, 14480480, 7808 },
	{ 143855564, 14488320, 7840 },
	{ 143863404, 14472864, 8064 },
	{ 143871468, 14480960, 8192 },
	{ 143879660, 14489184, 8256 },
	{ 143887916, 14472864, 8416 },
	{ 143896332, 14481312, 8480 },
	{ 143904812, 14489824, 8544 },
	{ 143913356, 14472864, 8640 },
	{ 143921996, 14481536, 8672 },
	{ 143930668, 14490240, 8800 },
	{ 143939468, 14472864, 8928 },
	{ 143948396, 14481824, 9088 },
	{ 143957484, 14490944, 9248 },
	{ 143966732, 14472864, 9376 },
	{ 143976108, 14482272, 9536 },
	{ 143985644, 14491840, 9632 },
	{ 143995276, 14472864, 9760 },
	{ 144005036, 14482656, 9920 },
	{ 144014956, 14492608, 9984 },
	{ 144024940, 14472864, 10080 },
	{ 144035020, 14482976, 10144 },
	{ 144045164, 14493152, 10240 },
	{ 144055404, 14472864, 10272 },
	{ 144065676, 14483168, 10400 },
	{ 144076076, 14493600, 10496 },
	{ 144086572, 14472864, 10592 },
	{ 144097164, 14483488, 10656 },
	{ 144107820, 14494176, 10688 },
	{ 144118508, 14472864, 10752 },
	{ 144129260, 14483648, 10784 },
	{ 151179596, 13090176, 44672 },
	{ 151224268, 14517568, 41280 },
	{ 151265548, 13090176, 39584 },
	{ 151305132, 14517568, 40704 },
	{ 151345836, 13090176, 41824 },
	{ 151387660, 14517568, 43072 },
	{ 151430732, 14560672, 47872 },
	{ 151478604, 14608576, 45280 },
	{ 151523884, 14517568, 45280 },
	{ 151569164, 14562880, 46848 },
	{ 151616012, 13090176, 43584 },
	{ 151659596, 14517568, 44000 },
	{ 151703596, 14561600, 45408 },
	{ 151749004, 14607040, 49088 },
	{ 151798092, 14517568, 48800 },
	{ 151846892, 14566400, 49376 },
	{ 151896268, 14517568, 47616 },
	{ 151943884, 14565216, 48800 },
	{ 151992684, 14614048, 48512 },
	{ 152041196, 14517568, 49056 },
	{ 152090252, 14566656, 48448 },
	{ 152138700, 14517568, 48736 },
	{ 152187436, 14566336, 49120 },
	{ 152236556, 14517568, 48640 },
	{ 152285196, 14566240, 49312 },
	{ 152334508, 14615584, 49664 },
	{ 152384172, 14517568, 48256 },
	{ 152432428, 14565856, 48640 },
	{ 152481068, 14614528, 48672 },
	{ 152529740, 14517568, 48576 },
	{ 152578316, 14566176, 48512 },
	{ 152626828, 14517568, 48512 },
	{ 152675340, 14566112, 48480 },
	{ 152723820, 14517568, 48416 },
	{ 154348396, 13090176, 29280 },
	{ 154377676, 14502176, 40512 },
	{ 154418188, 14542720, 41888 },
	{ 154460076, 14584640, 44352 },
	{ 154504428, 14502176, 43616 },
	{ 154548044, 14545824, 41888 },
	{ 154589932, 14502176, 41248 },
	{ 154631180, 14543456, 43808 },
	{ 154674988, 14587296, 46336 },
	{ 154721324, 14502176, 46944 },
	{ 154768268, 14549152, 45856 },
	{ 154814124, 14502176, 44000 },
	{ 154858124, 14546208, 42624 },
	{ 154900748, 14502176, 41280 },
	{ 154942028, 14543488, 40736 },
	{ 154982764, 14502176, 38464 },
	{ 155021228, 14540672, 36064 },
	{ 155057292, 14502176, 35520 },
	{ 155092812, 14537728, 32896 },
	{ 155125708, 14502176, 30368 },
	{ 155156076, 13090176, 28128 },
	{ 155184204, 14502176, 27488 },
	{ 155211692, 13090176, 25504 },
	{ 155237196, 14502176, 26048 },
	{ 155263244, 14528256, 29504 },
	{ 155292748, 14557792, 34592 },
	{ 155327340, 14502176, 33120 },
	{ 155360460, 14535328, 38848 },
	{ 155399308, 14574208, 40960 },
	{ 155440268, 14502176, 32128 },
	{ 155472396, 14534336, 36800 },
	{ 155509196, 14571168, 46144 },
	{ 155555340, 14502176, 42272 },
	{ 155597612, 14544480, 44960 },
	{ 155642572, 14589472, 45472 },
	{ 155688044, 14502176, 44000 },
	{ 155732044, 14546208, 47392 },
	{ 155779436, 14593632, 45376 },
	{ 155824812, 14502176, 47232 },
	{ 155872044, 14549440, 46720 },
	{ 155918764, 14502176, 46592 },
	{ 155965356, 14548800, 41856 },
	{ 156007212, 14502176, 41856 },
	{ 156049068, 14544064, 41664 },
	{ 156090732, 14585760, 42720 },
	{ 156133452, 14502176, 45632 },
	{ 156179084, 14547840, 48256 },
	{ 156227340, 14596128, 48416 },
	{ 156275756, 14502176, 48736 },
	{ 156324492, 14550944, 48672 },
	{ 156373164, 14599648, 49440 },
	{ 156422604, 14502176, 48896 },
	{ 156471500, 14551104, 48800 },
	{ 156520300, 14599936, 49120 },
	{ 156569420, 14502176, 49088 },
	{ 156618508, 14551296, 48480 },
	{ 156666988, 14502176, 48736 },
	{ 156715724, 14550944, 48096 },
	{ 156763820, 14502176, 47296 },
	{ 156811116, 14549504, 46496 },
	{ 156857612, 14502176, 46176 },
	{ 156903788, 14548384, 46688 },
	{ 156950476, 14595104, 46464 },
	{ 156996940, 14502176, 46496 },
	{ 157043436, 14548704, 46112 },
	{ 157089548, 14502176, 46144 },
	{ 157135692, 14548352, 45248 },
	{ 157180940, 14502176, 44928 },
	{ 157225868, 14547136, 44032 },
	{ 157269900, 14502176, 44352 },
	{ 157314252, 14546560, 44544 },
	{ 157358796, 14591136, 46464 },
	{ 157405260, 14502176, 45952 },
	{ 157451212, 14548160, 45664 },
	{ 157496876, 14502176, 45408 },
	{ 157542284, 14547616, 45056 },
	{ 157587340, 14502176, 44512 },
	{ 157631852, 14546720, 44736 },
	{ 157676588, 14502176, 42656 },
	{ 157719244, 14544864, 42912 },
	{ 157762156, 14587808, 43328 },
	{ 157805484, 14502176, 42720 },
	{ 157848204, 14544928, 42880 },
	{ 157891084, 14587840, 44256 },
	{ 157935340, 14502176, 42048 },
	{ 157977388, 14544256, 40000 },
	{ 158017388, 14502176, 40480 },
	{ 158057868, 14542688, 39232 },
	{ 158097100, 14502176, 38816 },
	{ 158135916, 14541024, 40256 },
	{ 158176172, 14581312, 42624 },
	{ 158218796, 14502176, 42368 },
	{ 158261164, 14544576, 40544 },
	{ 158301708, 14502176, 40480 },
	{ 175665132, 13131168, 47744 },
	{ 175712876, 14696832, 49216 },
	{ 175762092, 13131168, 47616 },
	{ 175809708, 14696832, 46976 },
	{ 175856684, 14743840, 48672 },
	{ 175905356, 14792544, 49312 },
	{ 175954668, 14696832, 48320 },
	{ 176002988, 14745184, 48384 },
	{ 176051372, 14793600, 49152 },
	{ 176100524, 14696832, 48448 },
	{ 176148972, 14745312, 49024 },
	{ 176197996, 14696832, 48160 },
	{ 176246156, 14745024, 48416 },
	{ 176294572, 14793472, 48832 },
	{ 176343404, 14696832, 48832 },
	{ 176392236, 14745696, 49312 },
	{ 176441548, 14795040, 49312 },
	{ 176490860, 14696832, 49408 },
	{ 176540268, 14746272, 49088 },
	{ 176589356, 14696832, 49120 },
	{ 176638476, 14745984, 48800 },
	{ 176687276, 14696832, 48000 },
	{ 176735276, 14744864, 48832 },
	{ 176784108, 14793728, 48160 },
	{ 176832268, 14696832, 49280 },
	{ 176881548, 14746144, 49248 },
	{ 176930796, 13131168, 47232 },
	{ 176978028, 14696832, 49056 },
	{ 177027084, 14745920, 48512 },
	{ 177075596, 13131168, 46944 },
	{ 177122540, 14696832, 49248 },
	{ 177171788, 13131168, 46400 },
	{ 177218188, 14696832, 48576 },
	{ 177266764, 14745440, 48160 },
	{ 177314924, 14696832, 47936 },
	{ 177362860, 14744800, 48608 },
	{ 177411468, 14793440, 49024 },
	{ 177460492, 14696832, 48928 },
	{ 177509420, 14745792, 48864 },
	{ 177558284, 14696832, 48192 },
	{ 177606476, 14745056, 48384 },
	{ 177654860, 14793472, 48736 },
	{ 177703596, 14696832, 49088 },
	{ 177752684, 14745952, 49024 },
	{ 177801708, 14696832, 47968 },
	{ 185495308, 13131168, 29472 },
	{ 185524780, 14696832, 35744 },
	{ 185560524, 14732608, 44192 },
	{ 185604716, 14776832, 46592 },
	{ 185651308, 14696832, 48896 },
	{ 185700204, 14745760, 47424 },
	{ 185747628, 14696832, 45888 },
	{ 185793516, 14742752, 44288 },
	{ 185837804, 14696832, 43072 },
	{ 185880876, 14739936, 45600 },
	{ 185926476, 14696832, 40224 },
	{ 185966700, 14737088, 40800 },
	{ 186007500, 14777920, 42272 },
	{ 186049772, 14696832, 41984 },
	{ 186091756, 14738848, 40320 },
	{ 186132076, 14696832, 41408 },
	{ 186173484, 14738272, 41408 },
	{ 186214892, 14779712, 41856 },
	{ 186256748, 14696832, 40064 },
	{ 186296812, 14736928, 37344 },
	{ 186334156, 14696832, 39392 },
	{ 186373548, 14736256, 38304 },
	{ 186411852, 14774592, 40480 },
	{ 186452332, 14696832, 38688 },
	{ 186491020, 14735552, 38432 },
	{ 186529452, 14774016, 39584 },
	{ 186569036, 14696832, 41504 },
	{ 186610540, 14738368, 42016 },
	{ 186652556, 14780416, 42432 },
	{ 186694988, 14696832, 39360 },
	{ 186734348, 14736224, 41408 },
	{ 186775756, 14777664, 42848 },
	{ 186818604, 14696832, 43360 },
	{ 186861964, 14740224, 43040 },
	{ 186905004, 14696832, 43104 },
	{ 186948108, 14739968, 44320 },
	{ 186992428, 14696832, 41696 },
	{ 187034124, 14738560, 41984 },
	{ 187076108, 14780576, 43808 },
	{ 187119916, 14696832, 43776 },
	{ 187163692, 14740640, 42976 },
	{ 187206668, 14783648, 43904 },
	{ 187250572, 14696832, 44352 },
	{ 187294924, 14741216, 40352 },
	{ 187335276, 14696832, 43520 },
	{ 187378796, 14740384, 42112 },
	{ 187420908, 14782528, 44160 },
	{ 187465068, 14696832, 39968 },
	{ 187505036, 14736832, 41184 },
	{ 187546220, 14696832, 39840 },
	{ 187586060, 14736704, 43296 },
	{ 187629356, 14780032, 43392 },
	{ 187672748, 14696832, 41664 },
	{ 187714412, 14738528, 45216 },
	{ 187759628, 14783776, 44864 },
	{ 187804492, 14696832, 41504 },
	{ 187845996, 14738368, 43264 },
	{ 187889260, 14781664, 44832 },
	{ 187934092, 14696832, 46400 },
	{ 187980492, 14743264, 47488 },
	{ 188027980, 14696832, 43808 },
	{ 188071788, 14740672, 43552 },
	{ 188115340, 14784256, 46912 },
	{ 188162252, 14696832, 46720 },
	{ 188208972, 14743584, 45696 },
	{ 188254668, 14696832, 45408 },
	{ 188300076, 14742272, 46656 },
	{ 188346732, 14788960, 46752 },
	{ 188393484, 14696832, 46528 },
	{ 188440012, 14743392, 46656 },
	{ 188486668, 14696832, 46400 },
	{ 188533068, 14743264, 44672 },
	{ 188577740, 14696832, 44032 },
	{ 188621772, 14740896, 43584 },
	{ 188665356, 14696832, 43008 },
	{ 188708364, 14739872, 45504 },
	{ 188753868, 14785408, 43904 },
	{ 188797772, 14696832, 46720 },
	{ 188844492, 14743584, 44768 },
	{ 188889260, 14696832, 45792 },
	{ 188935052, 14742656, 45024 },
	{ 188980076, 14696832, 43008 },
	{ 189023084, 14739872, 46272 },
	{ 189069356, 14786176, 43712 },
	{ 189113068, 14696832, 44640 },
	{ 189157708, 14741504, 44064 },
	{ 189201772, 14785600, 47136 },
	{ 189248908, 14696832, 46912 },
	{ 189295820, 14743776, 45056 },
	{ 189340876, 14696832, 46144 },
	{ 189387020, 14743008, 44576 },
	{ 189431596, 14696832, 44000 },
	{ 189475596, 14740864, 46048 },
	{ 189521644, 14786944, 45568 },
	{ 189567212, 14696832, 41664 },
	{ 189608876, 14738528, 45184 },
	{ 189654060, 14783744, 46976 },
	{ 189701036, 14696832, 45184 },
	{ 189746220, 14742048, 44032 },
	{ 189790252, 14696832, 44192 },
	{ 189834444, 14741056, 45536 },
	{ 189879980, 14786624, 46880 },
	{ 189926860, 14696832, 42752 },
	{ 189969612, 14739616, 47520 },
	{ 190017132, 14787168, 45568 },
	{ 190062700, 14696832, 44896 },
	{ 190107596, 14741760, 45248 },
	{ 190152844, 14696832, 44544 },
	{ 190197388, 14741408, 47904 },
	{ 190245292, 14696832, 42720 },
	{ 190288012, 14739584, 42912 },
	{ 190330924, 14782528, 47616 },
	{ 190378540, 14696832, 44640 },
	{ 190423180, 14741504, 47680 },
	{ 190470860, 14789216, 49696 },
	{ 190520556, 14696832, 48224 },
	{ 190568780, 14745088, 48384 },
	{ 190617164, 14696832, 43840 },
	{ 190661004, 14740704, 47616 },
	{ 190708620, 14788352, 45376 },
	{ 190753996, 14696832, 41280 },
	{ 190795276, 14738144, 44992 },
	{ 190840268, 14696832, 40992 },
	{ 190881260, 14737856, 44128 },
	{ 190925388, 14782016, 46208 },
	{ 190971596, 14696832, 41888 },
	{ 191013484, 14738752, 38304 },
	{ 191051788, 14696832, 41600 },
	{ 191093388, 14738464, 41824 },
	{ 191135212, 14696832, 40128 },
	{ 191175340, 14736992, 40032 },
	{ 191215372, 14696832, 39680 },
	{ 191255052, 14736544, 34464 },
	{ 191289516, 14696832, 36352 },
	{ 191325868, 14733216, 35456 },
	{ 191361324, 14768704, 40224 },
	{ 191401548, 14696832, 39040 },
	{ 191440588, 14735904, 36864 },
	{ 191477452, 14696832, 37856 },
	{ 191515308, 14734720, 38208 },
	{ 191553516, 14772960, 37920 },
	{ 191591436, 14696832, 40800 },
	{ 191632236, 14737664, 40896 },
	{ 191673132, 14696832, 38272 },
	{ 191711404, 14735136, 41376 },
	{ 191752780, 14776544, 41152 },
	{ 191793932, 14696832, 40672 },
	{ 191834604, 14737536, 40192 },
	{ 191874796, 14777760, 42496 },
	{ 191917292, 14696832, 43360 },
	{ 191960652, 14740224, 43008 },
	{ 192003660, 14696832, 42912 },
	{ 192046572, 14739776, 44096 },
	{ 192090668, 14783904, 44544 },
	{ 192135212, 14696832, 45728 },
	{ 192180940, 14742592, 44000 },
	{ 192224940, 14696832, 43104 },
	{ 192268044, 14739968, 41728 },
	{ 192309772, 14696832, 37312 },
	{ 192347084, 14734176, 38144 },
	{ 192385228, 14772352, 38080 },
	{ 192423308, 14696832, 35424 },
	{ 192458732, 14732288, 36832 },
	{ 192495564, 14696832, 35072 },
	{ 192530636, 14731936, 33120 },
	{ 192563756, 14696832, 33120 },
	{ 192596876, 14729984, 32416 },
	{ 192629292, 14696832, 32448 },
	{ 192661740, 13131168, 29408 },
	{ 192691148, 14696832, 31584 },
	{ 192722732, 14728448, 33568 },
	{ 192756300, 14696832, 29792 },
	{ 192786092, 14726656, 32256 },
	{ 192818348, 14758944, 30656 },
	{ 192849004, 14696832, 33248 },
	{ 192882252, 14730112, 31296 },
	{ 192913548, 14696832, 30912 },
	{ 192944460, 13131168, 26496 },
	{ 192970956, 14696832, 30624 },
	{ 193001580, 13131168, 28320 },
	{ 193029900, 14696832, 30304 },
	{ 193060204, 14727168, 31552 },
	{ 193091756, 13131168, 26880 },
	{ 193118636, 14696832, 29344 },
	{ 193147980, 14726208, 30112 },
	{ 193178092, 14756352, 30016 },
	{ 193208108, 13131168, 27968 },
	{ 193236076, 14696832, 29152 },
	{ 193265228, 13131168, 27232 },
	{ 193292460, 14696832, 25856 },
	{ 193318316, 13131168, 20736 },
	{ 193339052, 14696832, 21024 },
	{ 193360076, 13131168, 20544 },
	{ 193380620, 14696832, 18560 },
	{ 193399180, 13131168, 19648 },
	{ 193418828, 14543360, 16832 },
	{ 193435660, 13131168, 18272 },
	{ 193453932, 14543360, 14816 },
	{ 193468748, 13131168, 20448 },
	{ 193489196, 14696832, 18400 },
	{ 193507596, 14543360, 12608 },
	{ 193520204, 13131168, 17152 },
	{ 193537356, 14543360, 14528 },
	{ 193551884, 13131168, 13152 },
	{ 193565036, 13144352, 12448 },
	{ 193577484, 13131168, 9376 },
	{ 193586860, 14543360, 8576 },
	{ 193595436, 9311520, 7296 },
	{ 193602732, 9608512, 5568 },
	{ 193608300, 9311520, 5536 },
	{ 902895392, 14597056, 320 },
	{ 897572256, 10278880, 74688 },
	{ 897479200, 10587680, 30784 },
	{ 897572128, 14658272, 128 },
	{ 897512064, 10265760, 5024 },
	{ 897517088, 11013088, 576 },
	{ 897517664, 10955840, 576 },
	{ 897518240, 10831776, 2624 },
	{ 897443808, 10587680, 26752 },
	{ 897298208, 10213152, 17344 },
	{ 897443296, 10955840, 512 },
	{ 897317248, 11312224, 126048 },
	{ 897316736, 10956992, 512 },
	{ 897279872, 11438304, 18336 },
	{ 897270944, 10230528, 8928 },
	{ 897315552, 10957888, 512 },
	{ 897316224, 10958592, 512 },
	{ 897316064, 10959232, 160 },
	{ 897470560, 11456672, 8640 },
	{ 911804672, 11979200, 371392 },
	{ 913718624, 11629504, 144384 },
	{ 919913056, 11509152, 69664 },
	{ 920483264, 11773920, 18048 },
	{ 920293440, 12350624, 189824 },
	{ 918118592, 12540480, 100000 },
	{ 913655840, 11399808, 62784 },
	{ 903194432, 12640512, 524288 },
	{ 903718720, 13164800, 524288 },
	{ 904243008, 13689088, 360992 },
	{ 921055232, 14050112, 86528 },
	{ 906864096, 14136672, 81216 },
	{ 911026784, 14696832, 524288 },
	{ 911551072, 15221120, 230784 },
	{ 907706048, 14217920, 108864 },
	{ 914021472, 10587680, 16352 },
	{ 841594848, 10587680, 4160 },
	{ 841599008, 10591872, 2112 },
	{ 841601120, 10594016, 2304 },
	{ 841603424, 11416960, 18656 },
	{ 841622080, 11435648, 15264 },
	{ 841637344, 10596352, 7168 },
	{ 841644512, 11509152, 18816 },
	{ 841663328, 11450944, 1792 },
	{ 841665120, 11452768, 2080 },
	{ 841667200, 10957312, 992 },
	{ 841668192, 11528000, 18592 },
	{ 841686784, 11546624, 21472 },
	{ 841708256, 11454880, 1216 },
	{ 841709472, 11629504, 36128 },
	{ 841745600, 11568128, 12960 },
	{ 841758560, 11581120, 2208 },
	{ 841760768, 11665664, 34592 },
	{ 841795360, 11700288, 21632 },
	{ 841816992, 11721952, 32736 },
	{ 841849728, 11754720, 34816 },
	{ 841884544, 11583360, 1408 },
	{ 841885952, 11789568, 9184 },
	{ 841895136, 11798784, 3840 },
	{ 841898976, 11456128, 2208 },
	{ 841901184, 11979200, 36064 },
	{ 841937248, 12015296, 35808 },
	{ 841973056, 12051136, 17856 },
	{ 841990912, 12069024, 15328 },
	{ 842006240, 12084384, 32672 },
	{ 842038912, 12117088, 7744 },
	{ 842046656, 11802656, 1088 },
	{ 842047744, 12124864, 19968 },
	{ 842067712, 11458368, 4032 },
	{ 842071744, 12144864, 20000 },
	{ 842091744, 10966528, 640 },
	{ 842092384, 12164896, 32288 },
	{ 842124672, 12197216, 9216 },
	{ 842133888, 10972832, 608 },
	{ 842134496, 12206464, 2272 },
	{ 842136768, 12208768, 4448 },
	{ 842141216, 12213248, 16992 },
	{ 842158208, 12230272, 5600 },
	{ 842163808, 12235904, 21824 },
	{ 842185632, 12257760, 21408 },
	{ 842207040, 12279200, 4192 },
	{ 842213568, 12283424, 1184 },
	{ 842214752, 12284640, 8416 },
	{ 842223168, 12293088, 28608 },
	{ 842251776, 12321728, 20160 },
	{ 842271936, 12341920, 12416 },
	{ 842284352, 10986784, 416 },
	{ 842284768, 12354368, 8448 },
	{ 842293216, 12362848, 4672 },
	{ 842297888, 12367552, 2336 },
	{ 842300224, 10991040, 160 },
	{ 842300384, 12369920, 4768 },
	{ 842305152, 12374720, 21792 },
	{ 842326944, 12396544, 8256 },
	{ 842335200, 12404832, 1920 },
	{ 842337120, 12406784, 34016 },
	{ 842371136, 12440832, 2272 },
	{ 842373408, 12443136, 29120 },
	{ 842402528, 12472288, 2272 },
	{ 842404800, 12474592, 16800 },
	{ 842421600, 12491424, 7168 },
	{ 842428768, 12498624, 35808 },
	{ 842464576, 11002016, 128 },
	{ 842464704, 12534464, 20256 },
	{ 842498144, 12554752, 1280 },
	{ 842499424, 12556064, 18048 },
	{ 842517472, 12574144, 4288 },
	{ 842521760, 12578464, 34016 },
	{ 842555808, 11002912, 352 },
	{ 842556160, 12612512, 2496 },
	{ 842558656, 12615040, 18176 },
	{ 842576832, 12633248, 6560 },
	{ 842583392, 12639840, 1760 },
	{ 842585152, 12641632, 15104 },
	{ 842600256, 9077472, 1024 },
	{ 842601280, 12656768, 2080 },
	{ 842603360, 12658880, 21440 },
	{ 842624800, 12680352, 3552 },
	{ 842628352, 12683936, 31808 },
	{ 842660160, 12715776, 2464 },
	{ 842662624, 12718272, 4000 },
	{ 842666624, 9079392, 224 },
	{ 842666848, 9079744, 192 },
	{ 842667040, 12722304, 17440 },
	{ 842684480, 12739776, 1248 },
	{ 842685728, 9080320, 96 },
	{ 842685824, 12741056, 2336 },
	{ 842688160, 12743424, 4416 },
	{ 842692576, 12747872, 18880 },
	{ 842711456, 12766784, 1856 },
	{ 842713312, 12768672, 16000 },
	{ 842729312, 12784704, 32128 },
	{ 842761440, 12816864, 3968 },
	{ 842765408, 12820864, 1120 },
	{ 842766528, 12822016, 5056 },
	{ 842771584, 12827104, 15968 },
	{ 842787552, 12843104, 16032 },
	{ 842803584, 12859168, 18336 },
	{ 842821920, 12877536, 3200 },
	{ 842825120, 12880768, 4480 },
	{ 842829600, 12885280, 20384 },
	{ 842849984, 12905696, 8640 },
	{ 842858624, 12914368, 2112 },
	{ 842860736, 12916512, 31936 },
	{ 842892672, 12948480, 12352 },
	{ 842905024, 9082976, 672 },
	{ 842905696, 12960864, 19168 },
	{ 842924864, 12980064, 4608 },
	{ 842929472, 12984704, 33888 },
	{ 842963360, 9084160, 992 },
	{ 842964352, 13018624, 36128 },
	{ 843005600, 9085408, 640 },
	{ 843006240, 9086176, 160 },
	{ 843006400, 9086464, 192 },
	{ 843006592, 13054784, 20096 },
	{ 843026688, 13074912, 36352 },
	{ 843063040, 13111296, 2240 },
	{ 843065280, 13113568, 33696 },
	{ 843098976, 13147296, 1856 },
	{ 843100832, 13149184, 17216 },
	{ 843118048, 13166432, 1536 },
	{ 847519744, 9087680, 128 },
	{ 843238304, 9087936, 544 },
	{ 843300576, 13168000, 1280 },
	{ 843301856, 9088736, 672 },
	{ 843302528, 9089536, 256 },
	{ 843302784, 9089920, 32 },
	{ 843302816, 9090080, 160 },
	{ 843302976, 9090368, 160 },
	{ 843000480, 13169312, 5120 },
	{ 843244320, 13174464, 2720 },
	{ 843247040, 13177216, 1600 },
	{ 843249536, 9091040, 864 },
	{ 843269792, 13178848, 2272 },
	{ 843272064, 9092160, 800 },
	{ 843275968, 13181152, 2048 },
	{ 843278016, 9093216, 800 },
	{ 843283424, 9094144, 576 },
	{ 843303136, 9159648, 992 },
	{ 843306112, 13183232, 1632 },
	{ 843307744, 13184896, 3424 },
	{ 843311168, 13188352, 7200 },
	{ 843318368, 13195584, 26688 },
	{ 843345056, 13222304, 1344 },
	{ 843346400, 13223680, 17056 },
	{ 843363456, 13240768, 31360 },
	{ 843394816, 13272160, 4448 },
	{ 843399264, 13276640, 20832 },
	{ 843420096, 13297504, 152960 },
	{ 843573056, 9171104, 128 },
	{ 843573184, 13450496, 2336 },
	{ 843577440, 9098048, 864 },
	{ 843578304, 13452864, 2048 },
	{ 843580352, 13454944, 1888 },
	{ 843582240, 9171488, 704 },
	{ 843582944, 13456864, 896 },
	{ 843583840, 13592064, 8128 },
	{ 843591968, 13600224, 1088 },
	{ 843593056, 13458144, 416 },
	{ 843593472, 13458688, 992 },
	{ 843594464, 13601344, 1888 },
	{ 843596352, 13459936, 768 },
	{ 843597120, 13460832, 864 },
	{ 843597984, 13461824, 768 },
	{ 843598752, 13603264, 3616 },
	{ 843602368, 13462848, 832 },
	{ 843603200, 13463808, 896 },
	{ 843604096, 13606912, 1888 },
	{ 843605984, 13608832, 1888 },
	{ 843609120, 13465088, 704 },
	{ 843611808, 13610752, 1632 },
	{ 843613440, 13466048, 832 },
	{ 843614272, 13467008, 960 },
	{ 843615232, 13612416, 1888 },
	{ 843617120, 13614336, 1888 },
	{ 843619008, 13616256, 1888 },
	{ 843645600, 13468480, 672 },
	{ 843646272, 13618176, 3712 },
	{ 843683584, 13621920, 7904 },
	{ 843691488, 13629856, 24864 },
	{ 843654976, 13469664, 960 },
	{ 843655936, 13470752, 480 },
	{ 843716352, 13471360, 608 },
	{ 843716960, 13654752, 1664 },
	{ 843620896, 13472224, 704 },
	{ 843621600, 13656448, 2400 },
	{ 843656416, 13658880, 3520 },
	{ 843659936, 13662432, 8672 },
	{ 843624000, 13671136, 1408 },
	{ 843625408, 13672576, 4768 },
	{ 843630176, 13473696, 960 },
	{ 843631136, 13474784, 640 },
	{ 843668608, 13677376, 1600 },
	{ 843670208, 13679008, 4928 },
	{ 843676384, 13683968, 2496 },
	{ 843678880, 13686496, 4704 },
	{ 843675136, 13476064, 608 },
	{ 843675744, 13476800, 640 },
	{ 843631776, 13477568, 960 },
	{ 843632736, 13691232, 3776 },
	{ 843636512, 13478784, 896 },
	{ 843637408, 13695040, 3872 },
	{ 843641280, 13479936, 608 },
	{ 843641888, 13698944, 3712 },
	{ 843650816, 13480800, 608 },
	{ 843651424, 13702688, 3552 },
	{ 843718624, 13706272, 1184 },
	{ 844065952, 13481792, 320 },
	{ 844123424, 13707488, 5088 },
	{ 844035072, 13712608, 2016 },
	{ 844066272, 13714656, 27200 },
	{ 845273120, 13482624, 320 },
	{ 845273440, 13483072, 32 },
	{ 843989312, 13741888, 2528 },
	{ 846017792, 13483360, 160 },
	{ 846017952, 13744448, 39168 },
	{ 846057120, 13483776, 256 },
	{ 846057376, 13783648, 14080 },
	{ 846071456, 13484288, 96 },
	{ 846071552, 13797760, 11872 },
	{ 846083424, 13484640, 448 },
	{ 846083872, 13809664, 31616 },
	{ 846115488, 13841312, 1088 },
	{ 846118816, 13486592, 224 },
	{ 846119040, 13486944, 160 },
	{ 846119200, 13487232, 576 },
	{ 846119776, 13487936, 224 },
	{ 846129280, 13488800, 576 },
	{ 846129856, 13489504, 992 },
	{ 846130848, 13490624, 416 },
	{ 846131872, 13491840, 160 },
	{ 846132032, 13492128, 160 },
	{ 846132192, 13492416, 224 },
	{ 846134496, 13495040, 288 },
	{ 846134784, 13495456, 192 },
	{ 846386944, 13495776, 32 },
	{ 846386976, 13853248, 10240 },
	{ 846397216, 13496064, 416 },
	{ 846397632, 13496608, 576 },
	{ 846398208, 13497312, 160 },
	{ 846398368, 13863520, 1216 },
	{ 846399584, 13864768, 1216 },
	{ 846400800, 13866016, 1216 },
	{ 846403104, 13497984, 736 },
	{ 846403840, 13867264, 2048 },
	{ 846405888, 13869344, 1216 },
	{ 846407104, 13870592, 1216 },
	{ 846408320, 13499232, 736 },
	{ 846409056, 13871840, 2048 },
	{ 847094752, 13500224, 288 },
	{ 847388448, 13500640, 160 },
	{ 847389440, 13501824, 224 },
	{ 842211232, 13873920, 2336 },
	{ 842555776, 13502304, 32 },
	{ 843119584, 13876288, 37664 },
	{ 843157248, 13913984, 2528 },
	{ 843159776, 13916544, 35648 },
	{ 843195424, 13502848, 96 },
	{ 843195520, 13952224, 4096 },
	{ 843199616, 13956352, 1056 },
	{ 843200672, 13957440, 2880 },
	{ 843203552, 13503456, 768 },
	{ 843204320, 13960352, 1120 },
	{ 843205440, 13961504, 4896 },
	{ 843210336, 13966432, 1440 },
	{ 843211776, 13967904, 1440 },
	{ 843213216, 13969376, 1440 },
	{ 843214656, 13970848, 1504 },
	{ 843216160, 13972384, 1600 },
	{ 843217760, 13974016, 1568 },
	{ 843219328, 13975616, 2048 },
	{ 843221376, 13977696, 1856 },
	{ 843223232, 13979584, 1856 },
	{ 843225088, 13981472, 1984 },
	{ 843227072, 13983488, 1632 },
	{ 843228704, 13985152, 1952 },
	{ 843230656, 13987136, 1920 },
	{ 843232576, 13989088, 1856 },
	{ 843234432, 13990976, 1920 },
	{ 843236352, 13992928, 1952 },
	{ 843238848, 13994912, 1056 },
	{ 843239904, 13996000, 1408 },
	{ 843241312, 13506912, 672 },
	{ 843241984, 13507712, 384 },
	{ 843242368, 13997440, 1088 },
	{ 843243456, 13508352, 160 },
	{ 843243616, 13508640, 704 },
	{ 843248640, 13509472, 896 },
	{ 843250400, 13510496, 800 },
	{ 843251200, 13998560, 4256 },
	{ 843255456, 14002848, 4032 },
	{ 843259488, 14006912, 3776 },
	{ 843263264, 13511808, 96 },
	{ 843263360, 13512032, 704 },
	{ 843264448, 13512864, 96 },
	{ 843264544, 13513088, 320 },
	{ 843264064, 13513536, 96 },
	{ 843264160, 13513760, 288 },
	{ 843264864, 13514176, 32 },
	{ 843264896, 13514336, 160 },
	{ 843265056, 13514624, 256 },
	{ 843265312, 14010720, 4064 },
	{ 843269376, 13515136, 416 },
	{ 843272864, 14014816, 2304 },
	{ 843275168, 13515808, 800 },
	{ 843278816, 14017152, 2272 },
	{ 843281088, 14019456, 1728 },
	{ 843282816, 13516992, 608 },
	{ 843284000, 13517728, 960 },
	{ 843284960, 14021216, 1216 },
	{ 843286176, 14022464, 1440 },
	{ 843287616, 14023936, 1888 },
	{ 843289504, 14025856, 1056 },
	{ 843290560, 14026944, 1472 },
	{ 843292032, 13519456, 928 },
	{ 843292960, 14028448, 1888 },
	{ 843294848, 14030368, 1920 },
	{ 843296768, 14032320, 1952 },
	{ 843299840, 13520896, 736 },
	{ 847094592, 13521760, 160 },
	{ 847386656, 13522048, 832 },
	{ 847387488, 13523008, 960 },
	{ 847519872, 13516320, 96 },
	{ 847519968, 15975872, 21760 },
	{ 847541728, 16062976, 65536 },
	{ 847607264, 16128544, 65088 },
	{ 847672352, 16193664, 65184 },
	{ 847737536, 16258880, 64896 },
	{ 847802432, 16323808, 64896 },
	{ 847867328, 16388736, 65344 },
	{ 847932672, 16454112, 65440 },
	{ 847998112, 16519584, 65472 },
	{ 848063584, 16585088, 65408 },
	{ 848128992, 16650528, 64832 },
	{ 848193824, 16715392, 65152 },
	{ 848258976, 16780576, 65376 },
	{ 848324352, 16845984, 65376 },
	{ 848389728, 16911392, 62592 },
	{ 848452320, 16974016, 62368 },
	{ 848514688, 17036416, 65536 },
	{ 848580224, 17101984, 65312 },
	{ 848645536, 17167328, 64768 },
	{ 848710304, 17232128, 65504 },
	{ 848775808, 17297664, 64672 },
	{ 848840480, 17362368, 64704 },
	{ 848905184, 17427104, 65152 },
	{ 848970336, 17492288, 61344 },
	{ 849031680, 17553664, 64640 },
	{ 849096320, 17618336, 65536 },
	{ 849161856, 17683904, 64992 },
	{ 849226848, 17748928, 64160 },
	{ 849291008, 17813120, 65472 },
	{ 849356480, 17878624, 65280 },
	{ 849421760, 17943936, 64320 },
	{ 849486080, 15944864, 8064 },
	{ 849494144, 18008288, 58720 },
	{ 849552864, 18067040, 161376 },
	{ 849714240, 18228448, 174176 },
	{ 849888416, 15923360, 6464 },
	{ 847427296, 18551072, 160 },
	{ 847427552, 18568960, 160 },
	{ 847427840, 18852128, 160 },
	{ 847428384, 18852672, 160 },
	{ 847428896, 18853216, 160 },
	{ 847429408, 18853760, 160 },
	{ 847429824, 18854304, 160 },
	{ 847517920, 18859360, 160 },
	{ 847518240, 18859904, 160 },
	{ 847518624, 18861216, 160 },
	{ 847519040, 18861760, 160 },
	{ 847519296, 18865024, 160 },
	{ 847519584, 18865568, 160 },
	{ 902754112, 18973024, 384 },
	{ 902754496, 18973536, 416 },
	{ 902753216, 18974080, 416 },
	{ 902753632, 18974624, 480 },
	{ 902752448, 18975232, 384 },
	{ 902752832, 18975744, 384 },
	{ 902751680, 18976256, 384 },
	{ 902752064, 18976768, 384 },
	{ 902809472, 10208256, 3200 },
	{ 902812672, 18978528, 640 },
	{ 902285152, 18995616, 160 },
	{ 902285312, 18995904, 640 },
	{ 902285952, 18996672, 224 },
	{ 902286464, 18997376, 160 },
	{ 902286784, 18997888, 160 },
	{ 902287360, 18998656, 704 },
	{ 902288064, 18999488, 448 },
	{ 902288512, 19000064, 384 },
	{ 902288896, 19000576, 160 },
	{ 902289056, 10208256, 4064 },
	{ 902293120, 19000992, 224 },
	{ 902293344, 19001344, 800 },
	{ 902294144, 19002272, 800 },
	{ 902294944, 19003200, 800 },
	{ 902295744, 19004128, 800 },
	{ 902296544, 19005056, 800 },
	{ 902297344, 19005984, 256 },
	{ 902297600, 19006368, 224 },
	{ 902297824, 19006720, 288 },
	{ 902298112, 19007136, 224 },
	{ 902298336, 19007488, 224 },
	{ 902298560, 19007840, 224 },
	{ 902298784, 19008192, 256 },
	{ 902299040, 19008576, 224 },
	{ 902299264, 19008928, 160 },
	{ 902299424, 19009216, 800 },
	{ 902300224, 19010144, 800 },
	{ 902301024, 19011072, 352 },
	{ 902301376, 19011552, 224 },
	{ 902301600, 19011904, 800 },
	{ 902302400, 19012832, 288 },
	{ 902302688, 19013248, 224 },
	{ 902302912, 19013600, 800 },
	{ 902303712, 19014528, 288 },
	{ 902304000, 19014944, 224 },
	{ 902304224, 19015296, 800 },
	{ 902305024, 19016224, 160 },
	{ 902305184, 19016512, 224 },
	{ 902305408, 19016864, 160 },
	{ 902306144, 10110944, 6912 },
	{ 902350112, 19018240, 320 },
	{ 902350432, 10117888, 1760 },
	{ 902352192, 10119680, 1280 },
	{ 902353472, 19018944, 128 },
	{ 902353600, 19019200, 128 },
	{ 902353728, 10120992, 8704 },
	{ 902362432, 19019584, 64 },
	{ 902362496, 19019776, 448 },
	{ 902362944, 10129728, 8352 },
	{ 902371296, 19020480, 224 },
	{ 902371520, 19020832, 224 },
	{ 902371744, 19021184, 576 },
	{ 902372320, 19021888, 480 },
	{ 902372800, 19022496, 96 },
	{ 902372896, 19022720, 288 },
	{ 902373184, 19023136, 448 },
	{ 902373632, 19023712, 352 },
	{ 902373984, 19024192, 352 },
	{ 902374336, 19024672, 320 },
	{ 902374656, 10138112, 1440 },
	{ 902376096, 10139584, 1088 },
	{ 902377184, 10140704, 3648 },
	{ 844448704, 19493440, 205536 },
	{ 844654240, 19433056, 448 },
	{ 847389664, 9418240, 96 },
	{ 849894880, 10071072, 8800 },
	{ 844419136, 19493440, 29568 },
	{ 844335232, 19523040, 27168 },
	{ 844391552, 19550240, 27456 },
	{ 843846208, 11425536, 2048 },
	{ 843992416, 12713472, 4736 },
	{ 844419008, 9460064, 128 },
	{ 844362400, 19577728, 28864 },
	{ 843719808, 9460448, 320 },
	{ 844030784, 19799040, 4288 },
	{ 844391264, 9461024, 288 },
	{ 844093600, 19803360, 4448 },
	{ 715355168, 5925152, 96 },
	{ 713551104, 5924960, 96 },
	{ 713551200, 8488128, 36576 },
	{ 715355264, 8561312, 36576 },
	{ 713587776, 8524704, 36576 },
	{ 715391840, 8597888, 36576 },
	{ 849894880, 19640192, 8800 },
	{ 849903680, 19649024, 36832 },
	{ 849940512, 9401600, 352 },
	{ 849940864, 19685888, 2016 },
	{ 849942880, 19687936, 2304 },
	{ 849945184, 9402336, 352 },
	{ 849945536, 19690272, 24800 },
	{ 850303744, 9402944, 128 },
	{ 849970336, 19972192, 33824 },
	{ 850004160, 20006048, 34688 },
	{ 850038848, 9403456, 672 },
	{ 850039520, 9404256, 32 },
	{ 850039552, 9404416, 192 },
	{ 850040160, 19628704, 1440 },
	{ 850039744, 9404864, 416 },
	{ 850041600, 9405408, 224 },
	{ 850041824, 9405760, 864 },
	{ 850042688, 9406752, 32 },
	{ 850042720, 9406912, 192 },
	{ 850042912, 9407232, 160 },
	{ 850043072, 9407520, 768 },
	{ 850043840, 20040768, 39072 },
	{ 850082912, 19715104, 1600 },
	{ 850084512, 9408672, 32 },
	{ 850084544, 9408832, 192 },
	{ 850084736, 19716736, 1600 },
	{ 850086336, 9409280, 160 },
	{ 850086496, 20079872, 9088 },
	{ 850095584, 20088992, 6112 },
	{ 850101696, 9409824, 32 },
	{ 850101728, 9409984, 160 },
	{ 850101888, 20095136, 2560 },
	{ 850104448, 9410400, 160 },
	{ 850104608, 9410688, 960 },
	{ 850105568, 20097728, 7968 },
	{ 850113536, 20105728, 6048 },
	{ 850120192, 20111808, 1664 },
	{ 850121856, 20113504, 4768 },
	{ 850126624, 20118304, 1440 },
	{ 850128064, 9412416, 736 },
	{ 850128800, 9413280, 992 },
	{ 850129792, 20119776, 8288 },
	{ 850138080, 9414528, 448 },
	{ 850138528, 20128096, 7520 },
	{ 850146048, 9415232, 448 },
	{ 850146496, 20135648, 12672 },
	{ 850159168, 9415936, 992 },
	{ 850160160, 9417056, 544 },
	{ 850160704, 20148352, 18688 },
	{ 850179392, 20167072, 36320 },
	{ 850220128, 9418464, 800 },
	{ 850220928, 20203424, 4320 },
	{ 850225248, 9419520, 448 },
	{ 850233056, 9420096, 160 },
	{ 850303872, 9413216, 96 },
	{ 850303968, 20964704, 2752 },
	{ 850306720, 19972192, 65536 },
	{ 850372256, 19640192, 65472 },
	{ 850437728, 20104512, 55200 },
	{ 850492928, 20063680, 15904 },
	{ 850302528, 14673024, 160 },
	{ 850302816, 13533536, 160 },
	{ 850303296, 21041472, 160 },
	{ 850303584, 21067104, 160 },
	{ 847427712, 19385664, 128 },
	{ 842484960, 19972192, 13184 },
	{ 727124224, 5925152, 96 },
	{ 725606336, 5924960, 96 },
	{ 725606432, 8488128, 36576 },
	{ 727124320, 8561312, 36576 },
	{ 725643008, 8524704, 36576 },
	{ 727160896, 8597888, 36576 },
	{ 897520864, 10128544, 2112 },
	{ 897522976, 10130688, 2112 },
	{ 850302464, 19390656, 64 },
	{ 850302688, 19383360, 128 },
	{ 725679584, 8488128, 36576 },
	{ 727197472, 8561312, 36576 },
	{ 841599008, 15183808, 2112 },
	{ 850508832, 15944864, 8000 },
	{ 841816992, 11427424, 32736 },
	{ 725716160, 8524704, 36576 },
	{ 727234048, 8597888, 36576 },
	{ 850516832, 13614880, 24096 },
	{ 842046656, 14030048, 1088 },
	{ 850540928, 13603968, 2560 },
	{ 850543488, 11460192, 2112 },
	{ 842136768, 19305216, 4448 },
	{ 850545600, 11422208, 2688 },
	{ 842185632, 10213056, 21408 },
	{ 842214752, 14130816, 8416 },
	{ 850548288, 15134048, 21696 },
	{ 842297888, 9599424, 2336 },
	{ 842300224, 18967424, 160 },
	{ 850569984, 10599136, 4640 },
	{ 850574624, 13790912, 22176 },
	{ 850596800, 18970528, 416 },
	{ 850597216, 19988608, 21856 },
	{ 850619072, 20047456, 1184 },
	{ 850620256, 18976096, 128 },
	{ 842765408, 14410240, 1120 },
	{ 850620384, 9280192, 8992 },
	{ 850629376, 13409440, 21856 },
	{ 842858624, 10071072, 2112 },
	{ 850651232, 18939264, 2272 },
	{ 843005600, 18994944, 640 },
	{ 843006240, 18998368, 160 },
	{ 850799136, 19005664, 320 },
	{ 850655776, 18949376, 1024 },
	{ 850656800, 18955424, 960 },
	{ 850657760, 20940576, 2208 },
	{ 850659968, 18957152, 448 },
	{ 850660416, 18941568, 2048 },
	{ 850662464, 19254656, 1248 },
	{ 850663712, 13639008, 2016 },
	{ 850665728, 18959168, 1024 },
	{ 850666752, 18960320, 896 },
	{ 850698304, 13641056, 3520 },
	{ 850701824, 15113984, 8064 },
	{ 850709888, 15122080, 2048 },
	{ 850711936, 10084800, 8416 },
	{ 850720352, 18961856, 96 },
	{ 850683584, 18946432, 800 },
	{ 850693088, 15164928, 1696 },
	{ 850694784, 18950528, 96 },
	{ 850694880, 10981952, 768 },
	{ 850695648, 10990080, 864 },
	{ 850697312, 10960448, 992 },
	{ 850778784, 10961568, 96 },
	{ 850778880, 10965376, 512 },
	{ 850777984, 10966016, 96 },
	{ 850778080, 10967296, 704 },
	{ 850780224, 10968128, 32 },
	{ 850780256, 10978112, 384 },
	{ 850782368, 10982976, 32 },
	{ 850782400, 13523488, 864 },
	{ 850785600, 13524480, 96 },
	{ 850785696, 13524704, 512 },
	{ 850783264, 13525344, 32 },
	{ 850783296, 10093248, 2304 },
	{ 850787168, 14434272, 2432 },
	{ 850789600, 10590400, 2880 },
	{ 850792864, 13526016, 224 },
	{ 850793088, 8685664, 1120 },
	{ 850794208, 13526656, 160 },
	{ 850794368, 13527680, 768 },
	{ 850795136, 13528704, 96 },
	{ 850795232, 13529344, 448 },
	{ 850779392, 13529920, 32 },
	{ 850779424, 13533920, 800 },
	{ 850775296, 13534976, 160 },
	{ 850775456, 19731520, 1088 },
	{ 850776544, 13535392, 160 },
	{ 850776704, 10073216, 1088 },
	{ 850786208, 13535808, 192 },
	{ 850786400, 13537440, 416 },
	{ 850786816, 13537984, 32 },
	{ 850786848, 13538144, 320 },
	{ 850792480, 13538720, 32 },
	{ 850792512, 13538880, 352 },
	{ 850769312, 13539360, 544 },
	{ 850769856, 12705152, 5248 },
	{ 850780640, 13540160, 32 },
	{ 850780672, 9289216, 1696 },
	{ 850775104, 13540448, 32 },
	{ 850775136, 13540608, 160 },
	{ 850795680, 10593312, 2176 },
	{ 850797856, 13545664, 640 },
	{ 850799456, 13503264, 96 },
	{ 850799552, 19655296, 4544 },
	{ 850804096, 13833312, 62112 },
	{ 725752736, 8488128, 36576 },
	{ 727270624, 8561312, 36576 },
	{ 850866208, 14019328, 62720 },
	{ 850928928, 14442976, 64544 },
	{ 850993472, 20207328, 61536 },
	{ 851055008, 11266016, 65472 },
	{ 851120480, 11331520, 65280 },
	{ 851185760, 19493440, 65152 },
	{ 851250912, 19558624, 62912 },
	{ 851313824, 13603968, 63712 },
	{ 851377536, 20972384, 48320 },
	{ 850798720, 13478560, 160 },
	{ 850798976, 10990816, 160 },
	{ 725789312, 8524704, 36576 },
	{ 727307200, 8597888, 36576 },
	{ 725825888, 8488128, 36576 },
	{ 727343776, 8561312, 36576 },
	{ 725862464, 8524704, 36576 },
	{ 727380352, 8597888, 36576 },
	{ 725899040, 8488128, 36576 },
	{ 727416928, 8561312, 36576 },
	{ 897525088, 18569376, 768 },
	{ 850798496, 18570272, 96 },
	{ 852054720, 10077536, 4160 },
	{ 852058880, 21551104, 23392 },
	{ 852082272, 19705408, 8512 },
	{ 852090784, 11460224, 2304 },
	{ 852093088, 18537632, 704 },
	{ 852093792, 20034432, 8864 },
	{ 852102656, 21574528, 20416 },
	{ 852123072, 21594976, 22944 },
	{ 852146016, 18538848, 160 },
	{ 852146176, 19627328, 2208 },
	{ 852181120, 21617952, 34240 },
	{ 852215360, 21652224, 7840 },
	{ 852223200, 18804064, 18208 },
	{ 852241408, 19690112, 8576 },
	{ 852249984, 21526656, 6368 },
	{ 852256352, 21022656, 2304 },
	{ 852294720, 18542912, 32 },
	{ 725935616, 8524704, 36576 },
	{ 727453504, 8597888, 36576 },
	{ 852294752, 20956928, 9376 },
	{ 852304128, 20043328, 3744 },
	{ 852307872, 18822304, 10112 },
	{ 852319072, 18832448, 35296 },
	{ 852354368, 18867776, 21664 },
	{ 852376032, 18889472, 9088 },
	{ 852385120, 18898592, 32320 },
	{ 852417440, 20048800, 17792 },
	{ 852435232, 20066624, 23488 },
	{ 852458720, 20194016, 2272 },
	{ 852465440, 20090144, 8992 },
	{ 852474432, 20099168, 8608 },
	{ 852483040, 20107808, 32640 },
	{ 852515680, 20140480, 8960 },
	{ 852524640, 20149472, 9216 },
	{ 852533856, 18569376, 352 },
	{ 852534208, 19698720, 4128 },
	{ 852538336, 14130816, 68096 },
	{ 852606432, 20158720, 19296 },
	{ 852625728, 14198944, 18720 },
	{ 852646784, 18570592, 416 },
	{ 852647200, 20966336, 5920 },
	{ 852653120, 14217696, 17600 },
	{ 852670720, 18930944, 8000 },
	{ 852678720, 14235328, 31200 },
	{ 852714592, 18579584, 192 },
	{ 852714944, 14266560, 16896 },
	{ 852731840, 18580032, 448 },
	{ 852732288, 20178048, 9312 },
	{ 852741600, 14283488, 7456 },
	{ 852749056, 19305216, 4032 },
	{ 852753088, 14290976, 34720 },
	{ 852787808, 19248832, 5088 },
	{ 852792896, 14325728, 10272 },
	{ 852803168, 14336032, 22016 },
	{ 852825184, 18581504, 672 },
	{ 852825856, 14358080, 9632 },
	{ 852835488, 14367744, 22720 },
	{ 852858240, 21162496, 14816 },
	{ 852873056, 21177344, 29504 },
	{ 852902560, 21206880, 31456 },
	{ 852934016, 21238368, 26368 },
	{ 852960384, 21533056, 6560 },
	{ 852969024, 21264768, 19168 },
	{ 852988192, 20204160, 2656 },
	{ 853022656, 18583456, 160 },
	{ 853022816, 14390496, 9408 },
	{ 853032224, 21283968, 15744 },
	{ 853047968, 21299744, 8608 },
	{ 853056576, 21308384, 37248 },
	{ 853093824, 18584256, 320 },
	{ 853094144, 10478624, 1216 },
	{ 853095360, 21345664, 35616 },
	{ 853130976, 21381312, 15776 },
	{ 853146752, 12713024, 4576 },
	{ 853155296, 14399936, 4288 },
	{ 853159584, 21397120, 7744 },
	{ 853167328, 19713952, 2304 },
	{ 853174816, 18585600, 192 },
	{ 853175008, 21404896, 22976 },
	{ 853197984, 21427904, 7616 },
	{ 853205600, 21435552, 22208 },
	{ 853227808, 21457792, 3200 },
	{ 853231008, 21461024, 31904 },
	{ 853262912, 21492960, 3200 },
	{ 853266112, 10598016, 1088 },
	{ 853267200, 21496192, 11872 },
	{ 853279072, 11629088, 19552 },
	{ 853298624, 11648672, 19328 },
	{ 853317952, 11668032, 23776 },
	{ 853341728, 21508096, 3648 },
	{ 853345376, 11691840, 7936 },
	{ 853353312, 11699808, 13824 },
	{ 853367136, 21511776, 2240 },
	{ 853369376, 11713664, 4608 },
	{ 853373984, 18505568, 992 },
	{ 853374976, 18506688, 576 },
	{ 853375552, 21514048, 2592 },
	{ 853378144, 11718304, 9376 },
	{ 853387520, 11727712, 4448 },
	{ 853391968, 11732192, 22048 },
	{ 853414176, 21516672, 1280 },
	{ 853415456, 11754272, 10016 },
	{ 853425472, 11764320, 16256 },
	{ 855431872, 18509792, 128 },
	{ 853441728, 18521152, 288 },
	{ 853442016, 18531680, 288 },
	{ 853443392, 8080800, 896 },
	{ 853444288, 11425376, 1984 },
	{ 853446272, 11780608, 1856 },
	{ 853448128, 14672288, 800 },
	{ 853448928, 11782496, 1984 },
	{ 853450912, 10096928, 1696 },
	{ 853472320, 14673472, 960 },
	{ 853473280, 14596192, 896 },
	{ 853474176, 12717632, 1376 },
	{ 853475552, 13925184, 1600 },
	{ 853502624, 14674560, 448 },
	{ 853503072, 9171104, 480 },
	{ 853503552, 9171712, 480 },
	{ 853504032, 9065440, 512 },
	{ 853507712, 9077472, 448 },
	{ 853511456, 11784512, 14336 },
	{ 853525792, 9160096, 960 },
	{ 853526752, 11798880, 33824 },
	{ 853560576, 11832736, 2016 },
	{ 853562592, 11834784, 34688 },
	{ 853597280, 10964672, 864 },
	{ 853598144, 10965664, 1024 },
	{ 853599168, 11869504, 4160 },
	{ 853603328, 11873696, 2976 },
	{ 853606304, 10967072, 960 },
	{ 853607264, 10981952, 1024 },
	{ 853608288, 11003104, 480 },
	{ 853608768, 11003712, 960 },
	{ 853609728, 10957856, 864 },
	{ 853610592, 11876704, 3616 },
	{ 853614208, 10958976, 928 },
	{ 853615136, 10960032, 704 },
	{ 853615840, 11880352, 39072 },
	{ 853654912, 13949536, 1600 },
	{ 853656512, 10961120, 480 },
	{ 853656992, 10972800, 704 },
	{ 853657696, 10177312, 1056 },
	{ 853660288, 13512960, 992 },
	{ 853661280, 11921024, 8288 },
	{ 853669568, 13514208, 448 },
	{ 853670016, 11929344, 7520 },
	{ 853677536, 13514912, 448 },
	{ 853690656, 13515680, 992 },
	{ 853691648, 13516800, 544 },
	{ 853692192, 11936896, 7968 },
	{ 853700160, 11944896, 6048 },
	{ 853706816, 11950976, 1664 },
	{ 853708480, 11952672, 4768 },
	{ 853713248, 10587680, 1440 },
	{ 853714688, 13518112, 736 },
	{ 853715424, 11957472, 7264 },
	{ 853722688, 11964768, 1408 },
	{ 853724096, 13519232, 1024 },
	{ 853725120, 11966208, 4608 },
	{ 853729728, 13520512, 448 },
	{ 853730176, 13521088, 864 },
	{ 853731040, 13522080, 896 },
	{ 853731936, 13523104, 576 },
	{ 853782112, 11970848, 9312 },
	{ 853732512, 13523936, 576 },
	{ 853791424, 11980192, 10592 },
	{ 853802016, 12038240, 38336 },
	{ 853840352, 13524896, 640 },
	{ 853840992, 11990816, 9344 },
	{ 853850336, 12076608, 42976 },
	{ 853893312, 13526080, 160 },
	{ 853778720, 12000192, 1760 },
	{ 853902176, 12001984, 8608 },
	{ 853910784, 13526624, 128 },
	{ 853910912, 13527040, 896 },
	{ 853911808, 13529344, 736 },
	{ 853912544, 13530368, 544 },
	{ 853913088, 13531040, 704 },
	{ 853913792, 13532288, 736 },
	{ 853914528, 12010624, 1504 },
	{ 853916032, 20190752, 1056 },
	{ 853917088, 13533568, 928 },
	{ 853918016, 13534848, 736 },
	{ 853918752, 13535712, 384 },
	{ 853919136, 13537440, 1024 },
	{ 853780480, 13538720, 768 },
	{ 853781248, 13539616, 864 },
	{ 854382336, 13540608, 32 },
	{ 854382368, 13541280, 480 },
	{ 854617440, 13541888, 32 },
	{ 854617472, 13543264, 832 },
	{ 854613024, 13544224, 160 },
	{ 854613184, 12012160, 1792 },
	{ 854477728, 13544640, 416 },
	{ 854478144, 12119616, 2080 },
	{ 854545440, 13545664, 352 },
	{ 854545792, 12121728, 1888 },
	{ 854548320, 13546272, 128 },
	{ 854548448, 12123648, 1408 },
	{ 854610976, 13547424, 288 },
	{ 854611264, 12125088, 1760 },
	{ 854547680, 13548608, 32 },
	{ 854547712, 13552576, 608 },
	{ 854382848, 13553312, 32 },
	{ 725972192, 8488128, 36576 },
	{ 727490080, 8561312, 36576 },
	{ 854382880, 13553472, 416 },
	{ 854543328, 13554016, 352 },
	{ 854543680, 12126880, 1760 },
	{ 854362720, 13554624, 32 },
	{ 854362752, 13554784, 320 },
	{ 854364064, 13555232, 32 },
	{ 854364096, 13555392, 320 },
	{ 854365344, 13555840, 32 },
	{ 854365376, 13556384, 320 },
	{ 854366656, 13556832, 32 },
	{ 854366688, 13558496, 320 },
	{ 854657792, 12128672, 15904 },
	{ 854673696, 13559072, 160 },
	{ 854673856, 12144608, 2656 },
	{ 854676512, 12147296, 1696 },
	{ 854678208, 13571072, 992 },
	{ 854679200, 13573568, 768 },
	{ 854682816, 13577728, 832 },
	{ 854683648, 13580224, 800 },
	{ 854684448, 12149024, 1536 },
	{ 854685984, 13456864, 608 },
	{ 854686592, 13471680, 576 },
	{ 854687168, 13487040, 768 },
	{ 854687936, 13487936, 928 },
	{ 854688864, 12150592, 1440 },
	{ 854690304, 13492704, 704 },
	{ 854691008, 13493536, 448 },
	{ 854691456, 12152064, 1440 },
	{ 854692896, 13495328, 448 },
	{ 854693344, 13496512, 576 },
	{ 854693920, 13497696, 544 },
	{ 854696512, 13505152, 640 },
	{ 854697152, 13509696, 640 },
	{ 854699712, 12155616, 3616 },
	{ 854703328, 12159264, 1504 },
	{ 854704832, 13569216, 800 },
	{ 854705632, 18588704, 864 },
	{ 854706496, 12160800, 1888 },
	{ 854709536, 12162720, 1536 },
	{ 854711072, 18590752, 128 },
	{ 854711200, 18598208, 224 },
	{ 854711424, 18510048, 160 },
	{ 854713824, 18669088, 224 },
	{ 854845280, 18669440, 448 },
	{ 854845728, 18670016, 800 },
	{ 854846528, 18670944, 736 },
	{ 854847264, 18674368, 832 },
	{ 854848096, 18675328, 608 },
	{ 854848704, 18676192, 576 },
	{ 854849280, 18676896, 672 },
	{ 854849952, 18677696, 736 },
	{ 854850688, 18678560, 736 },
	{ 854851424, 18679424, 768 },
	{ 854852192, 18680320, 640 },
	{ 854852832, 12165568, 2432 },
	{ 854855904, 12168032, 1984 },
	{ 854857888, 12170048, 2592 },
	{ 854860480, 12172672, 1696 },
	{ 854862176, 18682304, 32 },
	{ 854862208, 18682464, 160 },
	{ 854863488, 18682752, 992 },
	{ 854864480, 18683872, 896 },
	{ 854865376, 12174400, 1056 },
	{ 854866432, 18685024, 256 },
	{ 854866688, 18685408, 992 },
	{ 854867680, 18686528, 160 },
	{ 854867840, 18686816, 832 },
	{ 854868672, 18687776, 1024 },
	{ 854869696, 18688928, 512 },
	{ 854870208, 18689568, 672 },
	{ 854870880, 12175488, 2336 },
	{ 854873216, 12177856, 1952 },
	{ 854875168, 18690624, 32 },
	{ 854875200, 18690784, 160 },
	{ 854875360, 12179840, 1504 },
	{ 854876864, 18691200, 192 },
	{ 854877056, 18691520, 992 },
	{ 854878048, 18692640, 160 },
	{ 854878208, 12181376, 2112 },
	{ 854880320, 18693056, 96 },
	{ 854880416, 12183520, 4288 },
	{ 854884704, 12187840, 3296 },
	{ 854888000, 18693536, 32 },
	{ 854888032, 18693696, 192 },
	{ 854888224, 18694016, 480 },
	{ 854888704, 18694624, 480 },
	{ 854889184, 18695232, 256 },
	{ 854889440, 18695616, 160 },
	{ 854889600, 18695904, 736 },
	{ 854890336, 12191168, 10368 },
	{ 854900704, 18696896, 64 },
	{ 854900768, 12201568, 8704 },
	{ 854909472, 18697216, 32 },
	{ 854909504, 18697376, 160 },
	{ 854909664, 18697664, 800 },
	{ 854910464, 18698592, 864 },
	{ 854911328, 18699584, 192 },
	{ 854911520, 18699904, 128 },
	{ 854911648, 18700160, 160 },
	{ 854911968, 12210304, 2240 },
	{ 854914208, 18700576, 576 },
	{ 854914784, 12212576, 2240 },
	{ 854917024, 12214848, 2592 },
	{ 854919616, 12217472, 2688 },
	{ 854922304, 12220192, 1632 },
	{ 854923936, 18701792, 32 },
	{ 854923968, 18701952, 160 },
	{ 854924128, 12221856, 1440 },
	{ 854925568, 18702368, 192 },
	{ 854925760, 18702688, 992 },
	{ 854926752, 18703808, 160 },
	{ 855432000, 13537216, 96 },
	{ 855432096, 19691200, 7328 },
	{ 855439424, 12038240, 65184 },
	{ 855504608, 18804064, 65312 },
	{ 855569920, 18869408, 65536 },
	{ 855635456, 14130816, 65120 },
	{ 855700576, 14195968, 65184 },
	{ 855765760, 14261184, 65280 },
	{ 855831040, 14326496, 65024 },
	{ 855896064, 21162496, 61984 },
	{ 855958048, 21224512, 62816 },
	{ 856020864, 21287360, 62880 },
	{ 856083744, 21350272, 64544 },
	{ 856148288, 11953472, 56736 },
	{ 856205024, 21414848, 60544 },
	{ 856265568, 11552192, 62336 },
	{ 856327904, 11614560, 65312 },
	{ 856393216, 11679904, 59168 },
	{ 856452384, 11739104, 64192 },
	{ 856516576, 11803328, 65504 },
	{ 856582080, 12204864, 58816 },
	{ 856640896, 12263712, 61408 },
	{ 856702304, 12325152, 55840 },
	{ 856758144, 12381024, 61856 },
	{ 856820000, 12442912, 53920 },
	{ 856873920, 12103456, 46752 },
	{ 856920672, 20328192, 275520 },
	{ 857196192, 13824064, 1728 },
	{ 854964448, 18583168, 160 },
	{ 854964736, 18595968, 160 },
	{ 854965088, 19476640, 160 },
	{ 854965440, 19477376, 160 },
	{ 854971456, 19477920, 160 },
	{ 855034240, 19478464, 160 },
	{ 855120352, 19479008, 160 },
	{ 855120800, 19479552, 160 },
	{ 855121152, 19407264, 160 },
	{ 855121504, 9464032, 160 },
	{ 855121856, 9464576, 160 },
	{ 855122208, 9465120, 160 },
	{ 855158272, 9465664, 160 },
	{ 855189184, 9466464, 160 },
	{ 855248064, 21082496, 160 },
	{ 855295232, 21088384, 160 },
	{ 855312992, 21097152, 160 },
	{ 855323488, 21113312, 160 },
	{ 855339360, 21119424, 160 },
	{ 855349792, 21125344, 160 },
	{ 855375136, 21031776, 160 },
	{ 855431712, 18644576, 160 },
	{ 726008768, 8524704, 36576 },
	{ 727526656, 8597888, 36576 },
	{ 897528320, 12683168, 1856 },
	{ 854926912, 16673184, 96 },
	{ 719838880, 5925152, 96 },
	{ 717159232, 5924960, 96 },
	{ 717159328, 8488128, 36576 },
	{ 719838976, 8561312, 36576 },
	{ 717195904, 8524704, 36576 },
	{ 719875552, 8597888, 36576 },
	{ 717232480, 8488128, 36576 },
	{ 719912128, 8561312, 36576 },
	{ 897512064, 16829760, 5024 },
	{ 897518240, 15332448, 2624 },
	{ 850798496, 16752192, 96 },
	{ 717269056, 8524704, 36576 },
	{ 719948704, 8597888, 36576 },
	{ 727124224, 5925152, 96 },
	{ 725606336, 5924960, 96 },
	{ 725606432, 8488128, 36576 },
	{ 727124320, 8561312, 36576 },
	{ 725643008, 8524704, 36576 },
	{ 727160896, 8597888, 36576 },
	{ 841622080, 20956928, 15264 },
	{ 841708256, 11425152, 1216 },
	{ 849894880, 8997824, 8800 },
	{ 849903680, 11464576, 36832 },
	{ 842091744, 16733152, 640 },
	{ 849940512, 16733920, 352 },
	{ 842583392, 10128544, 1760 },
	{ 849940864, 9006656, 2016 },
	{ 842684480, 10587680, 1248 },
	{ 842685728, 16734784, 96 },
	{ 842688160, 21020736, 4416 },
	{ 725679584, 8488128, 36576 },
	{ 727197472, 8561312, 36576 },
	{ 849942880, 20194016, 2304 },
	{ 849945184, 16735264, 352 },
	{ 849945536, 12652384, 24800 },
	{ 850303744, 16735872, 128 },
	{ 843238304, 16736128, 544 },
	{ 843300576, 12717536, 1280 },
	{ 843301856, 16736928, 672 },
	{ 843302528, 16737728, 256 },
	{ 843302784, 16738112, 32 },
	{ 843302816, 16738272, 160 },
	{ 843302976, 16738560, 160 },
	{ 849970336, 15773664, 33824 },
	{ 850004160, 16604576, 34688 },
	{ 850038848, 16739104, 672 },
	{ 850039520, 16739904, 32 },
	{ 850039552, 16740064, 192 },
	{ 850040160, 20199424, 1440 },
	{ 850039744, 16740512, 416 },
	{ 850041600, 16741056, 224 },
	{ 850041824, 16741408, 864 },
	{ 850042688, 16742400, 32 },
	{ 850042720, 16742560, 192 },
	{ 850042912, 16742880, 160 },
	{ 850043072, 16743168, 768 },
	{ 850043840, 15885376, 39072 },
	{ 850082912, 10096928, 1600 },
	{ 850084512, 16744320, 32 },
	{ 850084544, 16744480, 192 },
	{ 850084736, 19252480, 1600 },
	{ 850086336, 16744928, 160 },
	{ 850095584, 11501440, 6112 },
	{ 850101696, 16745344, 32 },
	{ 850101728, 16745504, 160 },
	{ 850101888, 15811040, 2560 },
	{ 850104448, 16745920, 160 },
	{ 850104608, 16746208, 960 },
	{ 850105568, 9280192, 7968 },
	{ 850113536, 9787584, 6048 },
	{ 850120192, 13949184, 1664 },
	{ 850121856, 9793664, 4768 },
	{ 850126624, 20204160, 1440 },
	{ 850128064, 16747936, 736 },
	{ 850128800, 16748864, 992 },
	{ 850129792, 10740192, 8288 },
	{ 850138080, 16750112, 448 },
	{ 850138528, 9231424, 7520 },
	{ 850146048, 16750816, 448 },
	{ 850159168, 16751392, 992 },
	{ 850160160, 16752512, 544 },
	{ 850160704, 15310112, 18688 },
	{ 850179392, 18804064, 36320 },
	{ 850220128, 16753440, 800 },
	{ 850220928, 10077536, 4320 },
	{ 850225248, 16754496, 448 },
	{ 850233056, 16755776, 160 },
	{ 850303872, 16733440, 96 },
	{ 850303968, 10476704, 2752 },
	{ 850306720, 15885376, 65536 },
	{ 850372256, 18804064, 65472 },
	{ 850437728, 15811040, 55200 },
	{ 850492928, 16818720, 15904 },
	{ 850302528, 16753376, 160 },
	{ 850302816, 16670752, 160 },
	{ 850303296, 13542048, 160 },
	{ 850303584, 18599392, 160 },
	{ 897522976, 20953408, 2112 },
	{ 850302464, 18512704, 64 },
	{ 725716160, 8524704, 36576 },
	{ 727234048, 8597888, 36576 },
	{ 841594848, 18799584, 4160 },
	{ 841599008, 18937120, 2112 },
	{ 841601120, 8800704, 2304 },
	{ 841603424, 19605792, 18656 },
	{ 841637344, 12705088, 7168 },
	{ 841644512, 19629696, 18816 },
	{ 841665120, 20046528, 2080 },
	{ 841667200, 18528064, 992 },
	{ 841668192, 20873600, 18592 },
	{ 841686784, 11423680, 21472 },
	{ 841709472, 13961344, 36128 },
	{ 841745600, 18784800, 12960 },
	{ 841758560, 18638432, 2208 },
	{ 841760768, 19988608, 34592 },
	{ 841795360, 20786016, 21632 },
	{ 841816992, 11464576, 32736 },
	{ 841884544, 8685664, 1408 },
	{ 841885952, 19789472, 9184 },
	{ 841898976, 15950944, 2208 },
	{ 841973056, 20807680, 17856 },
	{ 841990912, 20971808, 15328 },
	{ 842006240, 20987168, 32672 },
	{ 842038912, 19238592, 7744 },
	{ 842046656, 19624480, 1088 },
	{ 842047744, 13833312, 19968 },
	{ 842071744, 13853312, 20000 },
	{ 842134496, 15882272, 2272 },
	{ 842136768, 18776512, 4448 },
	{ 842141216, 13873344, 16992 },
	{ 842158208, 12021760, 5600 },
	{ 842163808, 20205600, 21824 },
	{ 842185632, 20227456, 21408 },
	{ 842213568, 20189440, 1184 },
	{ 725752736, 8488128, 36576 },
	{ 727270624, 8561312, 36576 },
	{ 842214752, 19689152, 8416 },
	{ 842223168, 14019328, 28608 },
	{ 842251776, 20248896, 20160 },
	{ 842271936, 19667936, 12416 },
	{ 842284352, 18532896, 416 },
	{ 842284768, 11497344, 8448 },
	{ 842297888, 11505824, 2336 },
	{ 842300224, 18533696, 160 },
	{ 842300384, 13890368, 4768 },
	{ 842305152, 14047968, 21792 },
	{ 842326944, 16807648, 8256 },
	{ 842337120, 14069792, 34016 },
	{ 842371136, 18780992, 2272 },
	{ 842373408, 19493440, 29120 },
	{ 842402528, 19697600, 2272 },
	{ 842404800, 19522592, 16800 },
	{ 842421600, 19648544, 7168 },
	{ 842428768, 19539424, 35808 },
	{ 842464576, 18535264, 128 },
	{ 842464704, 19575264, 20256 },
	{ 842498144, 9241120, 1280 },
	{ 842499424, 14442976, 18048 },
	{ 842517472, 19595552, 4288 },
	{ 842521760, 14461056, 34016 },
	{ 842556160, 16815936, 2496 },
	{ 842558656, 14495104, 18176 },
	{ 842576832, 10217376, 6560 },
	{ 842585152, 14103840, 15104 },
	{ 842600256, 18536672, 1024 },
	{ 842603360, 14513312, 21440 },
	{ 842666624, 18537952, 224 },
	{ 842666848, 18538304, 192 },
	{ 842667040, 14534784, 17440 },
	{ 842685824, 20269088, 2336 },
	{ 842692576, 11266016, 18880 },
	{ 842711456, 14559616, 1856 },
	{ 842713312, 11284928, 16000 },
	{ 842729312, 11300960, 32128 },
	{ 842765408, 20186816, 1120 },
	{ 842771584, 11333120, 15968 },
	{ 842787552, 11349120, 16032 },
	{ 842803584, 11365184, 18336 },
	{ 842821920, 10074240, 3200 },
	{ 842825120, 14552256, 4480 },
	{ 842829600, 13603968, 20384 },
	{ 842849984, 11383552, 8640 },
	{ 842858624, 12027392, 2112 },
	{ 842860736, 13624384, 31936 },
	{ 842892672, 11445184, 12352 },
	{ 842905024, 18540800, 672 },
	{ 842905696, 13656352, 19168 },
	{ 842924864, 10223968, 4608 },
	{ 842929472, 13675552, 33888 },
	{ 842963360, 18541984, 992 },
	{ 842964352, 13709472, 36128 },
	{ 843005600, 18543232, 640 },
	{ 843006240, 18544000, 160 },
	{ 843006400, 18549024, 192 },
	{ 843006592, 13745632, 20096 },
	{ 843026688, 14130816, 36352 },
	{ 843063040, 9599424, 2240 },
	{ 843065280, 14167200, 33696 },
	{ 843098976, 14433984, 1856 },
	{ 843100832, 13765760, 17216 },
	{ 843118048, 19680384, 1536 },
	{ 847519744, 18550240, 128 },
	{ 843000480, 20892224, 5120 },
	{ 843244320, 19655744, 2720 },
	{ 843247040, 11457568, 1600 },
	{ 843249536, 18550880, 864 },
	{ 843269792, 20825568, 2272 },
	{ 843272064, 18552000, 800 },
	{ 843275968, 11392224, 2048 },
	{ 843278016, 18553056, 800 },
	{ 843283424, 18553984, 576 },
	{ 843303136, 18554688, 992 },
	{ 843306112, 20827872, 1632 },
	{ 843307744, 20897376, 3424 },
	{ 843311168, 19801120, 7200 },
	{ 843318368, 14200928, 26688 },
	{ 843345056, 19658496, 1344 },
	{ 843346400, 14227648, 17056 },
	{ 843363456, 14244736, 31360 },
	{ 843394816, 20900832, 4448 },
	{ 843399264, 14276128, 20832 },
	{ 843420096, 20326752, 152960 },
	{ 843573056, 18557088, 128 },
	{ 843573184, 19808352, 2336 },
	{ 843577440, 18557472, 864 },
	{ 843578304, 11394304, 2048 },
	{ 843580352, 19810720, 1888 },
	{ 843582240, 18558720, 704 },
	{ 843582944, 18559552, 896 },
	{ 843583840, 13783008, 8128 },
	{ 843591968, 9601696, 1088 },
	{ 843593056, 18560928, 416 },
	{ 843593472, 18563520, 992 },
	{ 843594464, 19812640, 1888 },
	{ 843596352, 18567072, 768 },
	{ 843597120, 18567968, 864 },
	{ 843597984, 18568960, 768 },
	{ 843598752, 13791168, 3616 },
	{ 843602368, 18569984, 832 },
	{ 843603200, 18570944, 896 },
	{ 843604096, 13794816, 1888 },
	{ 843605984, 14296992, 1888 },
	{ 843609120, 18572224, 704 },
	{ 843611808, 14298912, 1632 },
	{ 843613440, 18573184, 832 },
	{ 843614272, 18574144, 960 },
	{ 843615232, 14300576, 1888 },
	{ 843617120, 14302496, 1888 },
	{ 843619008, 14304416, 1888 },
	{ 843645600, 18619904, 672 },
	{ 843646272, 14306336, 3712 },
	{ 843683584, 14310080, 7904 },
	{ 843691488, 14318016, 24864 },
	{ 843654976, 18621088, 960 },
	{ 843655936, 18630848, 480 },
	{ 843716352, 18631456, 608 },
	{ 843716960, 14342912, 1664 },
	{ 843620896, 18504512, 704 },
	{ 843621600, 14344608, 2400 },
	{ 843656416, 14347040, 3520 },
	{ 843659936, 14350592, 8672 },
	{ 843624000, 14001728, 1408 },
	{ 843625408, 14359296, 4768 },
	{ 843630176, 18506880, 960 },
	{ 843631136, 18507968, 640 },
	{ 843668608, 14364096, 1600 },
	{ 843670208, 14365728, 4928 },
	{ 843676384, 14370688, 2496 },
	{ 843678880, 14373216, 4704 },
	{ 843675136, 18509248, 608 },
	{ 725789312, 8524704, 36576 },
	{ 727307200, 8597888, 36576 },
	{ 843675744, 18509984, 640 },
	{ 843631776, 18510752, 960 },
	{ 843632736, 14377952, 3776 },
	{ 843636512, 18511968, 896 },
	{ 843637408, 14381760, 3872 },
	{ 843641280, 18515072, 608 },
	{ 843641888, 14385664, 3712 },
	{ 843650816, 18515936, 608 },
	{ 843651424, 14389408, 3552 },
	{ 843718624, 19814560, 1184 },
	{ 844065952, 18517600, 320 },
	{ 844123424, 14392992, 5088 },
	{ 844035072, 14398112, 2016 },
	{ 844066272, 20479744, 27200 },
	{ 845273120, 18518528, 320 },
	{ 845273440, 18518976, 32 },
	{ 843989312, 14400160, 2528 },
	{ 846017792, 18519264, 160 },
	{ 846017952, 20506976, 39168 },
	{ 846057120, 18519680, 256 },
	{ 846057376, 20546176, 14080 },
	{ 846071456, 18520192, 96 },
	{ 846071552, 20560288, 11872 },
	{ 846083424, 18520544, 448 },
	{ 846083872, 20572192, 31616 },
	{ 846115488, 20905312, 1088 },
	{ 846118816, 13549568, 224 },
	{ 846119040, 13549920, 160 },
	{ 846119200, 13550848, 576 },
	{ 846119776, 13551552, 224 },
	{ 846129280, 13555648, 576 },
	{ 846129856, 13559232, 992 },
	{ 846130848, 13564736, 416 },
	{ 846131872, 13565952, 160 },
	{ 846132032, 13566240, 160 },
	{ 846132192, 13566528, 224 },
	{ 846134496, 13568480, 288 },
	{ 846134784, 13568896, 192 },
	{ 846386944, 13570016, 32 },
	{ 846386976, 20603840, 10240 },
	{ 846397216, 13570304, 416 },
	{ 846397632, 13572160, 576 },
	{ 846398208, 13572864, 160 },
	{ 846398368, 14407424, 1216 },
	{ 846399584, 14408672, 1216 },
	{ 846400800, 14409920, 1216 },
	{ 846403104, 13574464, 736 },
	{ 846403840, 20614112, 2048 },
	{ 846405888, 20616192, 1216 },
	{ 846407104, 20617440, 1216 },
	{ 846408320, 13576224, 736 },
	{ 846409056, 20618688, 2048 },
	{ 847094752, 13577216, 288 },
	{ 847388448, 13578592, 160 },
	{ 847389440, 13579776, 224 },
	{ 843119584, 20620768, 37664 },
	{ 843157248, 20658464, 2528 },
	{ 843159776, 20661024, 35648 },
	{ 843195424, 13581280, 96 },
	{ 843195520, 20696704, 4096 },
	{ 843199616, 20023232, 1056 },
	{ 843200672, 20700832, 2880 },
	{ 843203552, 13581888, 768 },
	{ 843204320, 20703744, 1120 },
	{ 843205440, 20704896, 4896 },
	{ 843210336, 20709824, 1440 },
	{ 843211776, 20711296, 1440 },
	{ 843213216, 20712768, 1440 },
	{ 843214656, 20714240, 1504 },
	{ 843216160, 20715776, 1600 },
	{ 843217760, 20717408, 1568 },
	{ 843219328, 20719008, 2048 },
	{ 843221376, 20721088, 1856 },
	{ 843223232, 20722976, 1856 },
	{ 843225088, 20724864, 1984 },
	{ 843227072, 20726880, 1632 },
	{ 843228704, 20728544, 1952 },
	{ 843230656, 20730528, 1920 },
	{ 843232576, 20732480, 1856 },
	{ 843234432, 20734368, 1920 },
	{ 843236352, 20736320, 1952 },
	{ 843238848, 13949280, 1056 },
	{ 843239904, 20738304, 1408 },
	{ 843241312, 13586272, 672 },
	{ 843241984, 13587072, 384 },
	{ 843242368, 20739744, 1088 },
	{ 843243456, 13587712, 160 },
	{ 843243616, 13456928, 704 },
	{ 843248640, 13457760, 896 },
	{ 843250400, 13458784, 800 },
	{ 843272864, 11552192, 2304 },
	{ 843275168, 13459840, 800 },
	{ 843278816, 11554528, 2272 },
	{ 843281088, 11556832, 1728 },
	{ 843282816, 13461024, 608 },
	{ 843284000, 13461760, 960 },
	{ 843284960, 20740864, 1216 },
	{ 843286176, 11558592, 1440 },
	{ 843287616, 11560064, 1888 },
	{ 843289504, 11561984, 1056 },
	{ 843290560, 11563072, 1472 },
	{ 843292032, 13463488, 928 },
	{ 843292960, 11564576, 1888 },
	{ 843294848, 11566496, 1920 },
	{ 843296768, 11568448, 1952 },
	{ 843299840, 13464928, 736 },
	{ 847386656, 13465792, 832 },
	{ 847387488, 13466752, 960 },
	{ 847519872, 13458144, 96 },
	{ 847519968, 11423680, 21760 },
	{ 847541728, 14442976, 65536 },
	{ 847607264, 11266016, 65088 },
	{ 847672352, 11331136, 65184 },
	{ 847737536, 19493440, 64896 },
	{ 847802432, 19558368, 64896 },
	{ 847867328, 16438336, 65344 },
	{ 847932672, 16503712, 65440 },
	{ 847998112, 13603968, 65472 },
	{ 848063584, 13669472, 65408 },
	{ 848128992, 14130816, 64832 },
	{ 848193824, 14195680, 65152 },
	{ 848258976, 14260864, 65376 },
	{ 848324352, 14326272, 65376 },
	{ 848389728, 20326752, 62592 },
	{ 848452320, 20389376, 62368 },
	{ 848514688, 20451776, 65536 },
	{ 848580224, 20517344, 65312 },
	{ 848645536, 20582688, 64768 },
	{ 848710304, 20647488, 65504 },
	{ 848775808, 11541952, 64672 },
	{ 848840480, 11606656, 64704 },
	{ 848905184, 11671392, 65152 },
	{ 848970336, 13734912, 61344 },
	{ 849031680, 11736576, 64640 },
	{ 849096320, 11801248, 65536 },
	{ 849161856, 11866816, 64992 },
	{ 849226848, 11931840, 64160 },
	{ 849291008, 12038240, 65472 },
	{ 725825888, 8488128, 36576 },
	{ 727343776, 8561312, 36576 },
	{ 849356480, 12103744, 65280 },
	{ 849421760, 12169056, 64320 },
	{ 849486080, 11445472, 8064 },
	{ 849494144, 13833312, 58720 },
	{ 849552864, 12233408, 161376 },
	{ 849714240, 12394816, 174176 },
	{ 849888416, 19671328, 6464 },
	{ 847427296, 18654720, 160 },
	{ 847427552, 18676192, 160 },
	{ 847427840, 9417568, 160 },
	{ 847428384, 9447104, 160 },
	{ 847428896, 9448128, 160 },
	{ 847429408, 9452256, 160 },
	{ 847429824, 9453376, 160 },
	{ 847517920, 21030432, 160 },
	{ 847518240, 21030976, 160 },
	{ 847518624, 21032288, 160 },
	{ 847519040, 21032832, 160 },
	{ 847519296, 21036096, 160 },
	{ 847519584, 21036640, 160 },
	{ 844448704, 14130816, 205536 },
	{ 844654240, 21039552, 448 },
	{ 897479200, 17866528, 30784 },
	{ 865780928, 17897344, 29568 },
	{ 865697024, 14130816, 27168 },
	{ 897443808, 17866528, 26752 },
	{ 897298208, 14249728, 17344 },
	{ 897443296, 21096224, 512 },
	{ 897317248, 21208704, 126048 },
	{ 897316736, 21096992, 512 },
	{ 897279872, 14267104, 18336 },
	{ 897270944, 14285472, 8928 },
	{ 897315552, 21097888, 512 },
	{ 897316224, 21098528, 512 },
	{ 897316064, 21099168, 160 },
	{ 897470560, 14294432, 8640 },
	{ 865780928, 21296288, 29568 },
	{ 865697024, 14174592, 27168 },
	{ 865753344, 14293600, 27456 },
	{ 865197344, 18497088, 2048 },
	{ 865354208, 21325888, 4736 },
	{ 865780800, 16747424, 128 },
	{ 865724192, 21378592, 28864 },
	{ 865068960, 16753376, 320 },
	{ 865392576, 14098592, 4288 },
	{ 865753056, 16755200, 288 },
	{ 865455392, 17921376, 4448 },
	{ 725862464, 8524704, 36576 },
	{ 727380352, 8597888, 36576 },
	{ 847389664, 16769152, 96 },
};

#include "gc_dvd.h"
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>
#define HW_REG_BASE     0xcd800000
#define HW_ARMIRQMASK   (HW_REG_BASE + 0x03c)
#define HW_ARMIRQFLAG   (HW_REG_BASE + 0x038) 

int have_hw_access() {
	if (read32(HW_ARMIRQMASK) && read32(HW_ARMIRQFLAG)) {
		// disable DVD irq for starlet
		mask32(HW_ARMIRQMASK, 1 << 18, 0);
		network_printf("AHBPROT access OK\r\n");
		return 1;
	}
	return 0;
}


void DVDTest() {
	VIDEO_Init();
	have_hw_access();
	init_dvd();
	int max_size = 0;
	for (unsigned i = 0; i < sizeof(testvecs) / sizeof(*testvecs); ++i) {
		if (max_size < testvecs[i][2])
			max_size = testvecs[i][2];
	}
	void *buf = malloc(max_size);

	// Make sure DVD is spinning/ready/etc.
	DVD_LowRead64(buf, 2048, 0);

	u64 prev_tv = gettime();
	u64 start_tv = prev_tv;
	int prev_offset = 0;
	for (unsigned i = 0; i < sizeof(testvecs) / sizeof(*testvecs) / 10; ++i) {
		int offset = testvecs[i][0];
		int size = testvecs[i][2];
		offset &= ~2047;
		size = (size + 2047) & ~2047;

		//offset = (i & 7) * 16384 * 1024 * 16 + 1048576 * 17;
		//size = 16384 * 2;

		DVD_LowRead64(buf, size, offset);
		u64 cur_tv = gettime();
		int diff = diff_usec(prev_tv, cur_tv);
		network_printf("%d\t%d\n", abs(offset - prev_offset), diff);
		while (gettime() < cur_tv + 20000000) {
			LWP_YieldThread();
		}
		prev_tv = gettime();
		prev_offset = offset;
	}
	network_printf("TT: %d\n", diff_usec(start_tv, prev_tv) / 1000);
}

int main()
{
	network_init();
	WPAD_Init();

	GXTest::Init();

	//BitfieldTest();
	//TevCombinerTest();
	//ClipTest();
	//TestDepth();
	DVDTest();

	network_printf("Shutting down...\n");
	network_shutdown();

	return 0;
}
