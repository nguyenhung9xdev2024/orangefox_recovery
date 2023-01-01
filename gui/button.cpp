/*
	Copyright 2012 bigbiff/Dees_Troy TeamWin
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
#include "../data.hpp"

#include <string>

extern "C" {
#include "../twcommon.h"
}
#include "minuitwrp/minui.h"

#include "rapidxml.hpp"
#include "objects.hpp"

GUIButton::GUIButton(xml_node<>* node)
	: GUIObject(node)
{
	mButtonImg = NULL;
	mButtonIcon = NULL;
	mButtonLabel = NULL;
	mAction = NULL;
	mRendered = false;
	hasHighlightColor = false;
	renderHighlight = false;
	hasFill = false;

	if (!node)  return;

	// These can be loaded directly from the node
	mButtonLabel = new GUIText(node);
	mAction = new GUIAction(node);

	mButtonImg = new GUIImage(node);
	if (mButtonImg->Render() < 0)
	{
		delete mButtonImg;
		mButtonImg = NULL;
	}
	if (mButtonLabel->Render() < 0)
	{
		delete mButtonLabel;
		mButtonLabel = NULL;
	}
	// Load fill if it exists
	mFillColor = LoadAttrColor(FindNode(node, "fill"), "color", &hasFill);
	if (!hasFill && mButtonImg == NULL) {
		LOGERR("No image resource or fill specified for button.\n");
	}

	// The icon is a special case
	mButtonIcon = LoadAttrImage(FindNode(node, "icon"), "resource");

	mHighlightColor = LoadAttrColor(FindNode(node, "highlight"), "color", &hasHighlightColor);

	int x = 0, y = 0, w = 0, h = 0;
	TextPlacement = TOP_LEFT;
	mPlacement = TOP_LEFT;
	if (mButtonImg) {
		mButtonImg->GetRenderPos(x, y, w, h);
	} else if (hasFill) {
		LoadPlacement(FindNode(node, "placement"), &x, &y, &w, &h, &TextPlacement);
	}

	if (hasFill) {
		xml_node<>* placementNode = FindNode(node, "placement");

		if (placementNode->first_attribute("w"))
			mFillW = LoadAttrIntScaleX(placementNode, "w");

		if (placementNode->first_attribute("h"))
			mFillH = LoadAttrIntScaleY(placementNode, "h");

		if (mButtonImg == NULL) {
			if (placementNode->first_attribute("placement"))
				mPlacement = (Placement) LoadAttrInt(placementNode, "placement");
				
			if (mPlacement != TOP_LEFT && mPlacement != BOTTOM_LEFT)
			{
				if (mPlacement == CENTER)
					x -= (w / 2);
				else
					x -= w;
			}
			if (mPlacement != TOP_LEFT && mPlacement != TOP_RIGHT)
			{
				if (mPlacement == CENTER)
					y -= (h / 2);
				else
					y -= h;
			}
		}

		mRadius = LoadAttrIntScaleX(FindNode(node, "fill"), "radius");

		if(mRadius <= -1) {
			mCircle1 = gr_render_antialiased_circle(mFillH / 2 - 1, mFillColor.red, mFillColor.green, mFillColor.blue, mFillColor.alpha, 1);
			mCircle2 = gr_render_antialiased_circle(mFillH / 2 - 1, mFillColor.red, mFillColor.green, mFillColor.blue, mFillColor.alpha, 2);
		} else if(mRadius > 0) {
			mCircle1 = gr_render_antialiased_circle(mRadius * 2, mFillColor.red, mFillColor.green, mFillColor.blue, mFillColor.alpha, 3);
			mCircle2 = gr_render_antialiased_circle(mRadius * 2, mFillColor.red, mFillColor.green, mFillColor.blue, mFillColor.alpha, 4);
			mCircle3 = gr_render_antialiased_circle(mRadius * 2, mFillColor.red, mFillColor.green, mFillColor.blue, mFillColor.alpha, 5);
			mCircle4 = gr_render_antialiased_circle(mRadius * 2, mFillColor.red, mFillColor.green, mFillColor.blue, mFillColor.alpha, 6);
		}
	}

	SetRenderPos(x, y, w, h);
	if (mButtonLabel) {
		TextPlacement = (Placement)LoadAttrInt(FindNode(node, "placement"), "textplacement", TOP_LEFT);
		if (TextPlacement != TEXT_ONLY_RIGHT) {
			mButtonLabel->scaleWidth = 1;
			mButtonLabel->SetMaxWidth(w);
			mButtonLabel->SetPlacement(CENTER);
			mTextX = ((mRenderW / 2) + mRenderX);
			mTextY = mRenderY + (mRenderH / 2);
			mButtonLabel->SetRenderPos(mTextX, mTextY);
		} else {
			mTextX = mRenderW + mRenderX + 5;
			mButtonLabel->GetCurrentBounds(mTextW, mTextH);
			mRenderW += mTextW + 5;
			mTextY = mRenderY + (mRenderH / 2) - (mTextH / 2);
			mButtonLabel->SetRenderPos(mTextX, mTextY);
			if (mAction)
				mAction->SetActionPos(mRenderX, mRenderY, mRenderW, mRenderH);
			SetActionPos(mRenderX, mRenderY, mRenderW, mRenderH);
		}
	}
}

GUIButton::~GUIButton()
{
	delete mButtonImg;
	delete mButtonLabel;
	delete mAction;
	
	if (mCircle1)
		gr_free_surface(mCircle1);
	if (mCircle2)
		gr_free_surface(mCircle2);
	if (mCircle3)
		gr_free_surface(mCircle3);
	if (mCircle4)
		gr_free_surface(mCircle4);
}

int GUIButton::Render(void)
{
	if (!isConditionTrue())
	{
		mRendered = false;
		return 0;
	}

	int ret = 0;

	if (mButtonImg)	 ret = mButtonImg->Render();
	//if (ret >= 0)		return ret;
	if (hasFill) {
		// [f/d] rounded fill 
		if(mRadius <= -1) {
			gr_blit(mCircle1, 0, 0, mRenderH / 2, mRenderH + 1, mRenderX, mRenderY);
			gr_blit(mCircle2, 0, 0, mRenderH / 2, mRenderH + 1, mRenderX + mRenderW - mRenderH / 2, mRenderY);

			gr_color(mFillColor.red, mFillColor.green, mFillColor.blue, mFillColor.alpha);
			gr_fill(mRenderX + mRenderH / 2, mRenderY, mRenderW - mRenderH, mRenderH);
		} else if(mRadius > 0) {
			int d = mRadius * 2 + 1;
			int posFixX = mRenderW == mFillW ? 0 : (mRenderW - mFillW) / 2;
			int posFixY = mRenderH == mFillH ? 0 : (mRenderH - mFillH) / 2;

			gr_blit(mCircle1, 0, 0, d, d, posFixX + mRenderX, posFixY + mRenderY);
			gr_blit(mCircle2, 0, 0, d, d, posFixX + mRenderX + mFillW - d, posFixY + mRenderY);
			gr_blit(mCircle3, 0, 0, d, d, posFixX + mRenderX, posFixY + mRenderY + mFillH - d);
			gr_blit(mCircle4, 0, 0, d, d, posFixX + mRenderX + mFillW - d, posFixY + mRenderY + mFillH - d);

			gr_color(mFillColor.red, mFillColor.green, mFillColor.blue, mFillColor.alpha);
			gr_fill(posFixX + mRenderX + d, posFixY + mRenderY, mFillW - d * 2, mFillH);
			gr_fill(posFixX + mRenderX, posFixY + mRenderY + d, mFillW, mFillH - d * 2);
		} else {
			gr_color(mFillColor.red, mFillColor.green, mFillColor.blue, mFillColor.alpha);
			gr_fill(mRenderX, mRenderY, mFillW, mFillH);
		}
	}
	if (mButtonIcon && mButtonIcon->GetResource())
		gr_blit(mButtonIcon->GetResource(), 0, 0, mIconW, mIconH, mIconX, mIconY);
	if (mButtonLabel) {
		int w, h;
		mButtonLabel->GetCurrentBounds(w, h);
		if (w != mTextW) {
			mTextW = w;
		}
		ret = mButtonLabel->Render();
		if (ret < 0)		return ret;
	}
	if (renderHighlight && hasHighlightColor) {
		gr_color(mHighlightColor.red, mHighlightColor.green, mHighlightColor.blue, mHighlightColor.alpha);
		gr_fill(mRenderX, mRenderY, mRenderW, mRenderH);
	}
	mRendered = true;
	return ret;
}

int GUIButton::Update(void)
{
	if (!isConditionTrue())	return (mRendered ? 2 : 0);
	if (!mRendered)			return 2;

	int ret = 0, ret2 = 0;

	if (mButtonImg)			ret = mButtonImg->Update();
	if (ret < 0)			return ret;

	if (ret == 0)
	{
		if (mButtonLabel) {
			ret2 = mButtonLabel->Update();
			if (ret2 < 0)	return ret2;
			if (ret2 > ret)	ret = ret2;
		}
	}
	else if (ret == 1)
	{
		// The button re-rendered, so everyone else is a render
		if (mButtonIcon && mButtonIcon->GetResource())
			gr_blit(mButtonIcon->GetResource(), 0, 0, mIconW, mIconH, mIconX, mIconY);
		if (mButtonLabel)   ret = mButtonLabel->Render();
		if (ret < 0)		return ret;
		ret = 1;
	}
	else
	{
		// Aparently, the button needs a background update
		ret = 2;
	}
	return ret;
}

int GUIButton::SetRenderPos(int x, int y, int w, int h)
{
	mRenderX = x;
	mRenderY = y;
	if (w || h)
	{
		mRenderW = w;
		mRenderH = h;
	}
	mIconW = mIconH = 0;

	if (mButtonIcon && mButtonIcon->GetResource()) {
		mIconW = mButtonIcon->GetWidth();
		mIconH = mButtonIcon->GetHeight();
	}

	mTextH = 0;
	mTextW = 0;
	mIconX = mRenderX + ((mRenderW - mIconW) / 2);
	if (mButtonLabel)   mButtonLabel->GetCurrentBounds(mTextW, mTextH);
	if (mTextW && TextPlacement == TEXT_ONLY_RIGHT)
	{
		mRenderW += mTextW + 5;
	}

	if (mIconH == 0 || mTextH == 0 || mIconH + mTextH > mRenderH)
	{
		mIconY = mRenderY + (mRenderH / 2) - (mIconH / 2);
	}
	else
	{
		int divisor = mRenderH - (mIconH + mTextH);
		mIconY = mRenderY + (divisor / 3);
	}

	if (mButtonLabel)   mButtonLabel->SetRenderPos(mTextX, mTextY);
	if (mAction)		mAction->SetActionPos(mRenderX, mRenderY, mRenderW, mRenderH);
	SetActionPos(mRenderX, mRenderY, mRenderW, mRenderH);
	return 0;
}

int GUIButton::NotifyTouch(TOUCH_STATE state, int x, int y)
{
	static int last_state = 0;

	if (!isConditionTrue())	 return -1;
	if (x < mRenderX || x - mRenderX > mRenderW || y < mRenderY || y - mRenderY > mRenderH || state == TOUCH_RELEASE) {
		if (last_state == 1) {
			last_state = 0;
			if (mButtonLabel != NULL)
				mButtonLabel->isHighlighted = false;
			if (mButtonImg != NULL)
				mButtonImg->isHighlighted = false;
			renderHighlight = false;
			mRendered = false;
		}
	} else {
		if (last_state == 0) {
			last_state = 1;

#ifndef TW_NO_HAPTICS
			DataManager::Vibrate("tw_button_vibrate");
#endif
			if (mButtonLabel != NULL)
				mButtonLabel->isHighlighted = true;
			if (mButtonImg != NULL)
				mButtonImg->isHighlighted = true;
			renderHighlight = true;
			mRendered = false;
		}
	}
	if (x < mRenderX || x - mRenderX > mRenderW || y < mRenderY || y - mRenderY > mRenderH)
		return 0;
	return (mAction ? mAction->NotifyTouch(state, x, y) : 1);
}
