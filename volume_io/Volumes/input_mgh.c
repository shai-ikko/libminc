/**
 * \file Reader for MGH/MGZ (FreeSurfer) format files.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /*HAVE_CONFIG_H*/

#include "input_mgh.h"

#include <arpa/inet.h> /* for ntohl and ntohs */
#include "znzlib.h"
#include "errno.h"

#define NUM_BYTE_VALUES      (UCHAR_MAX + 1)

#define MGH_MAX_DIMS 4          /* Maximum number of dimensions */
#define MGH_N_SPATIAL VIO_N_DIMENSIONS /* Number of spatial dimensions */
#define MGH_N_COMPONENTS 4      /* Number of transform components. */
#define MGH_N_XFORM (MGH_N_COMPONENTS * MGH_N_SPATIAL)

#define MGH_HEADER_SIZE 284 /* Total number of bytes in the header. */

#define MGH_EXTRA_SIZE 194   /* Number of "unused" bytes in the header. */

#define MGH_TYPE_UCHAR 0  /**< Voxels are 1-byte unsigned integers. */
#define MGH_TYPE_INT 1    /**< Voxels are 4-byte signed integers. */
#define MGH_TYPE_LONG 2   /**< Unsupported here.  */
#define MGH_TYPE_FLOAT 3  /**< Voxels are 4-byte floating point. */
#define MGH_TYPE_SHORT 4  /**< Voxels are 2-byte signed integers. */
#define MGH_TYPE_BITMAP 5 /**< Unsupported here. */
#define MGH_TYPE_TENSOR 6 /**< Unsupported here. */

/* MGH tag types, at least the ones that are minimally documented.
 */
#define TAG_OLD_COLORTABLE          1
#define TAG_OLD_USEREALRAS          2
#define TAG_CMDLINE                 3
#define TAG_USEREALRAS              4
#define TAG_COLORTABLE              5

#define TAG_GCAMORPH_GEOM           10
#define TAG_GCAMORPH_TYPE           11
#define TAG_GCAMORPH_LABELS         12
 
#define TAG_OLD_SURF_GEOM           20
#define TAG_SURF_GEOM               21
 
#define TAG_OLD_MGH_XFORM           30
#define TAG_MGH_XFORM               31
#define TAG_GROUP_AVG_SURFACE_AREA  32

/**
 * Information in the MGH/MGZ file header.
 */
struct mgh_header {
  int version;                  /**< Must be 0x00000001. */
  int sizes[MGH_MAX_DIMS];      /**< Dimension sizes, fastest-varying FIRST. */
  int type;                     /**< One of the MGH_TYPE_xxx values. */
  int dof;                      /**< Degrees of freedom, if used. */
  short goodRASflag;            /**< True if spacing and dircos valid. */
  float spacing[MGH_N_SPATIAL]; /**< Dimension spacing.  */
  float dircos[MGH_N_COMPONENTS][MGH_N_SPATIAL]; /**< Dimension transform. */
};

/**
 * Trailer information found immediately AFTER the hyperslab of data.
 */
struct mgh_trailer {
  float TR;
  float FlipAngle;
  float TE;
  float TI;
  float FoV;
};

/**
 * Trivial function to swap floats if necessary.
 * \param f The big-endian 4-byte float value.
 * \return The float value in host byte order.
 */
static float
swapFloat(float f)
{
  union {
    float f;
    int i;
  } sl;

  sl.f = f;

  sl.i = ntohl(sl.i);

  return sl.f;
}

/**
 * Reads the next slice from the MGH volume. As a side effect, it advances
 * the slice_index value in the volume input structure if successful.
 *
 * \param in_ptr The volume input information.
 * \return VIO_OK if success
 */
static VIO_Status
input_next_slice(
                 volume_input_struct *in_ptr
                 )
{
  size_t n_voxels_in_slice;
  size_t n_bytes_per_voxel;
  znzFile fp = (znzFile) in_ptr->volume_file;

  if (in_ptr->slice_index >= in_ptr->sizes_in_file[2])
  {
    fprintf(stderr, "Read past final slice.\n");
    return VIO_ERROR;
  }

  n_bytes_per_voxel = get_type_size(in_ptr->file_data_type);
  n_voxels_in_slice = (in_ptr->sizes_in_file[0] *
                       in_ptr->sizes_in_file[1]);
  if (znzread(in_ptr->byte_slice_buffer,
              n_bytes_per_voxel,
              n_voxels_in_slice,
              fp) != n_voxels_in_slice)
  {
    fprintf(stderr, "read error %d\n", errno);
    return VIO_ERROR;
  }

  ++in_ptr->slice_index;
  return VIO_OK;
}

