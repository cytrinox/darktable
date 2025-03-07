/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
// our includes go first:
#include "bauhaus/bauhaus.h"
#include "common/exif.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/opencl.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/openmp_maths.h"
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/gradientslider.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"

//#include <gtk/gtk.h>
#include <stdlib.h>
#define LUT_ELEM 360 // gamut LUT number of elements: resolution of 1°
#define STEPS 72     // so we test 72×72×72 combinations of RGB in [0; 1] to build the gamut LUT

// Filmlight Yrg puts red at 330°, while usual HSL wheels put it at 360/0°
// so shift in GUI only it to not confuse people. User params are always degrees,
// pixel params are always radians.
#define ANGLE_SHIFT -30.f
#define DEG_TO_RAD(x) ((x + ANGLE_SHIFT) * M_PI / 180.f)
#define RAD_TO_DEG(x) (x * 180.f / M_PI - ANGLE_SHIFT)

DT_MODULE_INTROSPECTION(3, dt_iop_colorbalancergb_params_t)


typedef struct dt_iop_colorbalancergb_params_t
{
  /* params of v1 */
  float shadows_Y;             // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float shadows_C;             // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float shadows_H;             // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float midtones_Y;            // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float midtones_C;            // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float midtones_H;            // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float highlights_Y;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float highlights_C;          // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float highlights_H;          // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float global_Y;              // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
  float global_C;              // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
  float global_H;              // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float shadows_weight;        // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "shadows fall-off"
  float white_fulcrum;         // $MIN: -16.0 $MAX:   16.0 $DEFAULT: 0.0 $DESCRIPTION: "white fulcrum"
  float highlights_weight;     // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "highlights fall-off"
  float chroma_shadows;        // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
  float chroma_highlights;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float chroma_global;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
  float chroma_midtones;       // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
  float saturation_global;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
  float saturation_highlights; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float saturation_midtones;   // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
  float saturation_shadows;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
  float hue_angle;             // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "hue shift"

  /* params of v2 */
  float brilliance_global;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
  float brilliance_highlights; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
  float brilliance_midtones;   // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
  float brilliance_shadows;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"

  /* params of v3 */
  float grey_fulcrum;     // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.1845 $DESCRIPTION: "middle-grey fulcrum"

  /* add future params after this so the legacy params import can use a blind memcpy */


} dt_iop_colorbalancergb_params_t;


typedef enum dt_iop_colorbalancergb_mask_data_t
{
  MASK_SHADOWS = 0,
  MASK_MIDTONES = 1,
  MASK_HIGHLIGHTS = 2,
  MASK_NONE
} dt_iop_colorbalancergb_mask_data_t;


typedef struct dt_iop_colorbalancergb_gui_data_t
{
  GtkWidget *shadows_H, *midtones_H, *highlights_H, *global_H;
  GtkWidget *shadows_C, *midtones_C, *highlights_C, *global_C;
  GtkWidget *shadows_Y, *midtones_Y, *highlights_Y, *global_Y;
  GtkWidget *shadows_weight, *grey_fulcrum, *highlights_weight, *white_fulcrum;
  GtkWidget *chroma_highlights, *chroma_global, *chroma_shadows, *chroma_midtones;
  GtkWidget *saturation_global, *saturation_highlights, *saturation_midtones, *saturation_shadows;
  GtkWidget *brilliance_global, *brilliance_highlights, *brilliance_midtones, *brilliance_shadows;
  GtkWidget *hue_angle;
  GtkDrawingArea *area;
  GtkNotebook *notebook;
  GtkWidget *checker_color_1_picker, *checker_color_2_picker, *checker_size;
  gboolean mask_display;
  dt_iop_colorbalancergb_mask_data_t mask_type;
} dt_iop_colorbalancergb_gui_data_t;


typedef struct dt_iop_colorbalancergb_data_t
{
  float global[4];
  float shadows[4];
  float highlights[4];
  float midtones[4];
  float midtones_Y;
  float chroma_global, chroma[4];
  float saturation_global, saturation[4];
  float brilliance_global, brilliance[4];
  float hue_angle;
  float shadows_weight, white_fulcrum, highlights_weight, midtones_weight, grey_fulcrum;
  float *gamut_LUT;
  float max_chroma;
  float checker_color_1[4], checker_color_2[4];
  size_t checker_size;
  gboolean lut_inited;
  struct dt_iop_order_iccprofile_info_t *work_profile;
} dt_iop_colorbalancergb_data_t;

const char *name()
{
  return _("color balance rgb");
}

const char *aliases()
{
  return _("offset power slope|cdl|color grading|contrast|chroma_highlights|hue");
}

const char *description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("affect color, brightness and contrast"),
                                      _("corrective or creative"),
                                      _("linear, Lab, scene-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, Lab, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 3)
  {
    typedef struct dt_iop_colorbalancergb_params_v1_t
    {
      float shadows_Y;             // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float shadows_C;             // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float shadows_H;             // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float midtones_Y;            // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float midtones_C;            // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float midtones_H;            // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float highlights_Y;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float highlights_C;          // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float highlights_H;          // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float global_Y;              // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float global_C;              // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float global_H;              // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float shadows_weight;        // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "tonal weight"
      float white_fulcrum;       // $MIN: -6.0 $MAX:   6.0 $DEFAULT: 0.0 $DESCRIPTION: "fulcrum"
      float highlights_weight;     // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "tonal weight"
      float chroma_shadows;        // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float chroma_highlights;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float chroma_global;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float chroma_midtones;       // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
      float saturation_global;     // $MIN: -5.0 $MAX: 5.0 $DEFAULT: 0.0 $DESCRIPTION: "saturation global"
      float saturation_highlights; // $MIN: -0.2 $MAX: 0.2 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float saturation_midtones;   // $MIN: -0.2 $MAX: 0.2 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
      float saturation_shadows;    // $MIN: -0.2 $MAX: 0.2 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float hue_angle;             // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "hue shift"
    } dt_iop_colorbalancergb_params_v1_t;

    // Init params with defaults
    memcpy(new_params, self->default_params, sizeof(dt_iop_colorbalancergb_params_t));

    // Copy the common part of the params struct
    memcpy(new_params, old_params, sizeof(dt_iop_colorbalancergb_params_v1_t));

    dt_iop_colorbalancergb_params_t *n = (dt_iop_colorbalancergb_params_t *)new_params;
    n->saturation_global /= 180.f / M_PI;
    n->grey_fulcrum = 0.1845f;

    return 0;
  }

  if(old_version == 2 && new_version == 3)
  {
    typedef struct dt_iop_colorbalancergb_params_v2_t
    {
      /* params of v1 */
      float shadows_Y;             // $MIN: -1.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float shadows_C;             // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float shadows_H;             // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float midtones_Y;            // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float midtones_C;            // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float midtones_H;            // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float highlights_Y;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float highlights_C;          // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float highlights_H;          // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float global_Y;              // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "luminance"
      float global_C;              // $MIN:  0.0 $MAX:   1.0 $DEFAULT: 0.0 $DESCRIPTION: "chroma"
      float global_H;              // $MIN:  0.0 $MAX: 360.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
      float shadows_weight;        // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "shadows fall-off"
      float white_fulcrum;         // $MIN: -6.0 $MAX:   6.0 $DEFAULT: 0.0 $DESCRIPTION: "white pivot"
      float highlights_weight;     // $MIN:  0.0 $MAX:   3.0 $DEFAULT: 1.0 $DESCRIPTION: "highlights fall-off"
      float chroma_shadows;        // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float chroma_highlights;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float chroma_global;         // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float chroma_midtones;       // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
      float saturation_global;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float saturation_highlights; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float saturation_midtones;   // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
      float saturation_shadows;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
      float hue_angle;             // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "hue shift"

      /* params of v2 */
      float brilliance_global;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "global"
      float brilliance_highlights; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
      float brilliance_midtones;   // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "midtones"
      float brilliance_shadows;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"

    } dt_iop_colorbalancergb_params_v2_t;

    // Init params with defaults
    memcpy(new_params, self->default_params, sizeof(dt_iop_colorbalancergb_params_t));

    // Copy the common part of the params struct
    memcpy(new_params, old_params, sizeof(dt_iop_colorbalancergb_params_v2_t));

    dt_iop_colorbalancergb_params_t *n = (dt_iop_colorbalancergb_params_t *)new_params;
    n->grey_fulcrum = 0.1845f;

    return 0;
  }
  return 1;
}


