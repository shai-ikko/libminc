#include  <internal_volume_io.h>
#include  <minc.h>

#ifndef lint
static char rcsid[] = "$Header: /private-cvsroot/minc/volume_io/Volumes/input_free.c,v 1.18 1994-11-25 14:20:10 david Exp $";
#endif

#define  DEFAULT_SUFFIX  "fre"

#define  NUM_BYTE_VALUES      256

private  Status  input_slice(
    volume_input_struct   *volume_input );

/* ----------------------------- MNI Header -----------------------------------
@NAME       : initialize_free_format_input
@INPUT      : 
@OUTPUT     : volume
              volume_input
@RETURNS    : OK if successful
@DESCRIPTION: Initializes loading a free format file by reading the header.
              If the file contains short, but the convert_to_byte flag is set
              in volume_input, then the file is converted to bytes on input.
              This function assumes that volume->filename has been assigned.
@CREATED    :                      David MacDonald
@MODIFIED   : 
---------------------------------------------------------------------------- */

public  Status  initialize_free_format_input(
    char                 filename[],
    Volume               volume,
    volume_input_struct  *volume_input )
{
    Status         status, file_status;
    STRING         volume_filename, abs_volume_filename;
    STRING         slice_filename;
    int            slice, sizes[N_DIMENSIONS];
    int            c, volume_byte_offset;
    int            n_bytes_per_voxel, min_value, max_value, i;
    int            n_slices, n_voxels_in_slice;
    nc_type        desired_data_type;
    unsigned short value;
    char           ch;
    Real           file_separations[MAX_DIMENSIONS];
    Real           volume_separations[MAX_DIMENSIONS];
    Real           origin_voxel[MAX_DIMENSIONS];
    Real           trans[N_DIMENSIONS];
    FILE           *file;
    BOOLEAN        axis_valid;
    int            axis;

    status = OK;

    file_status = open_file_with_default_suffix(
                         filename, DEFAULT_SUFFIX,
                         READ_FILE, ASCII_FORMAT, &file );

    status = file_status;

    /* read the line containing: n_bytes_per_voxel
    */

    if( status == OK )
        status = input_int( file, &n_bytes_per_voxel );

    /* input the 3 translation values used for the voxel_to_world_transform */

    if( status == OK &&
        (input_real( file, &trans[X] ) != OK ||
         input_real( file, &trans[Y] ) != OK ||
         input_real( file, &trans[Z] ) != OK) )
    {
        print( "Error reading x,y,z translations from %s.\n", filename );
        status = ERROR;
    }

    if( status == OK )
    {
        /* decide what type of data is in image file */

        if( n_bytes_per_voxel == 1 )
            volume_input->file_data_type = UNSIGNED_BYTE;
        else if( n_bytes_per_voxel == 2 )
            volume_input->file_data_type = UNSIGNED_SHORT;
        else
        {
            print( "Must be either 1 or 2 bytes per voxel.\n" );
            status = ERROR;
        }

        /* decide how to store data in memory */

        if( volume->data_type == NO_DATA_TYPE )
            desired_data_type = volume_input->file_data_type;
        else
            desired_data_type = volume->nc_data_type;
    }

    /* read 3 lines, 1 for each axis:

            number_voxels   +/-voxel_separation  x|y|z

       where the x, y, or z is used to indicate the ordering of the axes
             within the file, with the 3rd one being the fastest varying index.
             negative voxel separation means flip on display 
    */

    if( volume->spatial_axes[X] < 0 ||
        volume->spatial_axes[Y] < 0 ||
        volume->spatial_axes[Z] < 0 )
    {
        print(
         "warning initialize_free_format_input: setting spatial axes to XYZ.\n");
        volume->spatial_axes[X] = 0;
        volume->spatial_axes[Y] = 1;
        volume->spatial_axes[Z] = 2;
    }

    if( status == OK )
    for_less( axis, 0, N_DIMENSIONS )
    {
        status = ERROR;

        if( input_int( file, &volume_input->sizes_in_file[axis] ) != OK )
            break;

        if( input_real( file, &file_separations[axis] ) != OK )
            break;

        if( input_nonwhite_character( file, &ch ) != OK )
            break;

        axis_valid = TRUE;

        switch( ch )
        {
        case 'x':
        case 'X':  volume_input->axis_index_from_file[axis] =
                                           volume->spatial_axes[X];
                   break;

        case 'y':
        case 'Y':  volume_input->axis_index_from_file[axis] =
                                           volume->spatial_axes[Y];
                   break;

        case 'z':
        case 'Z':  volume_input->axis_index_from_file[axis] =
                                           volume->spatial_axes[Z];
                   break;

        default:   axis_valid = FALSE;    break;
        }

        if( !axis_valid )
        {
            print( "Invalid axis.\n" );
            break;
        }

        status = OK;
    }

    for_less( c, 0, N_DIMENSIONS )
    {
        volume->spatial_axes[c] = c;
        volume->direction_cosines[c][X] = 0.0;
        volume->direction_cosines[c][Y] = 0.0;
        volume->direction_cosines[c][Z] = 0.0;
        volume->direction_cosines[c][c] = 1.0;
    }

    if( status == OK &&
        (volume_input->axis_index_from_file[X] ==
         volume_input->axis_index_from_file[Y] ||
         volume_input->axis_index_from_file[X] ==
         volume_input->axis_index_from_file[Z] ||
         volume_input->axis_index_from_file[Y] ==
         volume_input->axis_index_from_file[Z]) )
    {
        print( "Two axis indices are equal.\n" );
        status = ERROR;
    }

    extract_directory( filename, volume_input->directory );

    if( status == OK )
    if( volume_input->sizes_in_file[0] <= 0 )
    {
        n_slices = 0;

        while( input_string( file, slice_filename, MAX_STRING_LENGTH, ' ' )==OK)
        {
            SET_ARRAY_SIZE( volume_input->slice_filenames, n_slices, n_slices+1,
                            DEFAULT_CHUNK_SIZE );
            (void) strcpy( volume_input->slice_filenames[n_slices],
                           slice_filename );
            SET_ARRAY_SIZE( volume_input->slice_byte_offsets,
                            n_slices, n_slices+1, DEFAULT_CHUNK_SIZE );

            if( input_int( file, &volume_input->slice_byte_offsets[n_slices] )
                != OK )
            {
                volume_input->slice_byte_offsets[n_slices] = 0;
            }

            ++n_slices;
        }

        volume_input->sizes_in_file[0] = n_slices;
        volume_input->one_file_per_slice = TRUE;
    }
    else
    {
        volume_input->one_file_per_slice = FALSE;
        status = input_string( file, volume_filename, MAX_STRING_LENGTH, ' ' );
        get_absolute_filename( volume_filename, volume_input->directory,
                               abs_volume_filename );

        if( input_int( file, &volume_byte_offset ) != OK )
            volume_byte_offset = 0;

    }

    if( status == OK )
        status = close_file( file );

    /* record the information in the volume struct */

    if( status == OK )
    {
        for_less( axis, 0, N_DIMENSIONS )
        {
            sizes[volume_input->axis_index_from_file[axis]] =
                                 volume_input->sizes_in_file[axis];
            volume_separations[volume_input->axis_index_from_file[axis]] =
                                                     file_separations[axis];

            if( volume_separations[axis] < 0.0 )
            {
                trans[axis] += -volume_separations[axis] *
                               (Real) (sizes[axis]-1);
            }
        }

        for_less( axis, 0, N_DIMENSIONS )
            origin_voxel[axis] = 0.0;

        set_volume_separations( volume, volume_separations );
        set_volume_translation( volume, origin_voxel, trans );
    }

    set_volume_type( volume, desired_data_type, FALSE, 0.0, 0.0 );
    set_volume_sizes( volume, sizes );

    /* allocate the slice buffer */

    n_voxels_in_slice = volume_input->sizes_in_file[1] *
                        volume_input->sizes_in_file[2];

    if( status == OK )
    switch( volume_input->file_data_type )
    {
    case  UNSIGNED_BYTE:
        ALLOC( volume_input->byte_slice_buffer, n_voxels_in_slice );
        break;

    case  UNSIGNED_SHORT:
        ALLOC( volume_input->short_slice_buffer, n_voxels_in_slice );
        break;

    default:
        handle_internal_error( "initialize_free_format_input" );
        break;
    }

    /* if the data must be converted to byte, read the entire image file simply
       to find the max and min values, and set the value_scale and
       value_translation */

    if( status == OK && volume->data_type != volume_input->file_data_type )
    {
        if( status == OK && !volume_input->one_file_per_slice )
        {
            status = open_file( abs_volume_filename, READ_FILE, BINARY_FORMAT,
                                &volume_input->volume_file );

            if( status == OK )
                status = set_file_position( file, (long) volume_byte_offset );
        }

        min_value = 0;
        max_value = 0;

        volume_input->slice_index = 0;

        for_less( slice, 0, volume_input->sizes_in_file[0] )
        {
            (void) input_slice( volume_input );

            for_less( i, 0, n_voxels_in_slice )
            {
                value = (int) volume_input->short_slice_buffer[i];
                if( slice == 0 && i == 0 || value < min_value )
                    min_value = value;
                if( slice == 0 && i == 0 || value > max_value )
                    max_value = value;
            }

        }

        set_volume_voxel_range( volume, min_value, max_value );

        if( status == OK && !volume_input->one_file_per_slice )
            status = close_file( volume_input->volume_file );
    }

    if( status == OK && !volume_input->one_file_per_slice )
    {
        get_absolute_filename( volume_filename, volume_input->directory,
                               abs_volume_filename );
        status = open_file( abs_volume_filename, READ_FILE, BINARY_FORMAT,
                            &volume_input->volume_file );

        if( status == OK )
            status = set_file_position( file, (long) volume_byte_offset );
    }

    volume_input->slice_index = 0;

    return( status );
}