/**
 * Converts a MGH file header into a general linear transform for the
 * Volume IO library. There are two different ways of defining the
 " "centre" of the volume in the MGH world. One uses the values in 
 * c_r, c_a, and c_s (the last row of the dircos field) to offset
 * the origin. The other, more common case ignores these fields and
 * just uses the voxel size and spacing to determine a value for
 * the centre.
 * Note that the geometric structures produced by MGH tools use
 * the latter case.
 *
 * \param hdr_ptr A pointer to the MGH header structure.
 * \param in_ptr A pointer to the input information for this volume.
 * \param ignore_offsets TRUE if we should use grid centres.
 * \param linear_xform_ptr A pointer to the output transform
 * \returns void
 */
static void
mgh_header_to_linear_transform(const struct mgh_header *hdr_ptr,
                               const volume_input_struct *in_ptr,
                               VIO_BOOL ignore_offsets,
                               struct VIO_General_transform *linear_xform_ptr)
{
  int           i, j;
  VIO_Transform mnc_xform;
  VIO_Real      mgh_xform[MGH_N_SPATIAL][MGH_N_COMPONENTS];
  
  make_identity_transform(&mnc_xform);

#if DEBUG
  /* Print out the raw MGH transform information.
   */
  for (i = 0; i < MGH_N_SPATIAL; i++)
  {
    for (j = 0; j < MGH_N_COMPONENTS; j++)
    {
      printf("%c_%c %8.4f ", "xyzc"[j], "ras"[i], hdr_ptr->dircos[j][i]);
    }
    printf("\n");
  }
#endif // DEBUG

  /* Multiply the direction cosines by the spacings.
   */
  for (i = 0; i < MGH_N_SPATIAL; i++)
  {
    for (j = 0; j < MGH_N_SPATIAL; j++)
    {
      mgh_xform[i][j] = hdr_ptr->dircos[j][i] * hdr_ptr->spacing[i];
    }
  }

  /* Work out the final MGH transform. This requires that we figure out
   * the origin values to fill in the final column of the transform.
   */
  for (i = 0; i < MGH_N_SPATIAL; i++)
  {
    double temp = 0.0;
    for (j = 0; j < MGH_N_SPATIAL; j++)
    {
      temp += hdr_ptr->dircos[j][i] * (hdr_ptr->sizes[j] / 2.0);
    }

    /* Set the origin for the voxel-to-world transform .
     */
    if (ignore_offsets)
    {
      mgh_xform[i][MGH_N_COMPONENTS - 1] = -temp;
    }
    else
    {
      mgh_xform[i][MGH_N_COMPONENTS - 1] = hdr_ptr->dircos[MGH_N_COMPONENTS - 1][i] - temp;
    }
  }

#if DEBUG
  printf("mgh_xform:\n");       /* DEBUG */
  for (i = 0; i < MGH_N_SPATIAL; i++)
  {
    for (j = 0; j < MGH_N_COMPONENTS; j++)
    {
      printf("%.4f ", mgh_xform[i][j]);
    }
    printf("\n");
  }
#endif // DEBUG

  /* Convert MGH transform to the MINC format. The only difference is
   * that our transform is always written in XYZ (RAS) order, so we
   * have to swap the _columns_ as needed.
   */
  for (i = 0; i < MGH_N_SPATIAL; i++)
  {
    for (j = 0; j < MGH_N_COMPONENTS; j++)
    {
      int volume_axis = j;
      if (j < VIO_N_DIMENSIONS)
      {
        volume_axis = in_ptr->axis_index_from_file[j];
      }
      Transform_elem(mnc_xform, i, volume_axis) = mgh_xform[i][j];
    }
  }
  create_linear_transform(linear_xform_ptr, &mnc_xform);
#if DEBUG
  output_transform(stdout, "debug", NULL, NULL, linear_xform_ptr);
#endif // DEBUG
}