/* Custom matrix handling for speed */
static inline void repack_3x3_to_3xSSE(const float input[9], float output[3][4])
{
  // Repack a 3×3 array/matrice into a 3×1 SSE2 vector to enable SSE4/AVX/AVX2 dot products
  output[0][0] = input[0];
  output[0][1] = input[1];
  output[0][2] = input[2];
  output[0][3] = 0.0f;

  output[1][0] = input[3];
  output[1][1] = input[4];
  output[1][2] = input[5];
  output[1][3] = 0.0f;

  output[2][0] = input[6];
  output[2][1] = input[7];
  output[2][2] = input[8];
  output[2][3] = 0.0f;
}


static void mat3mul4(float *dst, const float *const m1, const float *const m2)
{
  for(int k = 0; k < 3; ++k)
  {
    for(int i = 0; i < 3; ++i)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++) x += m1[4 * k + j] * m2[4 * j + i];
      dst[4 * k + i] = x;
    }
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(output, output_comp: 16) uniform(shadows_weight, midtones_weight, highlights_weight)
#endif
static inline void opacity_masks(const float x,
                                 const float shadows_weight, const float highlights_weight,
                                 const float midtones_weight, const float grey_fulcrum,
                                 float output[4], float output_comp[4])
{
  const float x_offset = (x - grey_fulcrum);
  const float x_offset_norm = x_offset / grey_fulcrum;
  const float alpha = 1.f / (1.f + expf(x_offset_norm * shadows_weight));    // opacity of shadows
  const float beta = 1.f / (1.f + expf(-x_offset_norm * highlights_weight)); // opacity of highlights
  const float alpha_comp = 1.f - alpha;
  const float beta_comp = 1.f - beta;
  const float gamma = expf(-sqf(x_offset) * midtones_weight / 4.f) * sqf(alpha_comp) * sqf(beta_comp) * 8.f; // opacity of midtones
  const float gamma_comp = 1.f - gamma;

  output[0] = alpha;
  output[1] = gamma;
  output[2] = beta;
  output[3] = 0.f;

  if(output_comp)
  {
    output_comp[0] = alpha_comp;
    output_comp[1] = gamma_comp;
    output_comp[2] = beta_comp;
    output_comp[3] = 0.f;
  }
}


