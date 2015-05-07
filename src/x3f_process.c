#include "x3f_process.h"
#include "x3f_meta.h"
#include "x3f_image.h"
#include "x3f_matrix.h"
#include "x3f_denoise.h"
#include "x3f_spatial_gain.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <assert.h>

static int sum_area(x3f_t *x3f, char *name,
		    x3f_area16_t *image, int rescale, int colors,
		    uint64_t *sum /* in/out */)
{
  x3f_area16_t area;
  int row, col, color;

  if (image->channels < colors) return 0;
  if (!x3f_crop_area_camf(x3f, name, image, rescale, &area)) return 0;

  for (row = 0; row < area.rows; row++)
    for (col = 0; col < area.columns; col++)
      for (color = 0; color < colors; color++)
	sum[color] += (uint64_t)area.data[area.row_stride*row +
					  area.channels*col + color];

  return area.columns*area.rows;
}

static int sum_area_sqdev(x3f_t *x3f, char *name,
			  x3f_area16_t *image, int rescale, int colors,
			  double *mean, double *sum /* in/out */)
{
  x3f_area16_t area;
  int row, col, color;

  if (image->channels < colors) return 0;
  if (!x3f_crop_area_camf(x3f, name, image, rescale, &area)) return 0;

  for (row = 0; row < area.rows; row++)
    for (col = 0; col < area.columns; col++)
      for (color = 0; color < colors; color++) {
	double dev = area.data[area.row_stride*row +
			       area.channels*col + color] - mean[color];
	sum[color] += dev*dev;
      }

  return area.columns*area.rows;
}

static int get_black_level(x3f_t *x3f,
			   x3f_area16_t *image, int rescale, int colors,
			   double *black_level, double *black_dev)
{
  uint64_t *black_sum;
  double *black_sum_sqdev;
  int pixels, i;

  pixels = 0;
  black_sum = alloca(colors*sizeof(uint64_t));
  memset(black_sum, 0, colors*sizeof(uint64_t));
  pixels += sum_area(x3f, "DarkShieldTop", image, rescale, colors,
		     black_sum);
  pixels += sum_area(x3f, "DarkShieldBottom", image, rescale, colors,
		     black_sum);
  if (pixels == 0) return 0;

  for (i=0; i<colors; i++)
    black_level[i] = (double)black_sum[i]/pixels;

  pixels = 0;
  black_sum_sqdev = alloca(colors*sizeof(double));
  memset(black_sum_sqdev, 0, colors*sizeof(double));
  pixels += sum_area_sqdev(x3f, "DarkShieldTop", image, rescale, colors,
			   black_level, black_sum_sqdev);
  pixels += sum_area_sqdev(x3f, "DarkShieldBottom", image, rescale, colors,
			   black_level, black_sum_sqdev);
  if (pixels == 0) return 0;

  for (i=0; i<colors; i++)
    black_dev[i] = sqrt(black_sum_sqdev[i]/pixels);

  return 1;
}

static void get_raw_neutral(double *raw_to_xyz, double *raw_neutral)
{
  double d65_xyz[3] = {0.95047, 1.00000, 1.08883};
  double xyz_to_raw[9];

  x3f_3x3_inverse(raw_to_xyz, xyz_to_raw);
  x3f_3x3_3x1_mul(xyz_to_raw, d65_xyz, raw_neutral);
}