/**
 * Read an MGH header from an open file stream.
 * \param fp The open file (may be a pipe).
 * \param hdr_ptr A pointer to the header structure to fill in.
 * \returns TRUE if successful.
 */
static VIO_BOOL
mgh_header_from_file(znzFile fp, struct mgh_header *hdr_ptr)
{
  int i, j;
  char dummy[MGH_HEADER_SIZE];

  /* Read in the header. We do this piecemeal because the mgh_header
   * struct will not be properly aligned on most systems, so a single
   * fread() WILL NOT WORK.
   */
  if (znzread(&hdr_ptr->version, sizeof(int), 1, fp) != 1 ||
      znzread(hdr_ptr->sizes, sizeof(int), MGH_MAX_DIMS, fp) != MGH_MAX_DIMS ||
      znzread(&hdr_ptr->type, sizeof(int), 1, fp) != 1 ||
      znzread(&hdr_ptr->dof, sizeof(int), 1, fp) != 1 || 
      znzread(&hdr_ptr->goodRASflag, sizeof(short), 1, fp) != 1 ||
      /* The rest of the fields are optional, but we can safely read them
       * now and check goodRASflag later to see if we should really trust
       * them.
       */
      znzread(hdr_ptr->spacing, sizeof(float), MGH_N_SPATIAL, fp) != MGH_N_SPATIAL ||
      znzread(hdr_ptr->dircos, sizeof(float), MGH_N_XFORM, fp) != MGH_N_XFORM ||
      znzread(dummy, 1, MGH_EXTRA_SIZE, fp) != MGH_EXTRA_SIZE)
  {
    print_error("Problem reading MGH file header.");
    return FALSE;
  }

  /* Successfully read all of the data. Now we have to convert it to the
   * machine's byte ordering.
   */
  hdr_ptr->version = ntohl(hdr_ptr->version);
  for (i = 0; i < MGH_MAX_DIMS; i++)
  {
    hdr_ptr->sizes[i] = ntohl(hdr_ptr->sizes[i]);
  }
  hdr_ptr->type = ntohl(hdr_ptr->type);
  hdr_ptr->dof = ntohl(hdr_ptr->dof);
  hdr_ptr->goodRASflag = ntohs(hdr_ptr->goodRASflag);

  if (hdr_ptr->version != 1)
  {
    print_error("Must be MGH version 1.\n");
    return FALSE;
  }

  if (hdr_ptr->goodRASflag)
  {
    for (i = 0; i < MGH_N_SPATIAL; i++)
    {
      hdr_ptr->spacing[i] = swapFloat(hdr_ptr->spacing[i]);
      for (j = 0; j < MGH_N_COMPONENTS; j++)
      {
        hdr_ptr->dircos[j][i] = swapFloat(hdr_ptr->dircos[j][i]);
      }
    }
  }
  else
  {
    /* Flag is zero, so just use the defaults.
     */
    for (i = 0; i < MGH_N_SPATIAL; i++)
    {
      /* Default spacing is 1.0.
       */
      hdr_ptr->spacing[i] = 1.0;

      /* Assume coronal orientation.
       */
      for (j = 0; j < MGH_N_COMPONENTS; j++)
      {
        hdr_ptr->dircos[j][i] = 0.0;
      }
      hdr_ptr->dircos[0][0] = -1.0;
      hdr_ptr->dircos[1][2] = -1.0;
      hdr_ptr->dircos[2][1] = 1.0;
    }
  }
  return TRUE;
}