static inline float soft_clip(const float x, const float soft_threshold, const float hard_threshold)
{
  // use an exponential soft clipping above soft_threshold
  // hard threshold must be > soft threshold
  const float norm = hard_threshold - soft_threshold;
  return (x > soft_threshold) ? soft_threshold + (1.f - expf(-(x - soft_threshold) / norm)) * norm : x;
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorbalancergb_data_t *d = (dt_iop_colorbalancergb_data_t *)piece->data;
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  const struct dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return; // no point

  float DT_ALIGNED_ARRAY RGB_to_XYZ[3][4];
  float DT_ALIGNED_ARRAY XYZ_to_RGB[3][4];

  // repack the matrices as flat AVX2-compliant matrice
  if(work_profile)
  {
    // work profile can't be fetched in commit_params since it is not yet initialised
    repack_3x3_to_3xSSE(work_profile->matrix_in, RGB_to_XYZ);
    repack_3x3_to_3xSSE(work_profile->matrix_out, XYZ_to_RGB);
  }

  // Matrices from CIE 1931 2° XYZ D50 to Filmlight grading RGB D65 through CIE 2006 LMS
  const float XYZ_to_gradRGB[3][4] = { { 0.53346004f,  0.15226970f , -0.19946283f, 0.f },
                                       {-0.67012691f,  1.91752954f,   0.39223917f, 0.f },
                                       { 0.06557547f, -0.07983082f,   0.75036927f, 0.f } };
  const float gradRGB_to_XYZ[3][4] = { { 1.67222161f, -0.11185000f,  0.50297636f, 0.f },
                                       { 0.60120746f,  0.47018395f, -0.08596569f, 0.f },
                                       {-0.08217531f,  0.05979694f,  1.27957582f, 0.f } };

  // Premultiply the pipe RGB -> XYZ and XYZ -> grading RGB matrices to spare 2 matrix products per pixel
  float DT_ALIGNED_ARRAY input_matrix[3][4];
  float DT_ALIGNED_ARRAY output_matrix[3][4];
  mat3mul4((float *)input_matrix, (float *)XYZ_to_gradRGB, (float *)RGB_to_XYZ);
  mat3mul4((float *)output_matrix, (float *)XYZ_to_RGB, (float *)gradRGB_to_XYZ);

  // Test white point of the current space in grading RGB
  const float white_pipe_RGB[4] = { 1.f, 1.f, 1.f };
  float white_grading_RGB[4] = { 0.f };
  dot_product(white_pipe_RGB, input_matrix, white_grading_RGB);
  const float sum_white = white_grading_RGB[0] + white_grading_RGB[1] + white_grading_RGB[2];
  for_four_channels(c) white_grading_RGB[c] /= sum_white;

  const float *const restrict in = __builtin_assume_aligned(((const float *const restrict)ivoid), 64);
  float *const restrict out = __builtin_assume_aligned(((float *const restrict)ovoid), 64);
  const float *const restrict gamut_LUT = __builtin_assume_aligned(((const float *const restrict)d->gamut_LUT), 64);

  const float *const restrict global = __builtin_assume_aligned((const float *const restrict)d->global, 16);
  const float *const restrict highlights = __builtin_assume_aligned((const float *const restrict)d->highlights, 16);
  const float *const restrict shadows = __builtin_assume_aligned((const float *const restrict)d->shadows, 16);
  const float *const restrict midtones = __builtin_assume_aligned((const float *const restrict)d->midtones, 16);

  const float *const restrict chroma = __builtin_assume_aligned((const float *const restrict)d->chroma, 16);
  const float *const restrict saturation = __builtin_assume_aligned((const float *const restrict)d->saturation, 16);
  const float *const restrict brilliance = __builtin_assume_aligned((const float *const restrict)d->brilliance, 16);

  const gint mask_display
      = ((piece->pipe->type & DT_DEV_PIXELPIPE_FULL) == DT_DEV_PIXELPIPE_FULL && self->dev->gui_attached
         && g && g->mask_display);

  // pixel size of the checker background
  const size_t checker_1 = (mask_display) ? DT_PIXEL_APPLY_DPI(d->checker_size) : 0;
  const size_t checker_2 = 2 * checker_1;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, out, roi_in, roi_out, d, g, mask_display, input_matrix, output_matrix, gamut_LUT, white_grading_RGB, \
    global, highlights, shadows, midtones, chroma, saturation, brilliance, checker_1, checker_2) \
    schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < roi_out->height; i++)
  for(size_t j = 0; j < roi_out->width; j++)
  {
    const size_t k = ((i * roi_out->width) + j) * 4;
    const float *const restrict pix_in = __builtin_assume_aligned(in + k, 16);
    float *const restrict pix_out = __builtin_assume_aligned(out + k, 16);

    float DT_ALIGNED_PIXEL Ych[4] = { 0.f };
    float DT_ALIGNED_PIXEL RGB[4] = { 0.f };

    for_four_channels(c, aligned(pix_in:16)) Ych[c] = fmaxf(pix_in[c], 0.0f);
    dot_product(Ych, input_matrix, RGB);
    gradingRGB_to_Ych(RGB, Ych, white_grading_RGB);

    // Sanitize input : no negative luminance
    float Y = fmaxf(Ych[0], 0.f);

    // Opacities for luma masks
    float DT_ALIGNED_PIXEL opacities[4];
    float DT_ALIGNED_PIXEL opacities_comp[4];
    opacity_masks(powf(Y, 0.4101205819200422f), // center middle grey in 50 %
                  d->shadows_weight, d->highlights_weight, d->midtones_weight, d->grey_fulcrum, opacities, opacities_comp);

    // Hue shift - do it now because we need the gamut limit at output hue right after
    Ych[2] += d->hue_angle;

    // Ensure hue ± correction is in [-PI; PI]
    if(Ych[2] > M_PI_F) Ych[2] -= 2.f * M_PI_F;
    else if(Ych[2] < -M_PI_F) Ych[2] += 2.f * M_PI_F;

    // Get max allowed chroma in working RGB gamut at current output hue
    const float max_chroma_h = Y * gamut_LUT[CLAMP((size_t)(LUT_ELEM * (Ych[2] + M_PI_F) / (2.f * M_PI_F)), 0, LUT_ELEM - 1)];

    // Linear chroma : distance to achromatic at constant luminance in scene-referred
    // - in case we desaturate, we do so by a constant factor
    // - in case we resaturate, we normalize the correction by the max chroma allowed at current hue
    //   to prevent users from pushing saturated colors outside of gamut while the low-sat ones
    //   are still muted.
    const float chroma_boost = d->chroma_global + scalar_product(opacities, chroma);
    const float chroma_factor = fmaxf(1.f + chroma_boost, 0.f);
    Ych[1] = soft_clip(Ych[1] * chroma_factor, max_chroma_h, max_chroma_h * 4.f);
    Ych_to_gradingRGB(Ych, RGB, white_grading_RGB);

    // Color balance
    for_four_channels(c, aligned(RGB, opacities, opacities_comp, global, shadows, midtones, highlights:16))
    {
      // global : offset
      RGB[c] += global[c];

      //  highlights, shadows : 2 slopes with masking
      RGB[c] *= opacities_comp[2] * (opacities_comp[0] + opacities[0] * shadows[c]) + opacities[2] * highlights[c];
      // factorization of : (RGB[c] * (1.f - alpha) + RGB[c] * d->shadows[c] * alpha) * (1.f - beta)  + RGB[c] * d->highlights[c] * beta;

      // midtones : power with sign preservation
      const float sign = (RGB[c] < 0.f) ? -1.f : 1.f;
      RGB[c] = sign * powf(fabsf(RGB[c]) / d->white_fulcrum, midtones[c]) * d->white_fulcrum;
    }

    // for the Y midtones power (gamma), we need to go in Ych again because RGB doesn't preserve color
    gradingRGB_to_Ych(RGB, Ych, white_grading_RGB);
    Y = Ych[0] = powf(fmaxf(Ych[0] / d->white_fulcrum, 0.f), d->midtones_Y) * d->white_fulcrum;
    Ych_to_gradingRGB(Ych, RGB, white_grading_RGB);

    // Perceptual color adjustments

    // grading RGB to CIE 1931 XYZ 2° D65
    const float DT_ALIGNED_ARRAY RGB_to_XYZ_D65[3][4] = { { 1.64004888f, -0.10969806f, 0.49329934f, 0.f },
                                                          { 0.61055787f, 0.47749658f, -0.08730269f, 0.f },
                                                          { -0.10698534f, 0.07785058f, 1.66590006f, 0.f } };

    const float DT_ALIGNED_ARRAY XYZ_to_RGB_D65[3][4] = { { 0.54392489f, 0.14993776f, -0.15320716f, 0.f },
                                                          { -0.68327274f, 1.88816348f, 0.30127843f, 0.f },
                                                          { 0.06686186f, -0.07860825f, 0.57635773f, 0.f } };

    // Go to JzAzBz for perceptual saturation
    // We can't use gradingRGB_to_XYZ() since it also does chromatic adaptation to D50
    // and JzAzBz uses D65, same as grading RGB. So we use the matrices above instead
    float DT_ALIGNED_PIXEL Jab[4] = { 0.f };
    dot_product(RGB, RGB_to_XYZ_D65, Ych);
    dt_XYZ_2_JzAzBz(Ych, Jab);

    // Convert to JCh
    float JC[2] = { Jab[0], hypotf(Jab[1], Jab[2]) };               // brightness/chroma vector
    const float h = (JC[1] == 0.f) ? 0.f : atan2f(Jab[2], Jab[1]);  // hue : (a, b) angle

    // Project JC onto S, the saturation eigenvector, with orthogonal vector O.
    // Note : O should be = (C * cosf(T) - J * sinf(T)) = 0 since S is the eigenvector,
    // so we add the chroma projected along the orthogonal axis to get some control value
    const float T = atan2f(JC[1], JC[0]); // angle of the eigenvector over the hue plane
    const float sin_T = sinf(T);
    const float cos_T = cosf(T);
    const float DT_ALIGNED_PIXEL M_rot_dir[2][2] = { {  cos_T,  sin_T },
                                                     { -sin_T,  cos_T } };
    const float DT_ALIGNED_PIXEL M_rot_inv[2][2] = { {  cos_T, -sin_T },
                                                     {  sin_T,  cos_T } };
    float SO[2];

    // brilliance & Saturation : mix of chroma and luminance
    const float boosts[2] = { 1.f + d->brilliance_global + scalar_product(opacities, brilliance),     // move in S direction
                              d->saturation_global + scalar_product(opacities, saturation) }; // move in O direction

    SO[0] = JC[0] * M_rot_dir[0][0] + JC[1] * M_rot_dir[0][1];
    SO[1] = SO[0] * fminf(fmaxf(T * boosts[1], -T), DT_M_PI_F / 2.f - T);
    SO[0] = fmaxf(SO[0] * boosts[0], 0.f);

    // Project back to JCh, that is rotate back of -T angle
    JC[0] = fmaxf(SO[0] * M_rot_inv[0][0] + SO[1] * M_rot_inv[0][1], 0.f);
    JC[1] = fmaxf(SO[0] * M_rot_inv[1][0] + SO[1] * M_rot_inv[1][1], 0.f);

    // Project back to JzAzBz
    Jab[0] = JC[0];
    Jab[1] = JC[1] * cosf(h);
    Jab[2] = JC[1] * sinf(h);

    dt_JzAzBz_2_XYZ(Jab, Ych);
    dot_product(Ych, XYZ_to_RGB_D65, RGB);
    gradingRGB_to_Ych(RGB, Ych, white_grading_RGB);

    // Gamut mapping
    // Note : no need to check hue is in [-PI; PI], gradingRGB_to_Ych uses atan2f()
    // which always returns angles in [-PI; PI]
    const float out_max_chroma_h = Ych[0] * gamut_LUT[CLAMP((size_t)(LUT_ELEM * (Ych[2] + M_PI_F) / (2.f * M_PI_F)), 0, LUT_ELEM - 1)];
    Ych[1] = soft_clip(Ych[1], out_max_chroma_h, out_max_chroma_h * 4.f);

    Ych_to_gradingRGB(Ych, RGB, white_grading_RGB);
    dot_product(RGB, output_matrix, pix_out);
    if(mask_display)
    {
      // draw checkerboard
      float DT_ALIGNED_PIXEL color[4];
      if(i % checker_1 < i % checker_2)
      {
        if(j % checker_1 < j % checker_2) for_four_channels(c) color[c] = d->checker_color_2[c];
        else for_four_channels(c) color[c] = d->checker_color_1[c];
      }
      else
      {
        if(j % checker_1 < j % checker_2) for_four_channels(c) color[c] = d->checker_color_1[c];
        else for_four_channels(c) color[c] = d->checker_color_2[c];
      }

      float opacity = opacities[g->mask_type];
      const float opacity_comp = 1.0f - opacity;

      for_four_channels(c, aligned(pix_out, color:16)) pix_out[c] = opacity_comp * color[c] + opacity * fmaxf(pix_out[c], 0.f);
      pix_out[3] = 1.0f; // alpha is opaque, we need to preview it
    }
    else
    {
      for_four_channels(c, aligned(pix_out:16)) pix_out[c] = fmaxf(pix_out[c], 0.f);
      pix_out[3] = pix_in[3]; // alpha copy
    }
  }
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalancergb_data_t *d = (dt_iop_colorbalancergb_data_t *)(piece->data);
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)p1;

  d->checker_color_1[0] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/red"), 0.f, 1.f);
  d->checker_color_1[1] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/green"), 0.f, 1.f);
  d->checker_color_1[2] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/blue"), 0.f, 1.f);
  d->checker_color_1[3] = 1.f;

  d->checker_color_2[0] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/red"), 0.f, 1.f);
  d->checker_color_2[1] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/green"), 0.f, 1.f);
  d->checker_color_2[2] = CLAMP(dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/blue"), 0.f, 1.f);
  d->checker_color_2[3] = 1.f;

  d->checker_size = MAX(dt_conf_get_int("plugins/darkroom/colorbalancergb/checker/size"), 2);

  d->chroma_global = p->chroma_global;
  d->chroma[0] = p->chroma_shadows;
  d->chroma[1] = p->chroma_midtones;
  d->chroma[2] = p->chroma_highlights;
  d->chroma[3] = 0.f;

  d->saturation_global = p->saturation_global;
  d->saturation[0] = p->saturation_shadows;
  d->saturation[1] = p->saturation_midtones;
  d->saturation[2] = p->saturation_highlights;
  d->saturation[3] = 0.f;

  d->brilliance_global = p->brilliance_global;
  d->brilliance[0] = p->brilliance_shadows;
  d->brilliance[1] = p->brilliance_midtones;
  d->brilliance[2] = p->brilliance_highlights;
  d->brilliance[3] = 0.f;

  d->hue_angle = M_PI * p->hue_angle / 180.f;

  // measure the grading RGB of a pure white
  const float Ych_norm[4] = { 1.f, 0.f, 0.f, 0.f };
  float RGB_norm[4] = { 0.f };
  Ych_to_gradingRGB(Ych_norm, RGB_norm, NULL);

  // global
  {
    float Ych[4] = { 1.f, p->global_C, DEG_TO_RAD(p->global_H), 0.f };
    Ych_to_gradingRGB(Ych, d->global, NULL);
    for(size_t c = 0; c < 4; c++) d->global[c] = (d->global[c] - RGB_norm[c]) + RGB_norm[c] * p->global_Y;
  }

  // shadows
  {
    float Ych[4] = { 1.f, p->shadows_C, DEG_TO_RAD(p->shadows_H), 0.f };
    Ych_to_gradingRGB(Ych, d->shadows, NULL);
    for(size_t c = 0; c < 4; c++) d->shadows[c] = 1.f + (d->shadows[c] - RGB_norm[c]) + p->shadows_Y;
    d->shadows_weight = 2.f + p->shadows_weight * 2.f;
  }

  // highlights
  {
    float Ych[4] = { 1.f, p->highlights_C, DEG_TO_RAD(p->highlights_H), 0.f };
    Ych_to_gradingRGB(Ych, d->highlights, NULL);
    for(size_t c = 0; c < 4; c++) d->highlights[c] = 1.f + (d->highlights[c] - RGB_norm[c]) + p->highlights_Y;
    d->highlights_weight = 2.f + p->highlights_weight * 2.f;
  }

  // midtones
  {
    float Ych[4] = { 1.f, p->midtones_C, DEG_TO_RAD(p->midtones_H), 0.f };
    Ych_to_gradingRGB(Ych, d->midtones, NULL);
    for(size_t c = 0; c < 4; c++) d->midtones[c] = 1.f / (1.f + (d->midtones[c] - RGB_norm[c]));
    d->midtones_Y = 1.f / (1.f + p->midtones_Y);
    d->white_fulcrum = exp2f(p->white_fulcrum);
    d->midtones_weight = sqf(d->shadows_weight) * sqf(d->highlights_weight) /
      (sqf(d->shadows_weight) + sqf(d->highlights_weight));
    d->grey_fulcrum = powf(p->grey_fulcrum, 0.4101205819200422f);
  }

  // Check if the RGB working profile has changed in pipe
  // WARNING: this function is not triggered upon working profile change,
  // so the gamut boundaries are wrong until we change some param in this module
  struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return;
  if(work_profile != d->work_profile)
  {
    d->lut_inited = FALSE;
    d->work_profile = work_profile;
  }

  // find the maximum chroma allowed by the current working gamut in conjunction to hue
  // this will be used to prevent users to mess up their images by pushing chroma out of gamut
  if(!d->lut_inited && d->gamut_LUT)
  {
    float *const restrict LUT = d->gamut_LUT;

    // init the LUT between -pi and pi by increments of 1°
    for(size_t k = 0; k < LUT_ELEM; k++) LUT[k] = 0.f;

    // Premultiply the matrix to speed-up
    float DT_ALIGNED_ARRAY RGB_to_XYZ[3][4];
    repack_3x3_to_3xSSE(work_profile->matrix_in, RGB_to_XYZ);
    const float XYZ_to_gradingRGB[3][4] = { { 0.53346004f,  0.15226970f , -0.19946283f, 0.f },
                                            {-0.67012691f,  1.91752954f,   0.39223917f, 0.f },
                                            { 0.06557547f, -0.07983082f,   0.75036927f, 0.f } };
    float DT_ALIGNED_ARRAY input_matrix[3][4];
    mat3mul4((float *)input_matrix, (float *)XYZ_to_gradingRGB, (float *)RGB_to_XYZ);

    // Test white point of the current space in grading RGB
    const float white_pipe_RGB[4] = { 1.f, 1.f, 1.f };
    float white_grading_RGB[4] = { 0.f };
    dot_product(white_pipe_RGB, input_matrix, white_grading_RGB);
    const float sum_white = white_grading_RGB[0] + white_grading_RGB[1] + white_grading_RGB[2];
    for_four_channels(c) white_grading_RGB[c] /= sum_white;

    // make RGB values vary between [0; 1] in working space, convert to Ych and get the max(c(h)))
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(input_matrix, white_grading_RGB) schedule(static) dt_omp_sharedconst(LUT)
#endif
    for(size_t r = 0; r < STEPS; r++)
      for(size_t g = 0; g < STEPS; g++)
        for(size_t b = 0; b < STEPS; b++)
        {
          const float DT_ALIGNED_PIXEL rgb[4] = { (float)r / (float)(STEPS - 1),
                                                  (float)g / (float)(STEPS - 1),
                                                  (float)b / (float)(STEPS - 1),
                                                  0.f };

          float DT_ALIGNED_PIXEL RGB[4] = { 0.f };
          float DT_ALIGNED_PIXEL Ych[4] = { 0.f };
          dot_product(rgb, input_matrix, RGB);
          gradingRGB_to_Ych(RGB, Ych, white_grading_RGB);
          const size_t index = CLAMP((size_t)(LUT_ELEM * (Ych[2] + M_PI_F) / (2.f * M_PI_F)), 0, LUT_ELEM - 1);
          const float saturation = (Ych[0] > 0.f) ? Ych[1] / Ych[0] : 0.f;
          if(LUT[index] < saturation) LUT[index] = saturation;
        }

    d->lut_inited = TRUE;
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_colorbalancergb_data_t));
  dt_iop_colorbalancergb_data_t *d = (dt_iop_colorbalancergb_data_t *)(piece->data);
  d->gamut_LUT = NULL;
  d->gamut_LUT = dt_alloc_sse_ps(LUT_ELEM);
  d->lut_inited = FALSE;
  d->work_profile = NULL;
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalancergb_data_t *d = (dt_iop_colorbalancergb_data_t *)(piece->data);
  if(d->gamut_LUT) dt_free_align(d->gamut_LUT);
  free(piece->data);
  piece->data = NULL;
}