/* extern */ int x3f_get_gain(x3f_t *x3f, char *wb, double *gain)
{
  double cam_to_xyz[9], wb_correction[9], gain_fact[3];

  if (x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceGains", wb, 3, 0, gain) ||
      x3f_get_camf_matrix_for_wb(x3f, "DP1_WhiteBalanceGains", wb, 3, 0, gain));
  else if (x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceIlluminants", wb,
				      3, 3, cam_to_xyz) &&
	   x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceCorrections", wb,
				      3, 3, wb_correction)) {
    double raw_to_xyz[9], raw_neutral[3];

    x3f_3x3_3x3_mul(wb_correction, cam_to_xyz, raw_to_xyz);
    get_raw_neutral(raw_to_xyz, raw_neutral);
    x3f_3x1_invert(raw_neutral, gain);
  }
  else
    return 0;

  if (x3f_get_camf_float_vector(x3f, "SensorAdjustmentGainFact", gain_fact))
    x3f_3x1_comp_mul(gain_fact, gain, gain);

  if (x3f_get_camf_float_vector(x3f, "TempGainFact", gain_fact))
    x3f_3x1_comp_mul(gain_fact, gain, gain);

  if (x3f_get_camf_float_vector(x3f, "FNumberGainFact", gain_fact))
    x3f_3x1_comp_mul(gain_fact, gain, gain);

  printf("gain\n");
  x3f_3x1_print(gain);

  return 1;
}

/* extern */ int x3f_get_bmt_to_xyz(x3f_t *x3f, char *wb, double *bmt_to_xyz)
{
  double cc_matrix[9], cam_to_xyz[9], wb_correction[9];

  if (x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceColorCorrections", wb,
				 3, 3, cc_matrix) ||
      x3f_get_camf_matrix_for_wb(x3f, "DP1_WhiteBalanceColorCorrections", wb,
				 3, 3, cc_matrix)) {
    double srgb_to_xyz[9];

    x3f_sRGB_to_XYZ(srgb_to_xyz);
    x3f_3x3_3x3_mul(srgb_to_xyz, cc_matrix, bmt_to_xyz);
  }
  else if (x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceIlluminants", wb,
				      3, 3, cam_to_xyz) &&
	   x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceCorrections", wb,
				      3, 3, wb_correction)) {
    double raw_to_xyz[9], raw_neutral[3], raw_neutral_mat[9];

    x3f_3x3_3x3_mul(wb_correction, cam_to_xyz, raw_to_xyz);
    get_raw_neutral(raw_to_xyz, raw_neutral);
    x3f_3x3_diag(raw_neutral, raw_neutral_mat);
    x3f_3x3_3x3_mul(raw_to_xyz, raw_neutral_mat, bmt_to_xyz);
  }
  else
    return 0;

  printf("bmt_to_xyz\n");
  x3f_3x3_print(bmt_to_xyz);

  return 1;
}

/* extern */ int x3f_get_raw_to_xyz(x3f_t *x3f, char *wb, double *raw_to_xyz)
{
  double bmt_to_xyz[9], gain[9], gain_mat[9];

  if (!x3f_get_gain(x3f, wb, gain)) return 0;
  if (!x3f_get_bmt_to_xyz(x3f, wb, bmt_to_xyz)) return 0;

  x3f_3x3_diag(gain, gain_mat);
  x3f_3x3_3x3_mul(bmt_to_xyz, gain_mat, raw_to_xyz);

  printf("raw_to_xyz\n");
  x3f_3x3_print(raw_to_xyz);

  return 1;
}

/* x3f_denoise expects a 14-bit image since rescaling by a factor of 4
   takes place internally. */
#define INTERMEDIATE_DEPTH 14
#define INTERMEDIATE_UNIT ((1<<INTERMEDIATE_DEPTH) - 1)
#define INTERMEDIATE_BIAS_FACTOR 4.0

static int get_max_intermediate(x3f_t *x3f, char *wb,
				double intermediate_bias,
				uint32_t *max_intermediate)
{
  double gain[3], maxgain = 0.0;
  int i;

  if (!x3f_get_gain(x3f, wb, gain)) return 0;

  /* Cap the gains to 1.0 to avoid clipping */
  for (i=0; i<3; i++)
    if (gain[i] > maxgain) maxgain = gain[i];
  for (i=0; i<3; i++)
    max_intermediate[i] =
      (int32_t)round(gain[i]*(INTERMEDIATE_UNIT - intermediate_bias)/maxgain +
		     intermediate_bias);

  return 1;
}