static VIO_BOOL
mgh_scan_for_voxel_range(volume_input_struct *in_ptr, 
                         int n_voxels_in_slice,
                         float *min_value_ptr,
                         float *max_value_ptr)
{
  znzFile fp = (znzFile) in_ptr->volume_file;
  float min_value = FLT_MAX;
  float max_value = -FLT_MAX;
  int slice;
  float value = 0;
  int i;
  unsigned char *data_ptr;
  long int data_offset = znztell((znzFile) fp);

  if (data_offset < 0)
    return FALSE;
  
  for (slice = 0; slice < in_ptr->sizes_in_file[2]; slice++)
  {
    input_next_slice( in_ptr );
    data_ptr = in_ptr->byte_slice_buffer;
    for (i = 0; i < n_voxels_in_slice; i++)
    {
      switch (in_ptr->file_data_type)
      {
      case VIO_UNSIGNED_BYTE:
        value = *(unsigned char *)data_ptr;
        data_ptr += sizeof(unsigned char);
        break;
  
      case VIO_SIGNED_SHORT:
        value = ntohs(*(short *)data_ptr);
        data_ptr += sizeof(short);
        break;

      case VIO_SIGNED_INT:
        value = ntohl(*(int *)data_ptr);
        data_ptr += sizeof(int);
        break;

      case VIO_FLOAT:
        value = swapFloat(*(float *)data_ptr);
        data_ptr += sizeof(float);
        break;

      case VIO_NO_DATA_TYPE:
      case VIO_SIGNED_BYTE:
      case VIO_UNSIGNED_SHORT:
      case VIO_UNSIGNED_INT:
      case VIO_DOUBLE:
      case VIO_MAX_DATA_TYPE:
        break;
      }
  
      if (value < min_value )
        min_value = value;
      if (value > max_value )
        max_value = value;
    }
  }

  *min_value_ptr = min_value;
  *max_value_ptr = max_value;
  in_ptr->slice_index = 0;
  znzseek((znzFile) fp, data_offset, SEEK_SET);
  return TRUE;
}

