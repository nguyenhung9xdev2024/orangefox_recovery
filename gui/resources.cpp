/*
    Copyright 2012 to 2020 TeamWin
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

// resource.cpp - Source to manage GUI resources

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <fcntl.h>
#include <ziparchive/zip_archive.h>
#include <android-base/unique_fd.h>

extern "C" {
#include "../twcommon.h"
#include "gui.h"
}

#include "minuitwrp/truetype.hpp"
#include "minuitwrp/minui.h"

#include "rapidxml.hpp"
#include "objects.hpp"

#define TMP_RESOURCE_NAME   "/tmp/extract.bin"

Resource::Resource(xml_node<>* node, ZipArchiveHandle pZip __unused)
{
	if (node && node->first_attribute("name"))
		mName = node->first_attribute("name")->value();
}

int Resource::ExtractResource(ZipArchiveHandle pZip, std::string folderName, std::string fileName, std::string fileExtn, std::string destFile)
{
	if (!pZip)
		return -1;

	std::string src = folderName + "/" + fileName + fileExtn;
	ZipEntry binary_entry;
	if (FindEntry(pZip, src, &binary_entry) == 0) {
		android::base::unique_fd fd(
			open(destFile.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0666));
		if (fd == -1) {
			return -1;
		}
		int32_t err = ExtractEntryToFile(pZip, &binary_entry, fd);
		if (err != 0)
			return -1;
	} else {
		return -1;
	}
	return 0;
}

void Resource::LoadImage(ZipArchiveHandle pZip, std::string file, gr_surface* surface)
{
	int rc = 0;
	if (ExtractResource(pZip, "images", file, ".png", TMP_RESOURCE_NAME) == 0)
	{
		rc = res_create_surface(TMP_RESOURCE_NAME, surface);
		unlink(TMP_RESOURCE_NAME);
	}
	else if (ExtractResource(pZip, "images", file, "", TMP_RESOURCE_NAME) == 0)
	{
		// JPG includes the .jpg extension in the filename so extension should be blank
		rc = res_create_surface(TMP_RESOURCE_NAME, surface);
		unlink(TMP_RESOURCE_NAME);
	}
	else if (!pZip)
	{
		// File name in xml may have included .png so try without adding .png
		rc = res_create_surface(file.c_str(), surface);
	}
	if (rc != 0)
		LOGINFO("Failed to load image from %s%s, error %d\n", file.c_str(), pZip ? " (zip)" : "", rc);
}

void Resource::CheckAndScaleImage(gr_surface source, gr_surface* destination, int retain_aspect)
{
	if (!source) {
		*destination = nullptr;
		return;
	}
	if (get_scale_w() != 0 && get_scale_h() != 0) {
		float scale_w = get_scale_w(), scale_h = get_scale_h();
		if (retain_aspect) {
			if (scale_w < scale_h)
				scale_h = scale_w;
			else
				scale_w = scale_h;
		}
		if (res_scale_surface(source, destination, scale_w, scale_h)) {
			LOGINFO("Error scaling image, using regular size.\n");
			*destination = source;
		}
	} else {
		*destination = source;
	}
}

FontResource::FontResource(xml_node<>* node, ZipArchiveHandle pZip)
 : Resource(node, pZip)
{
	origFontSize = 0;
	origFont = NULL;
	LoadFont(node, pZip);
}

void FontResource::LoadFont(xml_node<>* node, ZipArchiveHandle pZip)
{
	std::string file;
	xml_attribute<>* attr;

	mFont = NULL;
	if (!node)
		return;

	attr = node->first_attribute("filename");
	if (!attr)
		return;

	file = attr->value();

	if (file.size() >= 4 && file.compare(file.size()-4, 4, ".ttf") == 0)
	{
		int font_size = 0;

		if (origFontSize != 0) {
			attr = node->first_attribute("scale");
			if (attr == NULL)
				return;
			font_size = origFontSize * atoi(attr->value()) / 100;
		} else {
			attr = node->first_attribute("size");
			if (attr == NULL)
				return;
			font_size = scale_theme_min(atoi(attr->value()));
			origFontSize = font_size;
		}

		int dpi = 300;

		attr = node->first_attribute("dpi");
		if (attr)
			dpi = atoi(attr->value());

		// we can't use TMP_RESOURCE_NAME here because the ttf subsystem is caching the name and scaling needs to reload the font
		std::string tmpname = "/tmp/" + file;
		if (ExtractResource(pZip, "fonts", file, "", tmpname) == 0)
		{
			mFont = twrpTruetype::gr_ttf_loadFont(tmpname.c_str(), font_size, dpi);
		}
		else
		{
			file = std::string(TWRES "fonts/") + file;
			mFont = twrpTruetype::gr_ttf_loadFont(file.c_str(), font_size, dpi);
		}
	}
	else
	{
		LOGERR("Non-TTF fonts are no longer supported.\n");
	}
}

void FontResource::DeleteFont() {
	if (mFont) {
		twrpTruetype::gr_ttf_freeFont(mFont);
	}
	mFont = NULL;
	if (origFont) {
		twrpTruetype::gr_ttf_freeFont(origFont);
	}
	origFont = NULL;
}

void FontResource::Override(xml_node<>* node, ZipArchiveHandle pZip) {
	if (!origFont) {
		origFont = mFont;
	} else if (mFont) {
		twrpTruetype::gr_ttf_freeFont(mFont);
		mFont = NULL;
	}
	LoadFont(node, pZip);
}

FontResource::~FontResource()
{
	DeleteFont();
}

ImageResource::ImageResource(xml_node<>* node, ZipArchiveHandle pZip)
 : Resource(node, pZip)
{
	std::string file;
	gr_surface temp_surface = nullptr;

	mSurface = NULL;
	if (!node) {
		LOGERR("ImageResource node is NULL\n");
		return;
	}

	if (node->first_attribute("filename"))
		file = node->first_attribute("filename")->value();
	else {
		LOGERR("No filename specified for image resource.\n");
		return;
	}

	bool retain_aspect = (node->first_attribute("retainaspect") != NULL);
	// the value does not matter, if retainaspect is present, we assume that we want to retain it
	LoadImage(pZip, file, &temp_surface);
	CheckAndScaleImage(temp_surface, &mSurface, retain_aspect);
}

// [f/d] draw antialiased circles, rectangles, rounded rectangles
// radius == -1 - fully rounded sides
// stroke == 0 - filled shape
// It's here because modifying minuitwrp requires full project rebuild
uint32_t* createShape(int w, int h, int radius, int stroke, COLOR color)
{
	const int malloc_size = w * h * 4;
    uint32_t *data;

    data = (uint32_t *)malloc(malloc_size);
    memset(data, 0, malloc_size);
	
    const uint32_t px = (color.alpha << 24) | (color.blue << 16) | (color.green << 8) | color.red;

	if (radius == 0 && stroke == 0) // that's just a fill
	{
		if (color.alpha != 0) // or it may be nothing
			for (int i = 0; i < w * h; i++)
				*(data + i) = px;
			
		return data;
	}

    float rx, ry;
    int min_side = std::min(w, h) / 2;
    if (radius < 0 || radius > min_side) radius = min_side;
    if (stroke < 0 || stroke > min_side) stroke = 0;

    int diameter = radius * 2;

	// original circle method produces circles with odd diameter. This variable makes circles with even diameter
    const float radius2 = radius - 0.5; 

    const float radius_check = radius2 * radius2 + radius2 * 0.8;
    const float radius_check_aa = radius2 * radius2 + radius2 * 1.6;
    
    const uint32_t px_aa = ((color.alpha / 2) << 24) | (color.blue << 16) | (color.green << 8) | color.red; // antialiasing
    
    const int s_half = w * h / 2;

    if (stroke <= 0) {
        for (int i = 0; i < w * h; i++)
            *(data + i) = px;

        for(ry = -radius2; ry <= radius; ++ry)
            for(rx = -radius2; rx <= radius; ++rx){
                int check = rx*rx+ry*ry,
                    space_w = rx >= 0 ? w - diameter : 0,
                    space_h = ry >= 0 ? h - diameter : 0,
                    pos = w*(radius2 + (ry + space_h)) + (radius2+rx) + space_w;

                if(check <= radius_check)
                    *(data + pos) = px;
                else if (check <= radius_check_aa && ry < radius)
                    *(data + pos) = px_aa;
                else
                    *(data + pos) = 0;
            }
		
    	return data;
    }

    const float radius_check_hollow = radius2 * radius2 - radius2 * 0.8 - radius * 2 * (stroke - 1);
    const float radius_check_hollow_aa = radius2 * radius2 - radius2 * 1.4 - radius * 2 * (stroke - 1);

    for(ry = -radius2; ry <= radius; ++ry)
        for(rx = -radius2; rx <= radius; ++rx) {
            int check = rx*rx+ry*ry,
                space_w = rx >= 0 ? w - diameter : 0,
                space_h = ry >= 0 ? h - diameter : 0,
                pos = w*(radius2 + (ry + space_h)) + (radius2+rx) + space_w;


            if(check < radius_check && check > radius_check_hollow) {
                *(data + pos) =  px;
                
                if (rx == -0.5)
                    for (int ii = 1; ii <= w - diameter; ii++)
                        *(data + pos + ii) =  px;    
                else if ((int)ry == 0 && pos < s_half)
                    for (int ii = 1; ii <= h - diameter; ii++)
                        *(data + pos + ii*w) = px;
                        
            } else if (check < radius_check_aa && check > radius_check_hollow_aa && ry < radius && ry < radius && rx < diameter)
                *(data + pos) = px_aa;
            else
                *(data + pos) = 0;
        }

    return data;
}

// [f/d] constructor for fake images that are actually shapes
// Usage: <resources><shape name="img_name" color="#FFFFFF" w="100" h="50" radius="10" stroke="0" /> </resources>
// Then, reference it like normal image: <image resource="img_name"/>
ImageResource::ImageResource(xml_node<>* node) : Resource(node, NULL)
{
	if (!node) {
		LOGERR("ImageResource node is NULL\n");
		return;
	}

	int original_radius = LoadAttrInt(node, "radius", 0),
		w = LoadAttrIntScaleX(node, "w", 1),
		h = LoadAttrIntScaleY(node, "h", 1),
		r = original_radius <= 0 ? original_radius : scale_theme_x(original_radius), //don't scale -1 value
		s = LoadAttrIntScaleY(node, "stroke", 0);

	

	COLOR color = LoadAttrColor(node, "color", COLOR(0,0,0));
	
    GGLSurface *surface;
    surface = (GGLSurface *)malloc(sizeof(GGLSurface));
    memset(surface, 0, sizeof(GGLSurface));

	surface->version = sizeof(surface);
    surface->width = w;
    surface->height = h;
    surface->stride = w;
    surface->data = (GGLubyte*)createShape(w, h, r, s, color);
	// TODO: FIX RECOVERY_BGRA
	#if defined(RECOVERY_BGRA)
		surface->format = GGL_PIXEL_FORMAT_BGRA_8888;
	#else
		surface->format = GGL_PIXEL_FORMAT_RGBA_8888;
	#endif
	
    mSurface = (gr_surface)surface;
}
// [/f/d]

ImageResource::~ImageResource()
{
	if (mSurface)
		res_free_surface(mSurface);
}

AnimationResource::AnimationResource(xml_node<>* node, ZipArchiveHandle pZip)
 : Resource(node, pZip)
{
	std::string file;
	int fileNum = 1;

	if (!node)
		return;

	if (node->first_attribute("filename"))
		file = node->first_attribute("filename")->value();
	else {
		LOGERR("No filename specified for image resource.\n");
		return;
	}

	bool retain_aspect = (node->first_attribute("retainaspect") != NULL);
	// the value does not matter, if retainaspect is present, we assume that we want to retain it
	for (;;)
	{
		std::ostringstream fileName;
		fileName << file << std::setfill ('0') << std::setw (3) << fileNum;

		gr_surface surface = nullptr;
		gr_surface temp_surface = nullptr;
		LoadImage(pZip, fileName.str(), &temp_surface);
		CheckAndScaleImage(temp_surface, &surface, retain_aspect);
		if (surface) {
			mSurfaces.push_back(surface);
			fileNum++;
		} else
			break; // Done loading animation images
	}
}

AnimationResource::~AnimationResource()
{
	std::vector<gr_surface>::iterator it;

	for (it = mSurfaces.begin(); it != mSurfaces.end(); ++it)
		res_free_surface(*it);

	mSurfaces.clear();
}

FontResource* ResourceManager::FindFont(const std::string& name) const
{
	for (std::vector<FontResource*>::const_iterator it = mFonts.begin(); it != mFonts.end(); ++it)
		if (name == (*it)->GetName())
			return *it;
	return NULL;
}

ImageResource* ResourceManager::FindImage(const std::string& name) const
{
	for (std::vector<ImageResource*>::const_iterator it = mImages.begin(); it != mImages.end(); ++it)
		if (name == (*it)->GetName())
			return *it;
	return NULL;
}

AnimationResource* ResourceManager::FindAnimation(const std::string& name) const
{
	for (std::vector<AnimationResource*>::const_iterator it = mAnimations.begin(); it != mAnimations.end(); ++it)
		if (name == (*it)->GetName())
			return *it;
	return NULL;
}

std::string ResourceManager::FindString(const std::string& name) const
{
	//if (this != NULL) {
		std::map<std::string, string_resource_struct>::const_iterator it = mStrings.find(name);
		if (it != mStrings.end())
			return it->second.value;
		LOGERR("String resource '%s' not found. No default value.\n", name.c_str());
		PageManager::AddStringResource("NO DEFAULT", name, "[" + name + ("]"));
	/*} else {
		LOGINFO("String resources not loaded when looking for '%s'. No default value.\n", name.c_str());
	}*/
	return "[" + name + ("]");
}