static int get_intermediate_bias(x3f_t *x3f, char *wb,
				 double *black_level, double *black_dev,
				 double *intermediate_bias)
{
  uint32_t max_raw[3], max_intermediate[3];
  int i;

  if (!x3f_get_max_raw(x3f, max_raw)) return 0;
  if (!get_max_intermediate(x3f, wb, 0, max_intermediate)) return 0;

  *intermediate_bias = 0.0;
  for (i=0; i<3; i++) {
    double bias = INTERMEDIATE_BIAS_FACTOR * black_dev[i] *
      max_intermediate[i] / (max_raw[i] - black_level[i]);
    if (bias > *intermediate_bias) *intermediate_bias = bias;
  }

  return 1;
}

typedef struct bad_pixel_s {
  int c, r;
  struct bad_pixel_s *prev, *next;
} bad_pixel_t;

/* Address pixel at column _c and row _r */
#define _PN(_c, _r, _cs) ((_r)*(_cs) + (_c))

/* Test if a pixel (_c,_r) is within a rectancle */
#define _INB(_c, _r, _cs, _rs)					\
  ((_c) >= 0 && (_c) < (_cs) && (_r) >= 0 && (_r) < (_rs))

/* Test if a pixel has been marked in the bad pixel vector */
#define TEST_PIX(_vec, _c, _r, _cs, _rs)				\
  (_INB((_c), (_r), (_cs), (_rs)) ?					\
   (_vec)[_PN((_c), (_r), (_cs)) >> 5] &				\
   1 << (_PN((_c), (_r), (_cs)) & 0x1f) : 1)

/* Mark the pixel, in the bad pixel vector and the bad pixel list */
#define MARK_PIX(_list, _vec, _c, _r, _cs, _rs)				\
  do {									\
    if (!TEST_PIX((_vec), (_c), (_r), (_cs), (_rs))) {			\
      bad_pixel_t *_p = malloc(sizeof(bad_pixel_t));			\
      _p->c = (_c);							\
      _p->r = (_r);							\
      _p->prev = NULL;							\
      _p->next = (_list);						\
      if (_list) (_list)->prev = (_p);					\
      (_list) = _p;							\
      (_vec)[_PN((_c), (_r), (_cs)) >> 5] |=				\
	1 << (_PN((_c), (_r), (_cs)) & 0x1f);				\
    }									\
    else if (!_INB((_c), (_r), (_cs), (_rs)))				\
      fprintf(stderr,							\
	      "WARNING: bad pixel (%u,%u) out of bounds : (%u,%u)\n",	\
	      (_c), (_r), (_cs), (_rs));				\
  } while (0)

/* Clear the mark in the bad pixel vector */
#define CLEAR_PIX(_vec, _c, _r, _cs, _rs)				\
  do {									\
    assert(_INB((_c), (_r), (_cs), (_rs)));				\
    _vec[_PN((_c), (_r), (_cs)) >> 5] &=				\
      ~(1 << (_PN((_c), (_r), (_cs)) & 0x1f));				\
  } while (0)