VIOAPI  VIO_Status
initialize_mgh_format_input(VIO_STR             filename,
                            VIO_Volume          volume,
                            volume_input_struct *in_ptr)
{
  VIO_Status        status;
  int               sizes[VIO_MAX_DIMENSIONS];
  int               n_voxels_in_slice;
  int               n_bytes_per_voxel;
  nc_type           desired_nc_type;
  znzFile           fp;
  int               axis;
  struct mgh_header hdr;
  VIO_General_transform mnc_native_xform;

  VIO_Real          mnc_dircos[VIO_N_DIMENSIONS][VIO_N_DIMENSIONS];
  VIO_Real          mnc_steps[VIO_MAX_DIMENSIONS];
  VIO_Real          mnc_starts[VIO_MAX_DIMENSIONS];
  int               n_dimensions;
  nc_type           file_nc_type;
  VIO_BOOL          signed_flag;

  status = VIO_OK;

  if ((fp = znzopen(filename, "rb", 1)) == NULL)
  {
    print_error("Unable to open file %s, errno %d.\n", filename, errno);
    return VIO_ERROR;
  }

  if (!mgh_header_from_file(fp, &hdr))
  {
    znzclose(fp);
    return VIO_ERROR;
  }

  /* Translate from MGH to VIO types.
   */
  switch (hdr.type)
  {
  case MGH_TYPE_UCHAR:
    in_ptr->file_data_type = VIO_UNSIGNED_BYTE;
    file_nc_type = NC_BYTE;
    signed_flag = FALSE;
    break;
  case MGH_TYPE_INT:
    in_ptr->file_data_type = VIO_SIGNED_INT;
    file_nc_type = NC_INT;
    signed_flag = TRUE;
    break;
  case MGH_TYPE_FLOAT:
    in_ptr->file_data_type = VIO_FLOAT;
    file_nc_type = NC_FLOAT;
    signed_flag = TRUE;
    break;
  case MGH_TYPE_SHORT:
    in_ptr->file_data_type = VIO_SIGNED_SHORT;
    file_nc_type = NC_SHORT;
    signed_flag = TRUE;
    break;
  default:
    print_error("Unknown MGH data type.\n");
    znzclose(fp);
    return VIO_ERROR;
  }

  /* Decide how to store data in memory. */

  if ( get_volume_data_type(volume) == VIO_NO_DATA_TYPE )
    desired_nc_type = file_nc_type;
  else
    desired_nc_type = get_volume_nc_data_type(volume, &signed_flag);

  if( volume->spatial_axes[VIO_X] < 0 ||
      volume->spatial_axes[VIO_Y] < 0 ||
      volume->spatial_axes[VIO_Z] < 0 )
  {
    print_error("warning: setting MGH spatial axes to XYZ.\n");
    volume->spatial_axes[VIO_X] = 0;
    volume->spatial_axes[VIO_Y] = 1;
    volume->spatial_axes[VIO_Z] = 2;
  }

  /* Calculate the number of non-trivial dimensions in the file.
   */
  n_dimensions = 0;
  for_less( axis, 0, MGH_MAX_DIMS )
  {
    in_ptr->sizes_in_file[axis] = hdr.sizes[axis];
    if (hdr.sizes[axis] > 1)
    {
      n_dimensions++;
    }
  }

  if (!set_volume_n_dimensions(volume, n_dimensions))
  {
    printf("Problem setting number of dimensions to %d\n", n_dimensions);
  }

  /* Set up the correspondence between the file axes and the MINC 
   * spatial axes. Each row contains the 'x', 'y', and 'z' components
   * along the right/left, anterior/posterior, or superior/inferior
   * axes (RAS). The "xspace" axis will be the one that has the largest
   * component in the RL direction, "yspace" refers to AP, and "zspace" 
   * to SI. This tells us both how to convert the transform and how the
   * file data is arranged.
   */
  for_less( axis, 0, MGH_N_SPATIAL )
  {
    int spatial_axis = VIO_X;
    float c_x = fabs(hdr.dircos[axis][VIO_X]);
    float c_y = fabs(hdr.dircos[axis][VIO_Y]);
    float c_z = fabs(hdr.dircos[axis][VIO_Z]);

    if (c_y > c_x && c_y > c_z)
    {
      spatial_axis = VIO_Y;
    }
    if (c_z > c_x && c_z > c_y)
    {
      spatial_axis = VIO_Z;
    }
    in_ptr->axis_index_from_file[axis] = spatial_axis;
  }

  mgh_header_to_linear_transform(&hdr, in_ptr, TRUE, &mnc_native_xform);

  convert_transform_to_starts_and_steps(&mnc_native_xform,
                                        VIO_N_DIMENSIONS,
                                        NULL,
                                        volume->spatial_axes,
                                        mnc_starts,
                                        mnc_steps,
                                        mnc_dircos);

  delete_general_transform(&mnc_native_xform);

  for_less( axis, 0, VIO_N_DIMENSIONS)
  {
    int volume_axis = volume->spatial_axes[axis];
    int file_axis = in_ptr->axis_index_from_file[volume_axis];
    sizes[file_axis] = in_ptr->sizes_in_file[volume_axis];
    set_volume_direction_cosine(volume, volume_axis, mnc_dircos[volume_axis]);
  }
#if DEBUG
  for_less( axis, 0, VIO_N_DIMENSIONS)
  {
    int volume_axis = volume->spatial_axes[axis];

    printf("%d %d size:%4d step:%6.3f start:%9.4f dc:[%7.4f %7.4f %7.4f]\n", 
           axis,
           in_ptr->axis_index_from_file[volume_axis],
           sizes[volume_axis],
           mnc_steps[volume_axis],
           mnc_starts[volume_axis],
           mnc_dircos[volume_axis][0], 
           mnc_dircos[volume_axis][1], 
           mnc_dircos[volume_axis][2]);
  }
#endif // DEBUG
  set_volume_separations( volume, mnc_steps );
  set_volume_starts( volume, mnc_starts );

  /* If we are a 4D image, we need to copy the size here.
   */
  sizes[3] = in_ptr->sizes_in_file[3];

  set_volume_type( volume, desired_nc_type, signed_flag, 0.0, 0.0 );
  set_volume_sizes( volume, sizes );

  n_bytes_per_voxel = get_type_size( in_ptr->file_data_type );

  n_voxels_in_slice = (in_ptr->sizes_in_file[0] *
                       in_ptr->sizes_in_file[1]);

  in_ptr->min_value = FLT_MAX;
  in_ptr->max_value = -FLT_MAX;

  /* Allocate the slice buffer. */

  ALLOC(in_ptr->byte_slice_buffer, n_voxels_in_slice * n_bytes_per_voxel);

  in_ptr->volume_file = (FILE *) fp;

  in_ptr->slice_index = 0;

  /* If the data must be converted to byte, read the entire image file simply
   * to find the max and min values. This allows us to set the value_scale and
   * value_translation properly when we read the file.
   */
  if (get_volume_data_type(volume) != in_ptr->file_data_type )
  {
    float min_value, max_value;

    mgh_scan_for_voxel_range(in_ptr, n_voxels_in_slice, &min_value, &max_value);
    set_volume_voxel_range(volume, min_value, max_value);
  }
  return VIO_OK;
}

