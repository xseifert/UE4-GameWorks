// Copyright (c) 2017 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.
//
// ---------------------------------------------------------------------------
//
// This file was generated by the CEF translator tool. If making changes by
// hand only do so within the body of existing method and function
// implementations. See the translator.README.txt file in the tools directory
// for more information.
//

#ifndef CEF_LIBCEF_DLL_CTOCPP_IMAGE_CTOCPP_H_
#define CEF_LIBCEF_DLL_CTOCPP_IMAGE_CTOCPP_H_
#pragma once

#if !defined(WRAPPING_CEF_SHARED)
#error This file can be included wrapper-side only
#endif

#include "include/cef_image.h"
#include "include/capi/cef_image_capi.h"
#include "libcef_dll/ctocpp/ctocpp_ref_counted.h"

// Wrap a C structure with a C++ class.
// This class may be instantiated and accessed wrapper-side only.
class CefImageCToCpp
    : public CefCToCppRefCounted<CefImageCToCpp, CefImage, cef_image_t> {
 public:
  CefImageCToCpp();

  // CefImage methods.
  bool IsEmpty() OVERRIDE;
  bool IsSame(CefRefPtr<CefImage> that) OVERRIDE;
  bool AddBitmap(float scale_factor, int pixel_width, int pixel_height,
      cef_color_type_t color_type, cef_alpha_type_t alpha_type,
      const void* pixel_data, size_t pixel_data_size) OVERRIDE;
  bool AddPNG(float scale_factor, const void* png_data,
      size_t png_data_size) OVERRIDE;
  bool AddJPEG(float scale_factor, const void* jpeg_data,
      size_t jpeg_data_size) OVERRIDE;
  size_t GetWidth() OVERRIDE;
  size_t GetHeight() OVERRIDE;
  bool HasRepresentation(float scale_factor) OVERRIDE;
  bool RemoveRepresentation(float scale_factor) OVERRIDE;
  bool GetRepresentationInfo(float scale_factor, float& actual_scale_factor,
      int& pixel_width, int& pixel_height) OVERRIDE;
  CefRefPtr<CefBinaryValue> GetAsBitmap(float scale_factor,
      cef_color_type_t color_type, cef_alpha_type_t alpha_type,
      int& pixel_width, int& pixel_height) OVERRIDE;
  CefRefPtr<CefBinaryValue> GetAsPNG(float scale_factor, bool with_transparency,
      int& pixel_width, int& pixel_height) OVERRIDE;
  CefRefPtr<CefBinaryValue> GetAsJPEG(float scale_factor, int quality,
      int& pixel_width, int& pixel_height) OVERRIDE;
};

#endif  // CEF_LIBCEF_DLL_CTOCPP_IMAGE_CTOCPP_H_