static void interpolate_bad_pixels(x3f_t *x3f, x3f_area16_t *image, int colors)
{
  bad_pixel_t *bad_pixel_list = NULL;
  uint32_t *bad_pixel_vec = calloc((image->rows*image->columns + 31)/32,
				   sizeof(uint32_t));
  int row, col, color, i;
  uint32_t *bpf23;
  int bpf23_len;
  int stat_pass = 0;		/* Statistics */
  int fix_corner = 0;		/* By default, do not accept corners */

  /* BEGIN - collecting bad pixels. This part reads meta data and
     collects all bad pixels both in the list 'bad_pixel_list' and the
     vector 'bad_pixel_vec' */

  if (colors == 3) {
    uint32_t keep[4], hpinfo[4], *bp, *bpf20;
    int bp_num, bpf20_rows, bpf20_cols;

    if (x3f_get_camf_matrix(x3f, "KeepImageArea", 4, 0, 0, M_UINT, keep) &&
	x3f_get_camf_matrix_var(x3f, "BadPixels", &bp_num, NULL, NULL,
				M_UINT, (void **)&bp))
      for (i=0; i < bp_num; i++)
	MARK_PIX(bad_pixel_list, bad_pixel_vec,
		 ((bp[i] & 0x000fff00) >> 8) - keep[0],
		 ((bp[i] & 0xfff00000) >> 20) - keep[1],
		 image->columns, image->rows);

    /* NOTE: the numbers of rows and cols in this matrix are
       interchanged due to bug in camera firmware */
    if (x3f_get_camf_matrix_var(x3f, "BadPixelsF20",
				&bpf20_cols, &bpf20_rows, NULL,
				M_UINT, (void **)&bpf20) && bpf20_cols == 3)
      for (row=0; row < bpf20_rows; row++)
	MARK_PIX(bad_pixel_list, bad_pixel_vec,
		 bpf20[3*row + 1], bpf20[3*row + 0],
		 image->columns, image->rows);

    /* NOTE: the numbers of rows and cols in this matrix are
       interchanged due to bug in camera firmware
       TODO: should Jpeg_BadClutersF20 really be used for RAW? It works
       though. */
    if (x3f_get_camf_matrix_var(x3f, "Jpeg_BadClusters",
				&bpf20_cols, &bpf20_rows, NULL,
				M_UINT, (void **)&bpf20) && bpf20_cols == 3)
      for (row=0; row < bpf20_rows; row++)
	MARK_PIX(bad_pixel_list, bad_pixel_vec,
		 bpf20[3*row + 1], bpf20[3*row + 0],
		 image->columns, image->rows);

    /* TODO: should those really be interpolated over, or should they be
       rescaled instead? */
    if (x3f_get_camf_matrix(x3f, "HighlightPixelsInfo", 2, 2, 0, M_UINT,
			    hpinfo))
      for (row = hpinfo[1]; row < image->rows; row += hpinfo[3])
	for (col = hpinfo[0]; col < image->columns; col += hpinfo[2])
	  MARK_PIX(bad_pixel_list, bad_pixel_vec,
		   col, row, image->columns, image->rows);
  } /* colors == 3 */

  if ((colors == 1 && x3f_get_camf_matrix_var(x3f, "BadPixelsLumaF23",
					      &bpf23_len, NULL, NULL,
					      M_UINT, (void **)&bpf23)) ||
      (colors == 3 && x3f_get_camf_matrix_var(x3f, "BadPixelsChromaF23",
					      &bpf23_len, NULL, NULL,
					      M_UINT, (void **)&bpf23)))
    for (i=0, row=-1; i < bpf23_len; i++)
      if (row == -1) row = bpf23[i];
      else if (bpf23[i] == 0) row = -1;
      else {MARK_PIX(bad_pixel_list, bad_pixel_vec,
		     bpf23[i], row,
		     image->columns, image->rows); i++;}

  /* END - collecting bad pixels */


  /* BEGIN - fixing bad pixels. This part fixes all bad pixels
     collected in the list 'bad_pixel_list', using the mirror data in
     the vector 'bad_pixel_vec'.  This is made in passes. In each pass
     all pixels that can be interpolated are interpolated and also
     removed from the list of bad pixels.  Eventually the list of bad
     pixels is going to be empty. */

  while (bad_pixel_list) {
    bad_pixel_t *p, *pn;
    bad_pixel_t *fixed = NULL;	/* Contains all, in this pass, fixed pixels */
    struct {
      int all_four, two_linear, two_corner, left; /* Statistics */
    } stats = {0,0,0,0};

    /* Iterate over all pixels in the bad pixel list, in this pass */
    for (p=bad_pixel_list; p && (pn=p->next, 1); p=pn) {
      uint16_t *outp =
	&image->data[p->r*image->row_stride + p->c*image->channels];
      uint16_t *inp[4] = {NULL, NULL, NULL, NULL};
      int num = 0;

      /* Collect status of neighbor pixels */
      if (!TEST_PIX(bad_pixel_vec, p->c - 1, p->r, image->columns, image->rows))
	num++, inp[0] =
	  &image->data[p->r*image->row_stride + (p->c - 1)*image->channels];
      if (!TEST_PIX(bad_pixel_vec, p->c + 1, p->r, image->columns, image->rows))
	num++, inp[1] =
	  &image->data[p->r*image->row_stride + (p->c + 1)*image->channels];
      if (!TEST_PIX(bad_pixel_vec, p->c, p->r - 1, image->columns, image->rows))
	num++, inp[2] =
	  &image->data[(p->r - 1)*image->row_stride + p->c*image->channels];
      if (!TEST_PIX(bad_pixel_vec, p->c, p->r + 1, image->columns, image->rows))
	num++, inp[3] =
	  &image->data[(p->r + 1)*image->row_stride + p->c*image->channels];

      /* Test if interpolation is possible ... */
      if (inp[0] && inp[1] && inp[2] && inp[3])
	/* ... all four neighbors are OK */
	stats.all_four++;
      else if (inp[0] && inp[1])
	/* ... left and right are OK */
	inp[2] = inp[3] = NULL, num = 2, stats.two_linear++;
      else if (inp[2] && inp[3])
	/* ... above and under are OK */
	inp[0] = inp[1] = NULL, num = 2, stats.two_linear++;
      else if (fix_corner && num == 2)
	/* ... corner (plus nothing else to do) are OK */
	stats.two_corner++;
      else
	/* ... nope - it was not possible. Look at next without doing
	   interpolation.  */
	{stats.left++; continue;};

      /* Interpolate the actual pixel */
      for (color=0; color < colors; color++) {
	uint32_t sum = 0;
	for (i=0; i<4; i++)
	  if (inp[i]) sum += inp[i][color];
	outp[color] = (sum + num/2)/num;
      }

      /* Remove p from bad_pixel_list */
      if (p->prev) p->prev->next  = p->next;
      else         bad_pixel_list = p->next;
      if (p->next) p->next->prev = p->prev;

      /* Add p to fixed list */
      p->prev = NULL;
      p->next = fixed;
      fixed = p;
    }

    printf("Bad pixels pass %d: %d fixed (%d all_four, %d linear, %d corner), %d left\n",
	   stat_pass,
	   stats.all_four + stats.two_linear + stats.two_corner,
	   stats.all_four,
	   stats.two_linear,
	   stats.two_corner,
	   stats.left);

    if (!fixed) {
      /* If nothing else to do, accept corners */
      if (!fix_corner) fix_corner = 1;
      else {
	fprintf(stderr,	"WARNING: Failed to interpolate %d bad pixels\n",
		stats.left);
	fixed = bad_pixel_list;	/* Free remaining list entries */
	bad_pixel_list = NULL;	/* Force termination */
      }
    }

    /* Clear the bad pixel vector and free the list */
    for (p=fixed; p && (pn=p->next, 1); p=pn) {
      CLEAR_PIX(bad_pixel_vec, p->c, p->r, image->columns, image->rows);
      free(p);
    }

    stat_pass++;
  }

  /* END - fixing bad pixels */

  free(bad_pixel_vec);
}