/* ----------------------------- MNI Header -----------------------------------
@NAME       : delete_free_format_input
@INPUT      : volume_input
@OUTPUT     : 
@RETURNS    : 
@DESCRIPTION: Frees the slice buffer and closes the image file.
@CREATED    :                      David MacDonald
@MODIFIED   : 
---------------------------------------------------------------------------- */

public  void  delete_free_format_input(
    volume_input_struct   *volume_input )
{
    if( volume_input->file_data_type == UNSIGNED_BYTE )
    {
        FREE( volume_input->byte_slice_buffer );
    }
    else
    {
        FREE( volume_input->short_slice_buffer );
    }

    if( volume_input->one_file_per_slice )
    {
        FREE( volume_input->slice_filenames );
        FREE( volume_input->slice_byte_offsets );
    }
    else
    {
        (void) close_file( volume_input->volume_file );
    }
}

/* ----------------------------- MNI Header -----------------------------------
@NAME       : input_slice
@INPUT      : volume
              volume_input
@OUTPUT     : 
@RETURNS    : OK if success
@DESCRIPTION: Reads the next slice from the volume.
@CREATED    :                      David MacDonald
@MODIFIED   : 
---------------------------------------------------------------------------- */

private  Status  input_slice(
    volume_input_struct   *volume_input )
{
    Status           status;
    FILE             *file;
    STRING           slice_filename;

    status = OK;

    if( volume_input->slice_index < volume_input->sizes_in_file[0] )
    {
        if( volume_input->one_file_per_slice )
        {
            get_absolute_filename(
                   volume_input->slice_filenames[volume_input->slice_index],
                   volume_input->directory, slice_filename );
            status = open_file( slice_filename, READ_FILE, BINARY_FORMAT,
                                &file );
            if( status == OK )
                status = set_file_position( file,
                           (long) (volume_input->slice_byte_offsets
                                           [volume_input->slice_index]) );
        }
        else
            file = volume_input->volume_file;

        if( status == OK )
        switch( volume_input->file_data_type )
        {
        case  UNSIGNED_BYTE:
            status = io_binary_data( file, READ_FILE,
                                 (void *) volume_input->byte_slice_buffer,
                                 sizeof(volume_input->byte_slice_buffer[0]),
                                 volume_input->sizes_in_file[1] *
                                 volume_input->sizes_in_file[2] );
            break;

        case  UNSIGNED_SHORT:
            status = io_binary_data( file, READ_FILE,
                                 (void *) volume_input->short_slice_buffer,
                                 sizeof(volume_input->short_slice_buffer[0]),
                                 volume_input->sizes_in_file[1] *
                                 volume_input->sizes_in_file[2] );
            break;

        default:
            handle_internal_error( "input_slice" );
            break;
        }

        if( status == OK && volume_input->one_file_per_slice )
        {
            status = close_file( file );
        }

        ++volume_input->slice_index;
    }
    else
        status = ERROR;

    return( status );
}