void pipe_RGB_to_Ych(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float RGB[4], float Ych[4])
{
  const struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return; // no point

  float XYZ[4] = { 0.f };
  float LMS[4] = { 0.f };

  dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ, work_profile->matrix_in, work_profile->lut_in,
                             work_profile->unbounded_coeffs_in, work_profile->lutsize,
                             work_profile->nonlinearlut);
  XYZ_to_gradingRGB(XYZ, LMS);
  gradingRGB_to_Ych(LMS, Ych, NULL);

  if(Ych[2] < 0.f)
    Ych[2] = 2.f * M_PI + Ych[2];
}


void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)self->params;

  float Ych[4] = { 0.f };
  float max_Ych[4] = { 0.f };
  pipe_RGB_to_Ych(self, piece, (const float *)self->picked_color, Ych);
  pipe_RGB_to_Ych(self, piece, (const float *)self->picked_color_max, max_Ych);
  float hue = RAD_TO_DEG(Ych[2]) + 180.f;   // take the opponent color
  hue = (hue > 360.f) ? hue - 360.f : hue;  // normalize in [0 ; 360]°

  ++darktable.gui->reset;
  if(picker == g->global_H)
  {
    p->global_H = hue;
    p->global_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set_soft(g->global_H, p->global_H);
    dt_bauhaus_slider_set_soft(g->global_C, p->global_C);
  }
  else if(picker == g->shadows_H)
  {
    p->shadows_H = hue;
    p->shadows_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set_soft(g->shadows_H, p->shadows_H);
    dt_bauhaus_slider_set_soft(g->shadows_C, p->shadows_C);
  }
  else if(picker == g->midtones_H)
  {
    p->midtones_H = hue;
    p->midtones_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set_soft(g->midtones_H, p->midtones_H);
    dt_bauhaus_slider_set_soft(g->midtones_C, p->midtones_C);
  }
  else if(picker == g->highlights_H)
  {
    p->highlights_H = hue;
    p->highlights_C = Ych[1] * Ych[0];
    dt_bauhaus_slider_set_soft(g->highlights_H, p->highlights_H);
    dt_bauhaus_slider_set_soft(g->highlights_C, p->highlights_C);
  }
  else if(picker == g->white_fulcrum)
  {
    p->white_fulcrum = log2f(max_Ych[0]);
    dt_bauhaus_slider_set_soft(g->white_fulcrum, p->white_fulcrum);
  }
  else
    fprintf(stderr, "[colorbalancergb] unknown color picker\n");
  --darktable.gui->reset;

  gui_changed(self, picker, NULL);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void paint_chroma_slider(GtkWidget *w, const float hue)
{
  const float x_min = DT_BAUHAUS_WIDGET(w)->data.slider.soft_min;
  const float x_max = DT_BAUHAUS_WIDGET(w)->data.slider.soft_max;
  const float x_range = x_max - x_min;

  // Varies x in range around current y param
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float x = x_min + stop * x_range;
    const float h = DEG_TO_RAD(hue);

    float RGB[4] = { 0.f };
    float Ych[4] = { 0.75f, x, h, 0.f };
    float LMS[4] = { 0.f };
    Ych_to_gradingRGB(Ych, LMS, NULL);
    gradingRGB_to_XYZ(LMS, Ych);
    dt_XYZ_to_Rec709_D65(Ych, RGB);
    const float max_RGB = fmaxf(fmaxf(RGB[0], RGB[1]), RGB[2]);
    for(size_t c = 0; c < 3; c++) RGB[c] = powf(RGB[c] / max_RGB, 1.f / 2.2f);
    dt_bauhaus_slider_set_stop(w, stop, RGB[0], RGB[1], RGB[2]);
  }

  gtk_widget_queue_draw(w);
}