static int preprocess_data(x3f_t *x3f, char *wb, x3f_image_levels_t *ilevels)
{
  x3f_area16_t image, qtop;
  int row, col, color;
  uint32_t max_raw[3];
  double scale[3], black_level[3], black_dev[3], intermediate_bias;
  int quattro = x3f_image_area_qtop(x3f, &qtop);
  int colors_in = quattro ? 2 : 3;

  if (!x3f_image_area(x3f, &image) || image.channels < 3) return 0;
  if (quattro && (qtop.channels < 1 ||
		  qtop.rows < 2*image.rows || qtop.columns < 2*image.columns))
    return 0;

  if (!get_black_level(x3f, &image, 1, colors_in, black_level, black_dev) ||
      (quattro && !get_black_level(x3f, &qtop, 0, 1,
				   &black_level[2], &black_dev[2]))) {
    fprintf(stderr, "Could not get black level\n");
    return 0;
  }
  printf("black_level = {%g,%g,%g}, black_dev = {%g,%g,%g}\n",
	 black_level[0], black_level[1], black_level[2],
	 black_dev[0], black_dev[1], black_dev[2]);

  if (!x3f_get_max_raw(x3f, max_raw)) {
    fprintf(stderr, "Could not get maximum RAW level\n");
    return 0;
  }
  printf("max_raw = {%u,%u,%u}\n", max_raw[0], max_raw[1], max_raw[2]);

  if (!get_intermediate_bias(x3f, wb, black_level, black_dev,
			     &intermediate_bias)) {
    fprintf(stderr, "Could not get intermediate bias\n");
    return 0;
  }
  printf("intermediate_bias = %g\n", intermediate_bias);
  ilevels->black[0] = ilevels->black[1] = ilevels->black[2] = intermediate_bias;

  if (!get_max_intermediate(x3f, wb, intermediate_bias, ilevels->white)) {
    fprintf(stderr, "Could not get maximum intermediate level\n");
    return 0;
  }
  printf("max_intermediate = {%u,%u,%u}\n",
	 ilevels->white[0], ilevels->white[1], ilevels->white[2]);

  for (color = 0; color < 3; color++)
    scale[color] = (ilevels->white[color] - ilevels->black[color]) /
      (max_raw[color] - black_level[color]);

  /* Preprocess image data (HUF/TRU->x3rgb16) */
  for (row = 0; row < image.rows; row++)
    for (col = 0; col < image.columns; col++)
      for (color = 0; color < colors_in; color++) {
	uint16_t *valp =
	  &image.data[image.row_stride*row + image.channels*col + color];
	int32_t out =
	  (int32_t)round(scale[color] * (*valp - black_level[color]) +
			 ilevels->black[color]);

	if (out < 0) *valp = 0;
	else if (out > 65535) *valp = 65535;
	else *valp = out;
      }

  if (quattro) {
    /* Preprocess and downsample Quattro top layer (Q->top16) */
    for (row = 0; row < image.rows; row++)
      for (col = 0; col < image.columns; col++) {
	uint16_t *outp =
	  &image.data[image.row_stride*row + image.channels*col + 2];
	uint16_t *row1 =
	  &qtop.data[qtop.row_stride*2*row + qtop.channels*2*col];
	uint16_t *row2 =
	  &qtop.data[qtop.row_stride*(2*row+1) + qtop.channels*2*col];
	uint32_t sum =
	  row1[0] + row1[qtop.channels] + row2[0] + row2[qtop.channels];
	int32_t out = (int32_t)round(scale[2] * (sum/4.0 - black_level[2]) +
				     ilevels->black[2]);

	if (out < 0) *outp = 0;
	else if (out > 65535) *outp = 65535;
	else *outp = out;
      }

    /* Preprocess Quattro top layer (Q->top16) at full resolution */
    for (row = 0; row < qtop.rows; row++)
      for (col = 0; col < qtop.columns; col++) {
	uint16_t *valp = &qtop.data[qtop.row_stride*row + qtop.channels*col];
	int32_t out = (int32_t)round(scale[2] * (*valp - black_level[2]) +
				     ilevels->black[2]);

	if (out < 0) *valp = 0;
	else if (out > 65535) *valp = 65535;
	else *valp = out;
      }
    interpolate_bad_pixels(x3f, &qtop, 1);
  }

  interpolate_bad_pixels(x3f, &image, 3);

  return 1;
}

