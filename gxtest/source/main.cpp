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

	float testvals[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 16748210.0f / 16777216,
						29006.0f / 16777216, 29007.0f / 16777216, 16748209.0f / 16777216,
						13822720.0f / 16777216, 8388609.0f / 16777216, 8388611.0f / 16777216,
						1.0f / 16777216, 16777215.0f / 16777216, 8388610.0f / 16777216,
						8388607.0f / 16777216/**/, 8388606.0f / 16777216/**/,

						// Random
						12247796.0f / 16777216,
						488857.0f / 16777216,
						14712663.0f / 16777216,
						12827623.0f / 16777216,
						2608669.0f / 16777216,
						6858126.0f / 16777216,/*23*/
						7466542.0f / 16777216,/*24*/
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
#if 0
	int actualvals[] = {
		1,
		12582911,
		8388608,
		4194305,
		1,
		29007,
		16748209,
		16748208,
		29008,
		2954497,
		8388607,
		8388606,
		16777214,
		2,
		8388607,
		8388609,
		8388610,
		4529421,
		16288358,
		2064554,
		3949594,
		14168546,
		9919090, /*23*/
		9310674, /*24*/
		2655576,
		14293329,
		15380860};
#endif
	for (int step = 0; step < int(sizeof(testvals) / sizeof(float)); ++step)
	//for (int step = 0; step < 2000; ++step)
	{
		auto zmode = CGXDefault<ZMode>();
		zmode.testenable = true;
		zmode.updateenable = true;
		zmode.func = COMPARE_ALWAYS;
		CGX_LOAD_BP_REG(zmode.hex);

		auto cc = CGXDefault<TevStageCombiner::ColorCombiner>(0);

#if 1
#define TESTPOWER 16
		int viewdividefactor = (1 << (16 - TESTPOWER));
		// Now, enable testing viewport and draw the (red) testing quad
		TestDepth_SetViewport(100.0f, 0.0f, 50.0f, 50.0f, double(0x1000000 / viewdividefactor), double(0x1000000 / viewdividefactor));
#else
#define TESTTWOTOSIXTEENMINUSONE 1
//#define TESTTWOTOSIXTEENMINUSTWOTOFOURTEEN 1
//#define TESTTWOTOFIFTEENMINUSTWOTOTHIRTEEN 1
		// Now, enable testing viewport and draw the (red) testing quad
		TestDepth_SetViewport(100.0f, 0.0f, 50.0f, 50.0f, double(0xFFFFFF), double(0xFFFFFF));
#endif
		cc.d = TEVCOLORARG_C0;
		CGX_LOAD_BP_REG(cc.hex);

		auto tevreg = CGXDefault<TevReg>(1, false); // c0
		tevreg.red = 0xff;
		CGX_LOAD_BP_REG(tevreg.low);
		CGX_LOAD_BP_REG(tevreg.high);

		CGX_BEGIN_LOAD_XF_REGS(0x1005, 1);
		wgPipe->U32 = 0; // 0 = enable clipping, 1 = disable clipping

		//bool expect_quad_to_be_drawn = true;
		int test_x = 125, test_y = 25; // Somewhere within the viewport
		GXTest::Quad test_quad;
		test_quad.ColorRGBA(0xff, 0, 0, 0xff);
		float testval = testvals[step];
		//float testval = (step + 1 + 0xFFFFFF - 40) / float(0x1000000);
		//float testval = (rand() & 0xFFFFFF) / float(0x1000000);
		test_quad.AtDepth(testval);

		test_quad.Draw();
		GXTest::CopyToTestBuffer(0, 0, 199, 49);
		CGX_WaitForGpuToFinish();

		GXTest::Vec4<u8> result = GXTest::ReadTestBuffer(test_x, test_y, 200);
		int depthval = (result.r << 16) + (result.g << 8) + (result.b << 0);
#ifdef TESTTWOTOSIXTEENMINUSONE
		int guessdepthval = int(testval * 0x1000000);
		if (guessdepthval < 0x400001)
			guessdepthval = 0xFFFFFF - guessdepthval;
		else if (guessdepthval < 0x800002)
			guessdepthval = 0xFFFFFF - guessdepthval + 1;
		else
			guessdepthval = 0xFFFFFF - guessdepthval + 2;
		if (guessdepthval == 0xFFFFFF)
			guessdepthval = 1;
#elif TESTPOWER == 16
		int guessdepthval = int(testval * 0x1000000);
		if (guessdepthval < 0x800001)
			guessdepthval = 0x1000000 - (guessdepthval);
		else
			guessdepthval = 0x1000000 - (guessdepthval - 1);
		if (guessdepthval == 0x1000000)
			guessdepthval = 1;
#elif TESTPOWER == 15
		int guessdepthval = int(testval * 0x1000000);
		if (guessdepthval < 0x800000)
			guessdepthval = 0x800000 - (guessdepthval + 1) / 2;
		else
			guessdepthval = 0x800000 - (guessdepthval) / 2;
		if (guessdepthval == 0x800000)
			guessdepthval = 1;
#elif TESTPOWER == 14
		int guessdepthval = int(testval * 0x1000000);
		if (guessdepthval < 0x800000)
			guessdepthval = 0x400000 - (guessdepthval + 3) / 4;
		else
			guessdepthval = 0x400000 - (guessdepthval + 2) / 4;
		if (guessdepthval == 0x400000)
			guessdepthval = 1;
#elif TESTPOWER == 13
		int guessdepthval = int(testval * 0x1000000);
		if (guessdepthval < 0x800000)
			guessdepthval = 0x200000 - (guessdepthval + 7) / 8;
		else
			guessdepthval = 0x200000 - (guessdepthval + 6) / 8;
		if (guessdepthval == 0x200000)
			guessdepthval = 1;
#elif TESTTWOTOSIXTEENMINUSTWOTOFOURTEEN
		double temp = float(testval) * 0xC00000 - 0xC00000;
		int guessdepthval = int(testval * 0x1000000);
		int input = guessdepthval;
		if (input <= 0x400000)
			guessdepthval = 0xC00000 - (guessdepthval - (guessdepthval + 1) / 4);
		else if (input <= 0x555555)
			guessdepthval = 0xC00000 - (guessdepthval - (guessdepthval + 2) / 4);
		else if (input <= 0x800000)
			guessdepthval = 0xC00000 - (guessdepthval - (guessdepthval + 3) / 4);
		else if (input <= 0xaaaaab)
			guessdepthval = 0xC00000 - (guessdepthval - (guessdepthval + 4) / 4);
		else
			guessdepthval = 0xC00000 - (guessdepthval - (guessdepthval + 6) / 4);
		if (guessdepthval == 0xC00000)
			guessdepthval = 1;
#elif TESTTWOTOFIFTEENMINUSTWOTOTHIRTEEN
		double temp = float(testval) * 0x600000 - 0x600000;
		int guessdepthval = int(testval * 0x1000000);
		int input = guessdepthval;
		if (input <= 0x400000)
			guessdepthval = 0x600000 - (guessdepthval - (guessdepthval * 5 + 1) / 8);
		else if (input <= 0x555555)
			guessdepthval = 0x600000 - (guessdepthval - (guessdepthval * 5 + 2) / 8);
		else if (input <= 0x800000)
			guessdepthval = 0x600000 - (guessdepthval - (guessdepthval * 5 + 3) / 8);
		else if (input <= 0xaaaaab)
			guessdepthval = 0x600000 - (guessdepthval - (guessdepthval * 5 + 4) / 8);
		else
			guessdepthval = 0x600000 - (guessdepthval - (guessdepthval * 5 + 6) / 8);
		if (guessdepthval == 0x600000)
			guessdepthval = 1;
#else
#error "No define?"
#endif
		//depthval = ((*(int*)(&randfloat) & 0x7FFFFF) | 0x800000) >> (126 - (*(int*)(&randfloat) >> 23));
		//depthval = actualvals[step];
		DO_TEST(depthval == guessdepthval, "Subtest %d failed: input %d, guess %d, actual %d\n", step + 1, int(testval * 0x1000000), guessdepthval, depthval);
		//if (depthval != guessdepthval)
		//	break;
		//network_printf("%f\n", temp);
		//network_printf("%10d %10d\n", int(testval * 0x1000000), depthval);
		//printf("r: %d, g: %d, b: %d, a: %d, depth: %u\n", result.r, result.g, result.b, result.a, (result.r << 16) + (result.g << 8) + (result.b << 0));
		//if (step % 1000 == 0) {
		//	network_printf("Step %d Val %f\n", step + 1, -temp);
		//}
		GXTest::DebugDisplayEfbContents();
	}

	END_TEST();
}

int main()
{
	network_init();
	WPAD_Init();

	GXTest::Init();

	//BitfieldTest();
	//TevCombinerTest();
	//ClipTest();
	TestDepth();

	network_printf("Shutting down...\n");
	network_shutdown();

	return 0;
}