static void mask_callback(GtkWidget *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_request_focus(self);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);

  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;

  // if blend module is displaying mask do not display it here
  if(self->request_mask_display)
  {
    dt_control_log(_("cannot display masks when the blending mask is displayed"));
    g->mask_display = 0;
  }
  else
  {
    g->mask_display = dt_bauhaus_widget_get_quad_active(GTK_WIDGET(togglebutton));
  }

  if(g->mask_display)
  {
    if(togglebutton == g->shadows_weight) g->mask_type = MASK_SHADOWS;
    if(togglebutton == g->grey_fulcrum) g->mask_type = MASK_MIDTONES;
    if(togglebutton == g->highlights_weight) g->mask_type = MASK_HIGHLIGHTS;
  }
  else
  {
    g->mask_type = MASK_NONE;
  }

  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->shadows_weight), g->mask_type == MASK_SHADOWS);
  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->grey_fulcrum), g->mask_type == MASK_MIDTONES);
  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->highlights_weight), g->mask_type == MASK_HIGHLIGHTS);

  dt_iop_refresh_center(self);
}


static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)self->params;
  const float shadows_weight = 2.f + p->shadows_weight * 2.f;
  const float highlights_weight = 2.f + p->highlights_weight * 2.f;

  const float aspect = dt_conf_get_int("plugins/darkroom/colorbalancergb/aspect_percent") / 100.0;
  dtgtk_drawing_area_set_aspect_ratio(widget, aspect);

  // Cache the graph objects to avoid recomputing all the view at each redraw
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  cairo_surface_t *cst =
    dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  PangoFontDescription *desc =
    pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);

  const gint font_size = pango_font_description_get_size(desc);
  pango_font_description_set_size(desc, 0.95 * font_size);
  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);

  char text[256];

  // Get the text line height for spacing
  PangoRectangle ink;
  snprintf(text, sizeof(text), "X");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  const float line_height = ink.height;

  const float inset = DT_PIXEL_APPLY_DPI(4);
  const float margin_top = inset;
  const float margin_bottom = line_height + 2 * inset;
  const float margin_left = line_height + inset;
  const float margin_right = 0;

  const float graph_width = allocation.width - margin_right - margin_left;   // align the right border on sliders
  const float graph_height = allocation.height - margin_bottom - margin_top; // give room to nodes

  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);

  // draw x gradient as axis legend
  cairo_pattern_t *grad;
  grad = cairo_pattern_create_linear(margin_left, 0.0, graph_width, 0.0);
  dt_cairo_perceptual_gradient(grad, 1.0);
  cairo_set_line_width(cr, 0.0);
  cairo_rectangle(cr, margin_left, graph_height + 2 * inset, graph_width, line_height);
  cairo_set_source(cr, grad);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  // draw y gradient as axis legend
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, line_height);
  unsigned char *data = malloc(stride * graph_height);
  cairo_surface_t *surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, (size_t)line_height, (size_t)graph_height, stride);

  const size_t checker_1 = DT_PIXEL_APPLY_DPI(6);
  const size_t checker_2 = 2 * checker_1;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(data, graph_height, line_height, checker_1, checker_2) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < (size_t)graph_height; i++)
    for(size_t j = 0; j < (size_t)line_height; j++)
    {
      const size_t k = ((i * (size_t)line_height) + j) * 4;
      unsigned char color;
      const float alpha = (float)i / graph_height;
      if(i % checker_1 < i % checker_2)
      {
        if(j % checker_1 < j % checker_2) color = 150;
        else color = 100;
      }
      else
      {
        if(j % checker_1 < j % checker_2) color = 100;
        else color = 150;
      }

      for(size_t c = 0; c < 4; ++c) data[k + c] = color * alpha;
      data[k+3] = alpha * 255;
    }

  cairo_set_source_surface(cr, surface, 0, margin_top);
  cairo_paint(cr);
  free(data);
  cairo_surface_destroy(surface);

  // set the graph as the origin of the coordinates
  cairo_translate(cr, margin_left, margin_top);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_rectangle(cr, 0, 0, graph_width, graph_height);
  cairo_fill_preserve(cr);
  cairo_clip(cr);

  // from https://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.583.3007&rep=rep1&type=pdf
  const float midtones_weight
      = sqf(shadows_weight) * sqf(highlights_weight) / (sqf(shadows_weight) + sqf(highlights_weight));
  const float grey_fulcrum = powf(p->grey_fulcrum, 0.4101205819200422f);

  float *LUT[3];
  for(size_t c = 0; c < 3; c++) LUT[c] = dt_alloc_align_float(LUT_ELEM);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(LUT, shadows_weight, midtones_weight, highlights_weight, grey_fulcrum) \
  schedule(static)
#endif
  for(size_t k = 0 ; k < LUT_ELEM; k++)
  {
    const float Y = k / (float)(LUT_ELEM - 1);
    float output[4];
    opacity_masks(Y, shadows_weight, highlights_weight, midtones_weight, grey_fulcrum, output, NULL);
    for(size_t c = 0; c < 3; c++) LUT[c][k] = output[c];
  }

  GdkRGBA fg_color = darktable.bauhaus->graph_fg;
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));

  for(size_t c = 0; c < 3; c++)
  {
    GdkRGBA line_color = { fg_color.red * (1. - (2 - c) / 4.),
                           fg_color.green * (1. - (2 - c) / 4.),
                           fg_color.blue * (1. - (2 - c) / 4.),
                           fg_color.alpha };
    set_color(cr, line_color);

    cairo_move_to(cr, 0, (1.f - LUT[c][0]) * graph_height);
    for(size_t k = 0; k < LUT_ELEM; k++)
    {
      const float x = (float)k / (float)(LUT_ELEM - 1) * graph_width;
      const float y = (1.f - LUT[c][k]) * graph_height;
      cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
  }

  for(size_t c = 0; c < 3; c++) dt_free_align(LUT[c]);

  cairo_restore(cr);

  // restore font size
  pango_font_description_set_size(desc, font_size);
  pango_layout_set_font_description(layout, desc);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  g_object_unref(layout);
  pango_font_description_free(desc);
  return TRUE;
}


static void checker_1_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  GdkRGBA color;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &color);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/red", color.red);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/green", color.green);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/blue", color.blue);
  dt_iop_refresh_center(self);
}


static void checker_2_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  GdkRGBA color;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &color);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/red", color.red);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/green", color.green);
  dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/blue", color.blue);
  dt_iop_refresh_center(self);
}


static void checker_size_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  const size_t size = dt_bauhaus_slider_get(widget);
  dt_conf_set_int("plugins/darkroom/colorbalancergb/checker/size", size);
  dt_iop_refresh_center(self);
}