/* Converts the data in place */

#define LUTSIZE 1024

static int convert_data(x3f_t *x3f,
			x3f_area16_t *image, x3f_image_levels_t *ilevels,
			x3f_color_encoding_t encoding, char *wb)
{
  int row, col, color;
  uint16_t max_out = 65535; /* TODO: should be possible to adjust */

  double raw_to_xyz[9];	/* White point for XYZ is assumed to be D65 */
  double xyz_to_rgb[9];
  double raw_to_rgb[9];
  double conv_matrix[9];
  double sensor_iso, capture_iso, iso_scaling;
  double lut[LUTSIZE];
  x3f_spatial_gain_corr_t sgain[MAXCORR];
  int sgain_num;

  if (image->channels < 3) return X3F_ARGUMENT_ERROR;

  if (x3f_get_camf_float(x3f, "SensorISO", &sensor_iso) &&
      x3f_get_camf_float(x3f, "CaptureISO", &capture_iso)) {
    printf("SensorISO = %g\n", sensor_iso);
    printf("CaptureISO = %g\n", capture_iso);
    iso_scaling = capture_iso/sensor_iso;
  }
  else {
    iso_scaling = 1.0;
    fprintf(stderr, "WARNING: could not calculate ISO scaling, assuming %g\n",
	    iso_scaling);
  }

  if (!x3f_get_raw_to_xyz(x3f, wb, raw_to_xyz)) {
    fprintf(stderr, "Could not get raw_to_xyz for white balance: %s\n", wb);
    return 0;
  }

  switch (encoding) {
  case SRGB:
    x3f_sRGB_LUT(lut, LUTSIZE, max_out);
    x3f_XYZ_to_sRGB(xyz_to_rgb);
    break;
  case ARGB:
    x3f_gamma_LUT(lut, LUTSIZE, max_out, 2.2);
    x3f_XYZ_to_AdobeRGB(xyz_to_rgb);
    break;
  case PPRGB:
    {
      double xyz_to_prophotorgb[9], d65_to_d50[9];

      x3f_gamma_LUT(lut, LUTSIZE, max_out, 1.8);
      x3f_XYZ_to_ProPhotoRGB(xyz_to_prophotorgb);
      /* The standad white point for ProPhoto RGB is D50 */
      x3f_Bradford_D65_to_D50(d65_to_d50);
      x3f_3x3_3x3_mul(xyz_to_prophotorgb, d65_to_d50, xyz_to_rgb);
    }
    break;
  default:
    fprintf(stderr, "Unknown color space %d\n", encoding);
    return X3F_ARGUMENT_ERROR;
  }

  x3f_3x3_3x3_mul(xyz_to_rgb, raw_to_xyz, raw_to_rgb);
  x3f_scalar_3x3_mul(iso_scaling, raw_to_rgb, conv_matrix);

  printf("raw_to_rgb\n");
  x3f_3x3_print(raw_to_rgb);
  printf("conv_matrix\n");
  x3f_3x3_print(conv_matrix);

  sgain_num = x3f_get_spatial_gain(x3f, wb, sgain);
  if (sgain_num == 0)
    fprintf(stderr, "WARNING: could not get spatial gain\n");

  for (row = 0; row < image->rows; row++) {
    for (col = 0; col < image->columns; col++) {
      uint16_t *valp[3];
      double input[3], output[3];

      /* Get the data */
      for (color = 0; color < 3; color++) {
	valp[color] =
	  &image->data[image->row_stride*row + image->channels*col + color];
	input[color] = x3f_calc_spatial_gain(sgain, sgain_num,
					     row, col, color,
					     image->rows, image->columns) *
	  (*valp[color] - ilevels->black[color]) /
	  (ilevels->white[color] - ilevels->black[color]);
      }

      /* Do color conversion */
      x3f_3x3_3x1_mul(conv_matrix, input, output);

      /* Write back the data, doing non linear coding */
      for (color = 0; color < 3; color++)
	*valp[color] = x3f_LUT_lookup(lut, LUTSIZE, output[color]);
    }
  }

  x3f_cleanup_spatial_gain(sgain, sgain_num);

  ilevels->black[0] = ilevels->black[1] = ilevels->black[2] = 0.0;
  ilevels->white[0] = ilevels->white[1] = ilevels->white[2] = max_out;

  return 1;
}