VIOAPI void
delete_mgh_format_input(
                        volume_input_struct   *in_ptr
                        )
{
  znzFile fp = (znzFile) in_ptr->volume_file;

  FREE( in_ptr->byte_slice_buffer );

  znzclose( fp );
}

VIOAPI  VIO_BOOL
input_more_mgh_format_file(
                           VIO_Volume          volume,
                           volume_input_struct *in_ptr,
                           VIO_Real            *fraction_done
                           )
{
  int            i;
  VIO_Real       value = 0;
  VIO_Status     status;
  VIO_Real       value_translation, value_scale;
  VIO_Real       original_min_voxel, original_max_voxel;
  int            *inner_index, indices[VIO_MAX_DIMENSIONS];
  unsigned char  *data_ptr;

  if ( in_ptr->slice_index < in_ptr->sizes_in_file[2] )
  {
    /* If the memory for the volume has not been allocated yet,
     * initialize that memory now.
     */
    if (!volume_is_alloced(volume))
    {
      alloc_volume_data(volume);
      if (!volume_is_alloced(volume))
      {
        print_error("Failed to allocate volume.\n");
        return FALSE;
      }
    }

    status = input_next_slice( in_ptr );

    /* See if we need to apply scaling to this slice. This is only
     * needed if the volume voxel type is not the same as the file
     * voxel type. THIS IS ONLY REALLY LEGAL FOR BYTE VOLUME TYPES.
     */
    if (get_volume_data_type(volume) != in_ptr->file_data_type)
    {
      get_volume_voxel_range(volume, &original_min_voxel, &original_max_voxel);
      value_translation = original_min_voxel;
      value_scale = (original_max_voxel - original_min_voxel) /
        (VIO_Real) (NUM_BYTE_VALUES - 1);
    }
    else
    {
      /* Just do the trivial scaling. */
      value_translation = 0.0;
      value_scale = 1.0;
    }

    /* Set up the indices.
     */
    inner_index = &indices[in_ptr->axis_index_from_file[0]];

    indices[in_ptr->axis_index_from_file[2]] = in_ptr->slice_index - 1;

    if ( status == VIO_OK )
    {
      data_ptr = in_ptr->byte_slice_buffer;

      for_less( i, 0, in_ptr->sizes_in_file[1] )
      {
        indices[in_ptr->axis_index_from_file[1]] = i;
        for_less( *inner_index, 0, in_ptr->sizes_in_file[0] )
        {
          switch ( in_ptr->file_data_type )
          {
          case VIO_UNSIGNED_BYTE:
            value = *(unsigned char *)data_ptr;
            data_ptr += sizeof(unsigned char);
            break;
          case VIO_SIGNED_SHORT:
            value = ntohs(*(short *)data_ptr);
            data_ptr += sizeof(short);
            break;
          case VIO_SIGNED_INT:
            value = ntohl(*(int *)data_ptr);
            data_ptr += sizeof(int);
            break;
          case VIO_FLOAT:
            value = swapFloat(*(float *)data_ptr);
            data_ptr += sizeof(float);
            break;
          default:
            handle_internal_error( "input_more_mgh_format_file" );
            break;
          }
          value = (value - value_translation) / value_scale;
          if (value > in_ptr->max_value)
          {
            in_ptr->max_value = value;
          }
          if (value < in_ptr->min_value)
          {
            in_ptr->min_value = value;
          }
          set_volume_voxel_value( volume,
                                  indices[VIO_X],
                                  indices[VIO_Y],
                                  indices[VIO_Z],
                                  0,
                                  0,
                                  value);
        }
      }
    }
  }

  *fraction_done = (VIO_Real) in_ptr->slice_index / in_ptr->sizes_in_file[2];

  /* See if we are all done. If so, we need to perform a final check
   * of the volume to set the ranges appropriately.
   */
  if (in_ptr->slice_index == in_ptr->sizes_in_file[2])
  {
    set_volume_voxel_range( volume, in_ptr->min_value, in_ptr->max_value );

    /* Make sure we scale the data up to the original real range,
     * if appropriate.
     */
    if (get_volume_data_type(volume) != in_ptr->file_data_type)
    {
      set_volume_real_range(volume, original_min_voxel, original_max_voxel);
    }

    return FALSE;
  }
  else
  {
    return TRUE;                /* More work to do. */
  }
}
