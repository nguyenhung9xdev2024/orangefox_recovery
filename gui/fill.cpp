/*
	Copyright 2017 TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

// fill.cpp - GUIFill object

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include <string>

extern "C" {
#include "../twcommon.h"
}
#include "minuitwrp/minui.h"

#include "rapidxml.hpp"
#include "objects.hpp"

GUIFill::GUIFill(xml_node<>* node) : GUIObject(node)
{
	bool has_color = false;
	mCircle1 = NULL;
	mCircle2 = NULL;
	mCircle3 = NULL;
	mCircle4 = NULL;
	mColor = LoadAttrColor(node, "color", &has_color);
	if (!has_color) {
		LOGERR("No color specified for fill\n");
		return;
	}

	mRadius = LoadAttrIntScaleX(node, "radius");

	// Load the placement
	LoadPlacement(FindNode(node, "placement"), &mRenderX, &mRenderY, &mRenderW, &mRenderH);

	if(mRadius <= -1) {
		mCircle1 = gr_render_antialiased_circle(mRenderH / 2 - 1, mColor.red, mColor.green, mColor.blue, mColor.alpha, 1);
		mCircle2 = gr_render_antialiased_circle(mRenderH / 2 - 1, mColor.red, mColor.green, mColor.blue, mColor.alpha, 2);
	} else if(mRadius > 0) {
		mCircle1 = gr_render_antialiased_circle(mRadius * 2, mColor.red, mColor.green, mColor.blue, mColor.alpha, 3);
		mCircle2 = gr_render_antialiased_circle(mRadius * 2, mColor.red, mColor.green, mColor.blue, mColor.alpha, 4);
		mCircle3 = gr_render_antialiased_circle(mRadius * 2, mColor.red, mColor.green, mColor.blue, mColor.alpha, 5);
		mCircle4 = gr_render_antialiased_circle(mRadius * 2, mColor.red, mColor.green, mColor.blue, mColor.alpha, 6);
	}

	return;
}

GUIFill::~GUIFill()
{
	if (mCircle1)
		gr_free_surface(mCircle1);
	if (mCircle2)
		gr_free_surface(mCircle2);
	if (mCircle3)
		gr_free_surface(mCircle3);
	if (mCircle4)
		gr_free_surface(mCircle4);
}

int GUIFill::Render(void)
{
	if (!isConditionTrue())
		return 0;

	// [f/d] rounded fill
	if(mRadius <= -1) { // "lite" mode, draws two circles on the sides, d = h
		gr_blit(mCircle1, 0, 0, mRenderH / 2, mRenderH + 1, mRenderX, mRenderY);
		gr_blit(mCircle2, 0, 0, mRenderH / 2, mRenderH + 1, mRenderX + mRenderW - mRenderH / 2, mRenderY);

		gr_color(mColor.red, mColor.green, mColor.blue, mColor.alpha);
		gr_fill(mRenderX + mRenderH / 2, mRenderY, mRenderW - mRenderH, mRenderH);
	} else if(mRadius > 0) {
		// draw 4 circles
		int d = mRadius * 2 + 1;
		gr_blit(mCircle1, 0, 0, d, d, mRenderX, mRenderY);
		gr_blit(mCircle2, 0, 0, d, d, mRenderX + mRenderW - d, mRenderY);
		gr_blit(mCircle3, 0, 0, d, d, mRenderX, mRenderY + mRenderH - d);
		gr_blit(mCircle4, 0, 0, d, d, mRenderX + mRenderW - d, mRenderY + mRenderH - d);

		// draw +
		gr_color(mColor.red, mColor.green, mColor.blue, mColor.alpha);
		gr_fill(mRenderX + d, mRenderY, mRenderW - d * 2, mRenderH);
		gr_fill(mRenderX, mRenderY + d, mRenderW, mRenderH - d * 2);
	} else {
		gr_color(mColor.red, mColor.green, mColor.blue, mColor.alpha);
		gr_fill(mRenderX, mRenderY, mRenderW, mRenderH);
	}
	// [/f/d]


	return 0;
}