static int run_denoising(x3f_t *x3f)
{
  x3f_area16_t original_image, image;
  x3f_denoise_type_t type = X3F_DENOISE_STD;
  char *sensorid;

  if (!x3f_image_area(x3f, &original_image)) return 0;
  if (!x3f_crop_area_camf(x3f, "ActiveImageArea", &original_image, 1, &image)) {
    image = original_image;
    fprintf(stderr,
	    "WARNING: could not get active area, denoising entire image\n");
  }

  if (x3f_get_prop_entry(x3f, "SENSORID", &sensorid) &&
      !strcmp(sensorid, "F20"))
    type = X3F_DENOISE_F20;

  x3f_denoise(&image, type);
  return 1;
}

static int expand_quattro(x3f_t *x3f, int denoise, x3f_area16_t *expanded)
{
  x3f_area16_t image, active, qtop, qtop_crop, active_exp;
  uint32_t rect[4];

  if (!x3f_image_area_qtop(x3f, &qtop)) return 0;
  if (!x3f_image_area(x3f, &image)) return 0;
  if (denoise &&
      !x3f_crop_area_camf(x3f, "ActiveImageArea", &image, 1, &active)) {
    active = image;
    fprintf(stderr,
	    "WARNING: could not get active area, denoising entire image\n");
  }

  rect[0] = 0;
  rect[1] = 0;
  rect[2] = 2*image.columns - 1;
  rect[3] = 2*image.rows - 1;
  if (!x3f_crop_area(rect, &qtop, &qtop_crop)) return 0;

  expanded->columns = qtop_crop.columns;
  expanded->rows = qtop_crop.rows;
  expanded->channels = 3;
  expanded->row_stride = expanded->columns*expanded->channels;
  expanded->data = expanded->buf =
    malloc(expanded->rows*expanded->row_stride*sizeof(uint16_t));

  if (denoise && !x3f_crop_area_camf(x3f, "ActiveImageArea", expanded, 0,
				     &active_exp)) {
    active_exp = *expanded;
    fprintf(stderr,
	    "WARNING: could not get active area, denoising entire image\n");
  }

  x3f_expand_quattro(&image, denoise ? &active : NULL, &qtop_crop,
		     expanded, denoise ? &active_exp : NULL);

  return 1;
}