std::string ResourceManager::FindString(const std::string& name, const std::string& default_string) const
{
	//if (this != NULL) {
		std::map<std::string, string_resource_struct>::const_iterator it = mStrings.find(name);
		if (it != mStrings.end())
			return it->second.value;
		LOGERR("String resource '%s' not found. Using default value.\n", name.c_str());
		PageManager::AddStringResource("DEFAULT", name, default_string);
	/*} else {
		LOGINFO("String resources not loaded when looking for '%s'. Using default value.\n", name.c_str());
	}*/
	return default_string;
}

void ResourceManager::DumpStrings() const
{
	/*if (this == NULL) {
		gui_print("No string resources\n");
		return;
	}*/
	std::map<std::string, string_resource_struct>::const_iterator it;
	gui_print("Dumping all strings:\n");
	for (it = mStrings.begin(); it != mStrings.end(); it++)
		gui_print("source: %s: '%s' = '%s'\n", it->second.source.c_str(), it->first.c_str(), it->second.value.c_str());
	gui_print("Done dumping strings\n");
}

ResourceManager::ResourceManager()
{
}

void ResourceManager::AddStringResource(std::string resource_source, std::string resource_name, std::string value)
{
	string_resource_struct res;
	res.source = resource_source;
	res.value = value;
	mStrings[resource_name] = res;
}