void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)self->params;

   ++darktable.gui->reset;

  if(!w || w == g->global_H)
    paint_chroma_slider(g->global_C, p->global_H);

  if(!w || w == g->shadows_H)
    paint_chroma_slider(g->shadows_C, p->shadows_H);

  if(!w || w == g->midtones_H)
    paint_chroma_slider(g->midtones_C, p->midtones_H);

  if(!w || w == g->highlights_H)
    paint_chroma_slider(g->highlights_C, p->highlights_H);

  if(!w || w == g->shadows_weight || w == g->highlights_weight || w == g->grey_fulcrum)
    gtk_widget_queue_draw(GTK_WIDGET(g->area));

  --darktable.gui->reset;

}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_colorbalancergb_params_t *p = (dt_iop_colorbalancergb_params_t *)self->params;

  dt_bauhaus_slider_set_soft(g->hue_angle, p->hue_angle);

  dt_bauhaus_slider_set_soft(g->chroma_global, p->chroma_global);
  dt_bauhaus_slider_set_soft(g->chroma_highlights, p->chroma_highlights);
  dt_bauhaus_slider_set_soft(g->chroma_midtones, p->chroma_midtones);
  dt_bauhaus_slider_set_soft(g->chroma_shadows, p->chroma_shadows);

  dt_bauhaus_slider_set_soft(g->saturation_global, p->saturation_global);
  dt_bauhaus_slider_set_soft(g->saturation_highlights, p->saturation_highlights);
  dt_bauhaus_slider_set_soft(g->saturation_midtones, p->saturation_midtones);
  dt_bauhaus_slider_set_soft(g->saturation_shadows, p->saturation_shadows);

  dt_bauhaus_slider_set_soft(g->brilliance_global, p->brilliance_global);
  dt_bauhaus_slider_set_soft(g->brilliance_highlights, p->brilliance_highlights);
  dt_bauhaus_slider_set_soft(g->brilliance_midtones, p->brilliance_midtones);
  dt_bauhaus_slider_set_soft(g->brilliance_shadows, p->brilliance_shadows);

  dt_bauhaus_slider_set_soft(g->global_C, p->global_C);
  dt_bauhaus_slider_set_soft(g->global_H, p->global_H);
  dt_bauhaus_slider_set_soft(g->global_Y, p->global_Y);

  dt_bauhaus_slider_set_soft(g->shadows_C, p->shadows_C);
  dt_bauhaus_slider_set_soft(g->shadows_H, p->shadows_H);
  dt_bauhaus_slider_set_soft(g->shadows_Y, p->shadows_Y);
  dt_bauhaus_slider_set_soft(g->shadows_weight, p->shadows_weight);

  dt_bauhaus_slider_set_soft(g->midtones_C, p->midtones_C);
  dt_bauhaus_slider_set_soft(g->midtones_H, p->midtones_H);
  dt_bauhaus_slider_set_soft(g->midtones_Y, p->midtones_Y);
  dt_bauhaus_slider_set_soft(g->white_fulcrum, p->white_fulcrum);

  dt_bauhaus_slider_set_soft(g->highlights_C, p->highlights_C);
  dt_bauhaus_slider_set_soft(g->highlights_H, p->highlights_H);
  dt_bauhaus_slider_set_soft(g->highlights_Y, p->highlights_Y);
  dt_bauhaus_slider_set_soft(g->highlights_weight, p->highlights_weight);

  dt_bauhaus_slider_set_soft(g->grey_fulcrum, p->grey_fulcrum);

  gui_changed(self, NULL, NULL);
  dt_iop_color_picker_reset(self, TRUE);
  g->mask_display = FALSE;
  g->mask_type = MASK_NONE;

  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->shadows_weight), FALSE);
  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->grey_fulcrum), FALSE);
  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->highlights_weight), FALSE);

  // Checkerboard mask preview preferences
  GdkRGBA color;
  color.alpha = 1.0f;
  color.red = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/red");
  color.green = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/green");
  color.blue = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker1/blue");

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->checker_color_1_picker), &color);

  color.red = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/red");
  color.green = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/green");
  color.blue = dt_conf_get_float("plugins/darkroom/colorbalancergb/checker2/blue");

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->checker_color_2_picker), &color);

  dt_bauhaus_slider_set_soft(g->checker_size, dt_conf_get_int("plugins/darkroom/colorbalancergb/checker/size"));
}


void gui_reset(dt_iop_module_t *self)
{
  //dt_iop_colorbalancergb_gui_data_t *g = (dt_iop_colorbalancergb_gui_data_t *)self->gui_data;
  dt_iop_color_picker_reset(self, TRUE);
}

static gboolean area_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  if(dt_gui_ignore_scroll(event)) return FALSE;

  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
    {
      //adjust aspect
      const int aspect = dt_conf_get_int("plugins/darkroom/colorbalancergb/aspect_percent");
      dt_conf_set_int("plugins/darkroom/colorbalancergb/aspect_percent", aspect + delta_y);
      gtk_widget_queue_draw(widget);

      return TRUE;
    }
  }

  return FALSE;
}