/* ----------------------------- MNI Header -----------------------------------
@NAME       : input_more_free_format_file
@INPUT      : volume
              volume_input
@OUTPUT     : fraction_done
@RETURNS    : TRUE if more to input after this call
@DESCRIPTION: Reads in one more slice from the image file.
@CREATED    :                      David MacDonald
@MODIFIED   : 
---------------------------------------------------------------------------- */

public  BOOLEAN  input_more_free_format_file(
    Volume                volume,
    volume_input_struct   *volume_input,
    Real                  *fraction_done )
{
    int             min_value, max_value;
    int             x, y, z, sizes[MAX_DIMENSIONS];
    Status          status;
    BOOLEAN         more_to_do, scaling_flag;
    Real            value_translation, value_scale;
    Real            original_min_voxel, original_max_voxel;
    int             *inner_index, i, value, indices[MAX_DIMENSIONS];
    unsigned char   *byte_buffer_ptr;
    unsigned short  *short_buffer_ptr;

    if( volume_input->slice_index < volume_input->sizes_in_file[0] )
    {
        if( volume->data == (void *) NULL )
            alloc_volume_data( volume );

        status = input_slice( volume_input );

        scaling_flag = FALSE;
        if( volume->data_type != volume_input->file_data_type )
        {
            scaling_flag = TRUE;
            get_volume_voxel_range( volume, &original_min_voxel,
                                    &original_max_voxel );
            value_translation = original_min_voxel;
            value_scale = (original_max_voxel - original_min_voxel) /
                          (Real) (NUM_BYTE_VALUES - 1);
        }

        inner_index = &indices[volume_input->axis_index_from_file[2]];

        indices[volume_input->axis_index_from_file[0]] =
                                         volume_input->slice_index-1;

        if( status == OK )
        switch( volume_input->file_data_type )
        {
        case  UNSIGNED_BYTE:
            byte_buffer_ptr = volume_input->byte_slice_buffer;
            for_less( i, 0, volume_input->sizes_in_file[1] )
            {
                indices[volume_input->axis_index_from_file[1]] = i;
                for_less( *inner_index, 0, volume_input->sizes_in_file[2] )
                {
                    if( scaling_flag )
                    {
                        value = (int)(
                         ((Real) (*byte_buffer_ptr) - value_translation) /
                                   value_scale );

                        if( value < 0 )
                            value = 0;
                        else if( value > 255 )
                            value = 255;
                    }
                    else
                        value = (int) (*byte_buffer_ptr);

                    SET_VOXEL_3D( volume, indices[X], indices[Y], indices[Z],
                                  value );

                    ++byte_buffer_ptr;
                }
            }
            break;

        case  UNSIGNED_SHORT:
            short_buffer_ptr = volume_input->short_slice_buffer;
            for_less( i, 0, volume_input->sizes_in_file[1] )
            {
                indices[volume_input->axis_index_from_file[1]] = i;
                for_less( *inner_index, 0, volume_input->sizes_in_file[2] )
                {
                    if( scaling_flag )
                    {
                        value = (int)(
                         ((Real) (*short_buffer_ptr) - value_translation) /
                                               value_scale );
                    }
                    else
                        value = (int) (*short_buffer_ptr);

                    SET_VOXEL_3D( volume, indices[X], indices[Y], indices[Z],
                                  value );

                    ++short_buffer_ptr;
                }
            }
            break;

        default:
            handle_internal_error( "input_more_free_format_file" );
            break;
        }
    }

    *fraction_done = (Real) volume_input->slice_index /
                   (Real) volume->sizes[volume_input->axis_index_from_file[0]];

    more_to_do = TRUE;

    if( volume_input->slice_index ==
        volume->sizes[volume_input->axis_index_from_file[0]] )
    {
        more_to_do = FALSE;

        GET_VOXEL_3D( min_value, volume, 0, 0, 0 );
        get_volume_sizes( volume, sizes );
        max_value = min_value;
        for_less( x, 0, sizes[X] )
        {
            for_less( y, 0, sizes[Y] )
            {
                for_less( z, 0, sizes[Z] )
                {
                    GET_VOXEL_3D( value, volume, x, y, z );
                    if( value < min_value )
                        min_value = value;
                    else if( value > max_value )
                        max_value = value;
                }
            }
        }

        set_volume_voxel_range( volume, min_value, max_value );

        if( volume->data_type != volume_input->file_data_type )
        {
            set_volume_real_range( volume, original_min_voxel,
                                   original_max_voxel );
        }
    }

    return( more_to_do );
}