void ResourceManager::LoadResources(xml_node<>* resList, ZipArchiveHandle pZip, std::string resource_source)
{
	if (!resList)
		return;

	for (xml_node<>* child = resList->first_node(); child; child = child->next_sibling())
	{
		std::string type = child->name();
		if (type == "resource") {
			// legacy format : <resource type="...">
			xml_attribute<>* attr = child->first_attribute("type");
			type = attr ? attr->value() : "*unspecified*";
		}

		bool error = false;
		if (type == "font")
		{
			FontResource* res = new FontResource(child, pZip);
			if (res && res->GetResource())
				mFonts.push_back(res);
			else {
				error = true;
				delete res;
			}
		}
		else if (type == "fontoverride")
		{
			if (mFonts.size() != 0 && child && child->first_attribute("name")) {
				string FontName = child->first_attribute("name")->value();
				size_t font_count = mFonts.size(), i;
				bool found = false;

				for (i = 0; i < font_count; i++) {
					if (mFonts[i]->GetName() == FontName) {
						mFonts[i]->Override(child, pZip);
						found = true;
						break;
					}
				}
				if (!found) {
					LOGERR("Unable to locate font '%s' for override.\n", FontName.c_str());
				}
			} else if (mFonts.size() != 0)
				LOGERR("Unable to locate font name for type fontoverride.\n");
		}
		else if (type == "image")
		{
			ImageResource* res = new ImageResource(child, pZip);
			if (res && res->GetResource())
				mImages.push_back(res);
			else {
				error = true;
				delete res;
			}
		}
		else if (type == "shape")
		{
			ImageResource* res = new ImageResource(child);
			if (res && res->GetResource())
				mImages.push_back(res);
			else {
				error = true;
				delete res;
			}
		}
		else if (type == "animation")
		{
			AnimationResource* res = new AnimationResource(child, pZip);
			if (res && res->GetResourceCount())
				mAnimations.push_back(res);
			else {
				error = true;
				delete res;
			}
		}
		else if (type == "string")
		{
			if (xml_attribute<>* attr = child->first_attribute("name")) {
				string_resource_struct res;
				res.source = resource_source;
				res.value = child->value();
				mStrings[attr->value()] = res;
			} else
				error = true;
		}
		else
		{
			LOGERR("Resource type (%s) not supported.\n", type.c_str());
			error = true;
		}

		if (error)
		{
			std::string res_name;
			if (child->first_attribute("name"))
				res_name = child->first_attribute("name")->value();
			if (res_name.empty() && child->first_attribute("filename"))
				res_name = child->first_attribute("filename")->value();

			if (!res_name.empty()) {
				LOGERR("Resource (%s)-(%s) failed to load\n", type.c_str(), res_name.c_str());
			} else
				LOGERR("Resource type (%s) failed to load\n", type.c_str());
		}
	}
}

ResourceManager::~ResourceManager()
{
	for (std::vector<FontResource*>::iterator it = mFonts.begin(); it != mFonts.end(); ++it)
		delete *it;

	for (std::vector<ImageResource*>::iterator it = mImages.begin(); it != mImages.end(); ++it)
		delete *it;

	for (std::vector<AnimationResource*>::iterator it = mAnimations.begin(); it != mAnimations.end(); ++it)
		delete *it;
}