void gui_init(dt_iop_module_t *self)
{
  dt_iop_colorbalancergb_gui_data_t *g = IOP_GUI_ALLOC(colorbalancergb);
  g->mask_display = FALSE;

  // start building top level widget
  g->notebook = GTK_NOTEBOOK(gtk_notebook_new());

  // Page master
  self->widget = dt_ui_notebook_page(g->notebook, _("master"), _("global grading"));

  g->hue_angle = dt_bauhaus_slider_from_params(self, "hue_angle");
  dt_bauhaus_slider_set_digits(g->hue_angle, 4);
  dt_bauhaus_slider_set_step(g->hue_angle, 1.);
  dt_bauhaus_slider_set_format(g->hue_angle, "%.2f °");
  gtk_widget_set_tooltip_text(g->hue_angle, _("rotate all hues by an angle, at the same luminance"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("linear chroma grading")), FALSE, FALSE, 0);

  g->chroma_global = dt_bauhaus_slider_from_params(self, "chroma_global");
  dt_bauhaus_slider_set_soft_range(g->chroma_global, -0.5, 0.5);
  dt_bauhaus_slider_set_digits(g->chroma_global, 4);
  dt_bauhaus_slider_set_factor(g->chroma_global, 100.0f);
  dt_bauhaus_slider_set_format(g->chroma_global, "%.2f %%");
  gtk_widget_set_tooltip_text(g->chroma_global, _("increase colorfulness at same luminance globally"));

  g->chroma_shadows = dt_bauhaus_slider_from_params(self, "chroma_shadows");
  dt_bauhaus_slider_set_digits(g->chroma_shadows, 4);
  dt_bauhaus_slider_set_factor(g->chroma_shadows, 100.0f);
  dt_bauhaus_slider_set_format(g->chroma_shadows, "%.2f %%");
  gtk_widget_set_tooltip_text(g->chroma_shadows, _("increase colorfulness at same luminance mostly in shadows"));

  g->chroma_midtones = dt_bauhaus_slider_from_params(self, "chroma_midtones");
  dt_bauhaus_slider_set_digits(g->chroma_midtones, 4);
  dt_bauhaus_slider_set_factor(g->chroma_midtones, 100.0f);
  dt_bauhaus_slider_set_format(g->chroma_midtones, "%.2f %%");
  gtk_widget_set_tooltip_text(g->chroma_midtones, _("increase colorfulness at same luminance mostly in midtones"));

  g->chroma_highlights = dt_bauhaus_slider_from_params(self, "chroma_highlights");
  dt_bauhaus_slider_set_digits(g->chroma_highlights, 4);
  dt_bauhaus_slider_set_factor(g->chroma_highlights, 100.0f);
  dt_bauhaus_slider_set_format(g->chroma_highlights, "%.2f %%");
  gtk_widget_set_tooltip_text(g->chroma_highlights, _("increase colorfulness at same luminance mostly in highlights"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("perceptual saturation grading")), FALSE, FALSE, 0);

  g->saturation_global = dt_bauhaus_slider_from_params(self, "saturation_global");
  dt_bauhaus_slider_set_soft_range(g->saturation_global, -0.25, 0.25);
  dt_bauhaus_slider_set_digits(g->saturation_global, 4);
  dt_bauhaus_slider_set_factor(g->saturation_global, 100.0f);
  dt_bauhaus_slider_set_format(g->saturation_global, "%.2f %%");
  gtk_widget_set_tooltip_text(g->saturation_global, _("add or remove saturation by an absolute amount"));

  g->saturation_shadows = dt_bauhaus_slider_from_params(self, "saturation_shadows");
  dt_bauhaus_slider_set_soft_range(g->saturation_shadows, -0.25, 0.25);
  dt_bauhaus_slider_set_digits(g->saturation_shadows, 4);
  dt_bauhaus_slider_set_factor(g->saturation_shadows, 100.0f);
  dt_bauhaus_slider_set_format(g->saturation_shadows, "%.2f %%");
  gtk_widget_set_tooltip_text(g->saturation_shadows, _("increase or decrease saturation proportionally to the original pixel saturation"));

  g->saturation_midtones= dt_bauhaus_slider_from_params(self, "saturation_midtones");
  dt_bauhaus_slider_set_soft_range(g->saturation_midtones, -0.25, 0.25);
  dt_bauhaus_slider_set_digits(g->saturation_midtones, 4);
  dt_bauhaus_slider_set_factor(g->saturation_midtones, 100.0f);
  dt_bauhaus_slider_set_format(g->saturation_midtones, "%.2f %%");
  gtk_widget_set_tooltip_text(g->saturation_midtones, _("increase or decrease saturation proportionally to the original pixel saturation"));

  g->saturation_highlights = dt_bauhaus_slider_from_params(self, "saturation_highlights");
  dt_bauhaus_slider_set_soft_range(g->saturation_highlights, -0.25, 0.25);
  dt_bauhaus_slider_set_digits(g->saturation_highlights, 4);
  dt_bauhaus_slider_set_factor(g->saturation_highlights, 100.0f);
  dt_bauhaus_slider_set_format(g->saturation_highlights, "%.2f %%");
  gtk_widget_set_tooltip_text(g->saturation_highlights, _("increase or decrease saturation proportionally to the original pixel saturation"));


  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("perceptual brilliance grading")), FALSE, FALSE, 0);

  g->brilliance_global = dt_bauhaus_slider_from_params(self, "brilliance_global");
  dt_bauhaus_slider_set_soft_range(g->brilliance_global, -0.25, 0.25);
  dt_bauhaus_slider_set_digits(g->brilliance_global, 4);
  dt_bauhaus_slider_set_factor(g->brilliance_global, 100.0f);
  dt_bauhaus_slider_set_format(g->brilliance_global, "%.2f %%");
  gtk_widget_set_tooltip_text(g->brilliance_global, _("add or remove brilliance by an absolute amount"));

  g->brilliance_shadows = dt_bauhaus_slider_from_params(self, "brilliance_shadows");
  dt_bauhaus_slider_set_soft_range(g->brilliance_shadows, -0.25, 0.25);
  dt_bauhaus_slider_set_digits(g->brilliance_shadows, 4);
  dt_bauhaus_slider_set_factor(g->brilliance_shadows, 100.0f);
  dt_bauhaus_slider_set_format(g->brilliance_shadows, "%.2f %%");
  gtk_widget_set_tooltip_text(g->brilliance_shadows, _("increase or decrease brilliance proportionally to the original pixel brilliance"));

  g->brilliance_midtones= dt_bauhaus_slider_from_params(self, "brilliance_midtones");
  dt_bauhaus_slider_set_soft_range(g->brilliance_midtones, -0.25, 0.25);
  dt_bauhaus_slider_set_digits(g->brilliance_midtones, 4);
  dt_bauhaus_slider_set_factor(g->brilliance_midtones, 100.0f);
  dt_bauhaus_slider_set_format(g->brilliance_midtones, "%.2f %%");
  gtk_widget_set_tooltip_text(g->brilliance_midtones, _("increase or decrease brilliance proportionally to the original pixel brilliance"));

  g->brilliance_highlights = dt_bauhaus_slider_from_params(self, "brilliance_highlights");
  dt_bauhaus_slider_set_soft_range(g->brilliance_highlights, -0.25, 0.25);
  dt_bauhaus_slider_set_digits(g->brilliance_highlights, 4);
  dt_bauhaus_slider_set_factor(g->brilliance_highlights, 100.0f);
  dt_bauhaus_slider_set_format(g->brilliance_highlights, "%.2f %%");
  gtk_widget_set_tooltip_text(g->brilliance_highlights, _("increase or decrease brilliance proportionally to the original pixel brilliance"));


  // Page 4-ways
  self->widget = dt_ui_notebook_page(g->notebook, _("4 ways"), _("selective color grading"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("global offset")), FALSE, FALSE, 0);

  g->global_Y = dt_bauhaus_slider_from_params(self, "global_Y");
  dt_bauhaus_slider_set_soft_range(g->global_Y, -0.05, 0.05);
  dt_bauhaus_slider_set_factor(g->global_Y, 100.0f);
  dt_bauhaus_slider_set_digits(g->global_Y, 4);
  dt_bauhaus_slider_set_format(g->global_Y, "%.2f %%");
  gtk_widget_set_tooltip_text(g->global_Y, _("global luminance offset"));

  g->global_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "global_H"));
  dt_bauhaus_slider_set_feedback(g->global_H, 0);
  dt_bauhaus_slider_set_step(g->global_H, 10.);
  dt_bauhaus_slider_set_digits(g->global_H, 4);
  dt_bauhaus_slider_set_format(g->global_H, "%.2f °");
  gtk_widget_set_tooltip_text(g->global_H, _("hue of the global color offset"));

  g->global_C = dt_bauhaus_slider_from_params(self, "global_C");
  dt_bauhaus_slider_set_soft_range(g->global_C, 0., 0.005);
  dt_bauhaus_slider_set_digits(g->global_C, 4);
  dt_bauhaus_slider_set_factor(g->global_C, 100.0f);
  dt_bauhaus_slider_set_format(g->global_C, "%.2f %%");
  gtk_widget_set_tooltip_text(g->global_C, _("chroma of the global color offset"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("shadows lift")), FALSE, FALSE, 0);

  g->shadows_Y = dt_bauhaus_slider_from_params(self, "shadows_Y");
  dt_bauhaus_slider_set_soft_range(g->shadows_Y, -0.5, 0.5);
  dt_bauhaus_slider_set_factor(g->shadows_Y, 100.0f);
  dt_bauhaus_slider_set_digits(g->shadows_Y, 4);
  dt_bauhaus_slider_set_format(g->shadows_Y, "%.2f %%");
  gtk_widget_set_tooltip_text(g->shadows_Y, _("luminance gain in shadows"));

  g->shadows_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "shadows_H"));
  dt_bauhaus_slider_set_feedback(g->shadows_H, 0);
  dt_bauhaus_slider_set_step(g->shadows_H, 10.);
  dt_bauhaus_slider_set_digits(g->shadows_H, 4);
  dt_bauhaus_slider_set_format(g->shadows_H, "%.2f °");
  gtk_widget_set_tooltip_text(g->shadows_H, _("hue of the color gain in shadows"));

  g->shadows_C = dt_bauhaus_slider_from_params(self, "shadows_C");
  dt_bauhaus_slider_set_soft_range(g->shadows_C, 0., 0.1);
  dt_bauhaus_slider_set_step(g->shadows_C, 0.01);
  dt_bauhaus_slider_set_digits(g->shadows_C, 4);
  dt_bauhaus_slider_set_factor(g->shadows_C, 100.0f);
  dt_bauhaus_slider_set_format(g->shadows_C, "%.2f %%");
  gtk_widget_set_tooltip_text(g->shadows_C, _("chroma of the color gain in shadows"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("highlights gain")), FALSE, FALSE, 0);

  g->highlights_Y = dt_bauhaus_slider_from_params(self, "highlights_Y");
  dt_bauhaus_slider_set_soft_range(g->highlights_Y, -0.5, 0.5);
  dt_bauhaus_slider_set_factor(g->highlights_Y, 100.0f);
  dt_bauhaus_slider_set_digits(g->highlights_Y, 4);
  dt_bauhaus_slider_set_format(g->highlights_Y, "%.2f %%");
  gtk_widget_set_tooltip_text(g->highlights_Y, _("luminance gain in highlights"));

  g->highlights_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "highlights_H"));
  dt_bauhaus_slider_set_feedback(g->highlights_H, 0);
  dt_bauhaus_slider_set_step(g->highlights_H, 10.);
  dt_bauhaus_slider_set_digits(g->highlights_H, 4);
  dt_bauhaus_slider_set_format(g->highlights_H, "%.2f °");
  gtk_widget_set_tooltip_text(g->highlights_H, _("hue of the color gain in highlights"));

  g->highlights_C = dt_bauhaus_slider_from_params(self, "highlights_C");
  dt_bauhaus_slider_set_soft_range(g->highlights_C, 0., 0.05);
  dt_bauhaus_slider_set_step(g->shadows_C, 0.01);
  dt_bauhaus_slider_set_digits(g->highlights_C, 4);
  dt_bauhaus_slider_set_factor(g->highlights_C, 100.0f);
  dt_bauhaus_slider_set_format(g->highlights_C, "%.2f %%");
  gtk_widget_set_tooltip_text(g->highlights_C, _("chroma of the color gain in highlights"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("power")), FALSE, FALSE, 0);

  g->midtones_Y = dt_bauhaus_slider_from_params(self, "midtones_Y");
  dt_bauhaus_slider_set_soft_range(g->midtones_Y, -0.25, 0.25);
  dt_bauhaus_slider_set_factor(g->midtones_Y, 100.0f);
  dt_bauhaus_slider_set_digits(g->midtones_Y, 4);
  dt_bauhaus_slider_set_format(g->midtones_Y, "%.2f %%");
  gtk_widget_set_tooltip_text(g->midtones_Y, _("luminance exponent in midtones"));

  g->midtones_H = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "midtones_H"));
  dt_bauhaus_slider_set_feedback(g->midtones_H, 0);
  dt_bauhaus_slider_set_step(g->midtones_H, 10.);
  dt_bauhaus_slider_set_digits(g->midtones_H, 4);
  dt_bauhaus_slider_set_format(g->midtones_H, "%.2f °");
  gtk_widget_set_tooltip_text(g->midtones_H, _("hue of the color exponent in midtones"));

  g->midtones_C = dt_bauhaus_slider_from_params(self, "midtones_C");
  dt_bauhaus_slider_set_soft_range(g->midtones_C, 0., 0.02);
  dt_bauhaus_slider_set_step(g->midtones_C, 0.005);
  dt_bauhaus_slider_set_digits(g->midtones_C, 4);
  dt_bauhaus_slider_set_factor(g->midtones_C, 100.0f);
  dt_bauhaus_slider_set_format(g->midtones_C, "%.2f %%");
  gtk_widget_set_tooltip_text(g->midtones_C, _("chroma of the color exponent in midtones"));

  // Page masks
  self->widget = dt_ui_notebook_page(g->notebook, _("masks"), _("isolate luminances"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("luminance ranges")), FALSE, FALSE, 0);

  const float aspect = dt_conf_get_int("plugins/darkroom/colorbalancergb/aspect_percent") / 100.0;
  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(aspect));
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), FALSE, FALSE, 0);
  gtk_widget_add_events(GTK_WIDGET(g->area), darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(g->area), "scroll-event", G_CALLBACK(area_scroll_callback), self);

  g->shadows_weight = dt_bauhaus_slider_from_params(self, "shadows_weight");
  dt_bauhaus_slider_set_digits(g->shadows_weight, 4);
  dt_bauhaus_slider_set_step(g->shadows_weight, 0.1);
  dt_bauhaus_slider_set_format(g->shadows_weight, "%.2f %%");
  dt_bauhaus_slider_set_factor(g->shadows_weight, 100.0f);
  gtk_widget_set_tooltip_text(g->shadows_weight, _("weight of the shadows over the whole tonal range"));
  dt_bauhaus_widget_set_quad_paint(g->shadows_weight, dtgtk_cairo_paint_showmask,
                                   CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->shadows_weight, TRUE);
  g_signal_connect(G_OBJECT(g->shadows_weight), "quad-pressed", G_CALLBACK(mask_callback), self);

  g->grey_fulcrum = dt_bauhaus_slider_from_params(self, "grey_fulcrum");
  dt_bauhaus_slider_set_digits(g->grey_fulcrum, 4);
  dt_bauhaus_slider_set_step(g->grey_fulcrum, 0.01);
  dt_bauhaus_slider_set_format(g->grey_fulcrum, "%.2f %%");
  dt_bauhaus_slider_set_factor(g->grey_fulcrum, 100.0f);
  gtk_widget_set_tooltip_text(g->grey_fulcrum, _("position of the middle-grey reference for masking"));
  dt_bauhaus_widget_set_quad_paint(g->grey_fulcrum, dtgtk_cairo_paint_showmask,
                                   CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->grey_fulcrum, TRUE);
  g_signal_connect(G_OBJECT(g->grey_fulcrum), "quad-pressed", G_CALLBACK(mask_callback), self);

  g->highlights_weight = dt_bauhaus_slider_from_params(self, "highlights_weight");
  dt_bauhaus_slider_set_step(g->highlights_weight, 0.1);
  dt_bauhaus_slider_set_digits(g->highlights_weight, 4);
  dt_bauhaus_slider_set_format(g->highlights_weight, "%.2f %%");
  dt_bauhaus_slider_set_factor(g->highlights_weight, 100.0f);
  gtk_widget_set_tooltip_text(g->highlights_weight, _("weights of highlights over the whole tonal range"));
  dt_bauhaus_widget_set_quad_paint(g->highlights_weight, dtgtk_cairo_paint_showmask,
                                   CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->highlights_weight, TRUE);
  g_signal_connect(G_OBJECT(g->highlights_weight), "quad-pressed", G_CALLBACK(mask_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("threshold")), FALSE, FALSE, 0);

  g->white_fulcrum = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "white_fulcrum"));
  dt_bauhaus_slider_set_soft_range(g->white_fulcrum, -2., +2.);
  dt_bauhaus_slider_set_step(g->white_fulcrum, 0.1);
  dt_bauhaus_slider_set_digits(g->white_fulcrum, 4);
  dt_bauhaus_slider_set_format(g->white_fulcrum, "%.2f EV");
  gtk_widget_set_tooltip_text(g->white_fulcrum, _("peak white luminance value used to normalize the power function"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("mask preview settings")), FALSE, FALSE, 0);

  GtkWidget *row1 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(GTK_BOX(row1), dt_ui_label_new(_("checkerboard color 1")), TRUE, TRUE, 0);
  g->checker_color_1_picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->checker_color_1_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->checker_color_1_picker), _("select color of the checkerboard from a swatch"));
  gtk_box_pack_start(GTK_BOX(row1), GTK_WIDGET(g->checker_color_1_picker), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->checker_color_1_picker), "color-set", G_CALLBACK(checker_1_picker_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(row1), FALSE, FALSE, 0);

  GtkWidget *row2 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(GTK_BOX(row2), dt_ui_label_new(_("checkerboard color 2")), TRUE, TRUE, 0);
  g->checker_color_2_picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->checker_color_2_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->checker_color_2_picker), _("select color of the checkerboard from a swatch"));
  gtk_box_pack_start(GTK_BOX(row2), GTK_WIDGET(g->checker_color_2_picker), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->checker_color_2_picker), "color-set", G_CALLBACK(checker_2_picker_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(row2), FALSE, FALSE, 0);

  g->checker_size = dt_bauhaus_slider_new_with_range(self, 2., 32., 1., 8., 0);
  dt_bauhaus_slider_set_format(g->checker_size, "%.0f px");
  dt_bauhaus_widget_set_label(g->checker_size,  NULL, _("checkerboard size"));
  g_signal_connect(G_OBJECT(g->checker_size), "value-changed", G_CALLBACK(checker_size_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->checker_size), FALSE, FALSE, 0);

  // Init the conf keys if they don't exist
  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker1/red"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/red", 1.0f);
  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker1/green"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/green", 1.0f);
  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker1/blue"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker1/blue", 1.0f);

  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker2/red"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/red", 0.18f);
  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker2/green"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/green", 0.18f);
  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker2/blue"))
    dt_conf_set_float("plugins/darkroom/colorbalancergb/checker2/blue", 0.18f);

  if(!dt_conf_key_exists("plugins/darkroom/colorbalancergb/checker/size"))
    dt_conf_set_int("plugins/darkroom/colorbalancergb/checker/size", 8);

  // paint backgrounds
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float h = DEG_TO_RAD(stop * (360.f));
    float RGB[4] = { 0.f };
    float Ych[4] = { 0.75f, 0.2f, h, 0.f };
    float LMS[4] = { 0.f };
    Ych_to_gradingRGB(Ych, LMS, NULL);
    gradingRGB_to_XYZ(LMS, Ych);
    dt_XYZ_to_Rec709_D65(Ych, RGB);
    const float max_RGB = fmaxf(fmaxf(RGB[0], RGB[1]), RGB[2]);
    for(size_t c = 0; c < 3; c++) RGB[c] = powf(RGB[c] / max_RGB, 1.f / 2.2f);
    dt_bauhaus_slider_set_stop(g->global_H, stop, RGB[0], RGB[1], RGB[2]);
    dt_bauhaus_slider_set_stop(g->shadows_H, stop, RGB[0], RGB[1], RGB[2]);
    dt_bauhaus_slider_set_stop(g->highlights_H, stop, RGB[0], RGB[1], RGB[2]);
    dt_bauhaus_slider_set_stop(g->midtones_H, stop, RGB[0], RGB[1], RGB[2]);

    const float Y = 0.f + stop;
    dt_bauhaus_slider_set_stop(g->global_Y, stop, Y, Y, Y);
    dt_bauhaus_slider_set_stop(g->shadows_Y, stop, Y, Y, Y);
    dt_bauhaus_slider_set_stop(g->highlights_Y, stop, Y, Y, Y);
    dt_bauhaus_slider_set_stop(g->midtones_Y, stop, Y, Y, Y);
  }

  // main widget is the notebook
  self->widget = GTK_WIDGET(g->notebook);
}


void gui_cleanup(struct dt_iop_module_t *self)
{
  IOP_GUI_FREE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