/* extern */ int x3f_get_image(x3f_t *x3f,
			       x3f_area16_t *image,
			       x3f_image_levels_t *ilevels,
			       x3f_color_encoding_t encoding,
			       int crop,
			       int denoise,
			       char *wb)
{
  x3f_area16_t original_image, expanded;
  x3f_image_levels_t il;

  if (wb == NULL) wb = x3f_get_wb(x3f);

  if (encoding == QTOP) {
    x3f_area16_t qtop;

    if (!x3f_image_area_qtop(x3f, &qtop)) return 0;
    if (!crop || !x3f_crop_area_camf(x3f, "ActiveImageArea", &qtop, 0, image))
      *image = qtop;

    return ilevels == NULL;
  }

  if (!x3f_image_area(x3f, &original_image)) return 0;
  if (!crop || !x3f_crop_area_camf(x3f, "ActiveImageArea", &original_image, 1,
				   image))
    *image = original_image;

  if (encoding == UNPROCESSED) return ilevels == NULL;

  if (!preprocess_data(x3f, wb, &il)) return 0;

  if (expand_quattro(x3f, denoise, &expanded)) {
    /* NOTE: expand_quattro destroys the data of original_image */
    if (!crop ||
	!x3f_crop_area_camf(x3f, "ActiveImageArea", &expanded, 0, image))
      *image = expanded;
    original_image = expanded;
  }
  else if (denoise && !run_denoising(x3f)) return 0;

  if (encoding != NONE &&
      !convert_data(x3f, &original_image, &il, encoding, wb)) {
    free(image->buf);
    return 0;
  }

  if (ilevels) *ilevels = il;
  return 1;
}
