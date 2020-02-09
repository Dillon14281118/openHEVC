/*
 * HEVC Parameter Set decoding
 *
 * Copyright (C) 2012 - 2103 Guillaume Martres
 * Copyright (C) 2012 - 2103 Mickael Raulet
 * Copyright (C) 2012 - 2013 Gildas Cocherel
 * Copyright (C) 2013 Vittorio Giovara
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/imgutils.h"
#include "golomb.h"
#include "hevc_data.h"
#include "hevc_ps.h"
#include "hevcdec.h"

static const uint8_t default_scaling_list_intra[] = {
    16, 16, 16, 16, 17, 18, 21, 24,
    16, 16, 16, 16, 17, 19, 22, 25,
    16, 16, 17, 18, 20, 22, 25, 29,
    16, 16, 18, 21, 24, 27, 31, 36,
    17, 17, 20, 24, 30, 35, 41, 47,
    18, 19, 22, 27, 35, 44, 54, 65,
    21, 22, 25, 31, 41, 54, 70, 88,
    24, 25, 29, 36, 47, 65, 88, 115
};

static const uint8_t default_scaling_list_inter[] = {
    16, 16, 16, 16, 17, 18, 20, 24,
    16, 16, 16, 17, 18, 20, 24, 25,
    16, 16, 17, 18, 20, 24, 25, 28,
    16, 17, 18, 20, 24, 25, 28, 33,
    17, 18, 20, 24, 25, 28, 33, 41,
    18, 20, 24, 25, 28, 33, 41, 54,
    20, 24, 25, 28, 33, 41, 54, 71,
    24, 25, 28, 33, 41, 54, 71, 91
};

static const AVRational vui_sar[] = {
    {  0,   1 },
    {  1,   1 },
    { 12,  11 },
    { 10,  11 },
    { 16,  11 },
    { 40,  33 },
    { 24,  11 },
    { 20,  11 },
    { 32,  11 },
    { 80,  33 },
    { 18,  11 },
    { 15,  11 },
    { 64,  33 },
    { 160, 99 },
    {  4,   3 },
    {  3,   2 },
    {  2,   1 },
};

static void remove_pps(HEVCParamSets *s, int id)
{
    if (s->pps_list[id] && s->pps == (const HEVCPPS*)s->pps_list[id]->data)
        s->pps = NULL;
    av_buffer_unref(&s->pps_list[id]);
}

static void remove_sps(HEVCParamSets *s, int id)
{
    int i;
    if (s->sps_list[id]) {
        if (s->sps == (const HEVCSPS*)s->sps_list[id]->data)
            s->sps = NULL;

        /* drop all PPS that depend on this SPS */
        for (i = 0; i < FF_ARRAY_ELEMS(s->pps_list); i++)
            if (s->pps_list[i] && ((HEVCPPS*)s->pps_list[i]->data)->sps_id == id)
                remove_pps(s, i);

        av_assert0(!(s->sps_list[id] && s->sps == (HEVCSPS*)s->sps_list[id]->data));
    }
    av_buffer_unref(&s->sps_list[id]);
}

static void remove_vps(HEVCParamSets *s, int id)
{
    int i;
    if (s->vps_list[id]) {
        if (s->vps == (const HEVCVPS*)s->vps_list[id]->data)
            s->vps = NULL;

        for (i = 0; i < FF_ARRAY_ELEMS(s->sps_list); i++)
            if (s->sps_list[i] && ((HEVCSPS*)s->sps_list[i]->data)->vps_id == id)
                remove_sps(s, i);
    }
    av_buffer_unref(&s->vps_list[id]);
}

int ff_hevc_decode_short_term_rps(GetBitContext *gb, AVCodecContext *avctx,
                                  ShortTermRPS *rps, const HEVCSPS *sps, int is_slice_header)
{
    uint8_t inter_ref_pic_set_prediction_flag = 0;
    int delta_poc;
    int k0 = 0;
    int k1 = 0;
    int k  = 0;
    int i;

    if (rps != sps->st_rps && sps->num_short_term_rps)
        inter_ref_pic_set_prediction_flag = get_bits1(gb);

    if (inter_ref_pic_set_prediction_flag) {
        const ShortTermRPS *rps_ridx;
        int delta_rps;
        unsigned abs_delta_rps;
        uint8_t use_delta_flag = 0;
        uint8_t delta_rps_sign;

        if (is_slice_header) {
            unsigned int delta_idx = get_ue_golomb_long(gb) + 1;
            if (delta_idx > sps->num_short_term_rps) {
                av_log(avctx, AV_LOG_ERROR,
                       "Invalid value of delta_idx in slice header RPS: %d > %d.\n",
                       delta_idx, sps->num_short_term_rps);
                return AVERROR_INVALIDDATA;
            }
            rps_ridx = &sps->st_rps[sps->num_short_term_rps - delta_idx];
            rps->rps_idx_num_delta_pocs = rps_ridx->num_delta_pocs;
        } else
            rps_ridx = &sps->st_rps[rps - sps->st_rps - 1];

        delta_rps_sign = get_bits1(gb);
        abs_delta_rps  = get_ue_golomb_long(gb) + 1;
        if (abs_delta_rps < 1 || abs_delta_rps > 32768) {
            av_log(avctx, AV_LOG_ERROR,
                   "Invalid value of abs_delta_rps: %d\n",
                   abs_delta_rps);
            return AVERROR_INVALIDDATA;
        }
        delta_rps      = (1 - (delta_rps_sign << 1)) * abs_delta_rps;
        for (i = 0; i <= rps_ridx->num_delta_pocs; i++) {
            int used = rps->used_by_curr_pic_flag[k] = get_bits1(gb);

            if (!used)
                use_delta_flag = get_bits1(gb);

            if (used || use_delta_flag) {
                if (i < rps_ridx->num_delta_pocs)
                    delta_poc = delta_rps + rps_ridx->delta_poc[i];
                else
                    delta_poc = delta_rps;
                rps->delta_poc[k] = delta_poc;
                if (delta_poc < 0)
                    k0++;
                else
                    k1++;
                k++;
            }
        }

        rps->num_delta_pocs    = k;
        rps->num_negative_pics = k0;
        // sort in increasing order (smallest first)
        if (rps->num_delta_pocs != 0) {
            int used, tmp;
            for (i = 1; i < rps->num_delta_pocs; i++) {
                delta_poc = rps->delta_poc[i];
                used      = rps->used_by_curr_pic_flag[i];
                for (k = i - 1; k >= 0; k--) {
                    tmp = rps->delta_poc[k];
                    if (delta_poc < tmp) {
                        rps->delta_poc[k + 1] = tmp;
                        rps->used_by_curr_pic_flag[k + 1]      = rps->used_by_curr_pic_flag[k];
                        rps->delta_poc[k]     = delta_poc;
                        rps->used_by_curr_pic_flag[k]          = used;
                    }
                }
            }
        }
        if ((rps->num_negative_pics >> 1) != 0) {
            int used;
            k = rps->num_negative_pics - 1;
            // flip the negative values to largest first
            for (i = 0; i < rps->num_negative_pics >> 1; i++) {
                delta_poc         = rps->delta_poc[i];
                used              = rps->used_by_curr_pic_flag[i];
                rps->delta_poc[i] = rps->delta_poc[k];
                rps->used_by_curr_pic_flag[i]      = rps->used_by_curr_pic_flag[k];
                rps->delta_poc[k] = delta_poc;
                rps->used_by_curr_pic_flag[k]      = used;
                k--;
            }
        }
    } else {
        unsigned int prev, nb_positive_pics;
        rps->num_negative_pics = get_ue_golomb_long(gb);
        nb_positive_pics       = get_ue_golomb_long(gb);

        if (rps->num_negative_pics >= HEVC_MAX_REFS ||
            nb_positive_pics >= HEVC_MAX_REFS) {
            av_log(avctx, AV_LOG_ERROR, "Too many refs in a short term RPS.\n");
            return AVERROR_INVALIDDATA;
        }

        rps->num_delta_pocs = rps->num_negative_pics + nb_positive_pics;
        if (rps->num_delta_pocs) {
            prev = 0;
            for (i = 0; i < rps->num_negative_pics; i++) {
                delta_poc = get_ue_golomb_long(gb) + 1;
                prev -= delta_poc;
                rps->delta_poc[i] = prev;
                rps->used_by_curr_pic_flag[i]      = get_bits1(gb);
            }
            prev = 0;
            for (i = 0; i < nb_positive_pics; i++) {
                delta_poc = get_ue_golomb_long(gb) + 1;
                prev += delta_poc;
                rps->delta_poc[rps->num_negative_pics + i] = prev;
                rps->used_by_curr_pic_flag[rps->num_negative_pics + i]      = get_bits1(gb);
            }
        }
    }
    return 0;
}


static int decode_profile_tier_level(GetBitContext *gb, AVCodecContext *avctx,
                                      PTLCommon *ptl)
{
    int i;

    if (get_bits_left(gb) < 2+1+5 + 32 + 4 + 16 + 16 + 12)
        return -1;

    ptl->profile_space = get_bits(gb, 2);
    ptl->tier_flag     = get_bits1(gb);
    ptl->profile_idc   = get_bits(gb, 5);

    if (ptl->profile_idc == FF_PROFILE_HEVC_MAIN)
        av_log(avctx, AV_LOG_DEBUG, "Main profile bitstream\n");
    else if (ptl->profile_idc == FF_PROFILE_HEVC_MAIN_10)
        av_log(avctx, AV_LOG_DEBUG, "Main 10 profile bitstream\n");
    else if (ptl->profile_idc == FF_PROFILE_HEVC_MAIN_STILL_PICTURE)
        av_log(avctx, AV_LOG_DEBUG, "Main Still Picture profile bitstream\n");
    else if (ptl->profile_idc == FF_PROFILE_HEVC_REXT)
        av_log(avctx, AV_LOG_DEBUG, "Range Extension profile bitstream\n");
    else if (ptl->profile_idc == FF_PROFILE_HEVC_HIGHTHROUGHPUTREXT)
        av_log(avctx, AV_LOG_DEBUG, "Highthroughput Range Extension profile bitstream\n");
    else if (ptl->profile_idc == FF_PROFILE_HEVC_MULTIVIEWMAIN)
        av_log(avctx, AV_LOG_DEBUG, "Mutiview Main profile bitstream\n");
    else if (ptl->profile_idc == FF_PROFILE_HEVC_SCALABLEMAIN)
        av_log(avctx, AV_LOG_DEBUG, "Scalable Main profile bitstream\n");
    else if (ptl->profile_idc == FF_PROFILE_HEVC_SCALABLEMAIN10)
        av_log(avctx, AV_LOG_DEBUG, "Scalable Main 10 profile bitstream\n");
    else
        av_log(avctx, AV_LOG_WARNING, "Unknown HEVC profile: %d\n", ptl->profile_idc);

    for (i = 0; i < 32; i++) {
        ptl->profile_compatibility_flag[i] = get_bits1(gb);
    }
    ptl->progressive_source_flag    = get_bits1(gb);
    ptl->interlaced_source_flag     = get_bits1(gb);
    ptl->non_packed_constraint_flag = get_bits1(gb);
    ptl->frame_only_constraint_flag = get_bits1(gb);


#if MULTIPLE_PTL_SUPPORT
    if( ptl->profile_idc == FF_PROFILE_HEVC_REXT || ptl->profile_compatibility_flag[4] ||
        ptl->profile_idc == FF_PROFILE_HEVC_HIGHTHROUGHPUTREXT || ptl->profile_compatibility_flag[5] ||
        ptl->profile_idc == FF_PROFILE_HEVC_MULTIVIEWMAIN || ptl->profile_compatibility_flag[6] ||
        ptl->profile_idc == FF_PROFILE_HEVC_SCALABLEMAIN || ptl->profile_compatibility_flag[7] ) {
        get_bits1(gb); // general_max_12bit_constraint_flag
        get_bits1(gb); //general_max_10bit_constraint_flag
        ptl->setProfileIdc = (get_bits1(gb)) ? FF_PROFILE_HEVC_SCALABLEMAIN : FF_PROFILE_HEVC_SCALABLEMAIN10; //general_max_8bit_constraint_flag
        get_bits1(gb);   //general_max_422chroma_constraint_flag
        get_bits1(gb);   //general_max_420chroma_constraint_flag
        get_bits1(gb);   //general_max_monochrome_constraint_flag
        get_bits1(gb);   //general_intra_constraint_flag
        get_bits1(gb);   //general_one_picture_only_constraint_flag
        get_bits1(gb);   //general_lower_bit_rate_constraint_flag
       
        skip_bits(gb, 32); //general_reserved_zero_34bits
        skip_bits(gb, 2);//general_reserved_zero_34bits
    } else {
        skip_bits(gb, 32); // general_reserved_zero_43bits
        skip_bits(gb, 11); // general_reserved_zero_43bits
    }
    if( ( ptl->profile_idc >= 1 && ptl->profile_idc <= 5 ) ||
        ptl->profile_compatibility_flag[1] || ptl->profile_compatibility_flag[2] ||
        ptl->profile_compatibility_flag[3] || ptl->profile_compatibility_flag[4] ||
        ptl->profile_compatibility_flag[5]) {

        ptl->general_inbld_flag = get_bits1(gb);
    } else {
        get_bits1(gb);// general_reserved_zero_bit
    }
#else
    skip_bits(gb, 16); // XXX_reserved_zero_44bits[0..15]
    skip_bits(gb, 16); // XXX_reserved_zero_44bits[16..31]
    skip_bits(gb, 12); // XXX_reserved_zero_44bits[32..43]
#endif
    return 0;
}

static int parse_ptl(GetBitContext *gb, AVCodecContext *avctx,
                      PTL *ptl, int max_num_sub_layers, int profile_present_flag)
{
    int i;
    if (profile_present_flag){
        if (decode_profile_tier_level(gb, avctx, &ptl->general_ptl) < 0 ||
            get_bits_left(gb) < 8 + (8*2 * (max_num_sub_layers - 1 > 0))) {
            av_log(avctx, AV_LOG_ERROR, "PTL information too short\n");
            return -1;
        }
    }

    ptl->general_ptl.level_idc = get_bits(gb, 8);

    for (i = 0; i < max_num_sub_layers - 1; i++) {
        ptl->sub_layer_profile_present_flag[i] = get_bits1(gb);
        ptl->sub_layer_level_present_flag[i]   = get_bits1(gb);
    }

    if (max_num_sub_layers - 1 > 0)
        for (i = max_num_sub_layers - 1; i < 8; i++)
            skip_bits(gb, 2); // reserved_zero_2bits[i]
    for (i = 0; i < max_num_sub_layers - 1; i++) {
        if (ptl->sub_layer_profile_present_flag[i] &&
            decode_profile_tier_level(gb, avctx, &ptl->sub_layer_ptl[i]) < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "PTL information for sublayer %i too short\n", i);
            return -1;
        }
        if (ptl->sub_layer_level_present_flag[i]) {
            if (get_bits_left(gb) < 8) {
                av_log(avctx, AV_LOG_ERROR,
                       "Not enough data for sublayer %i level_idc\n", i);
                return -1;
            } else
                ptl->sub_layer_ptl[i].level_idc = get_bits(gb, 8);
        }
    }

    return 0;
}

static void sub_layer_hrd_parameters(GetBitContext *gb, SubLayerHRDParams *sub_layer_hrd_parameters,
                                     int cpb_count, int sub_pic_hrd_params_present_flag ) {
    int i;
    for( i = 0; i  <=  cpb_count; i++ ) {
        sub_layer_hrd_parameters->bit_rate_value_minus1[i] = get_ue_golomb_long(gb);
        sub_layer_hrd_parameters->cpb_size_value_minus1[i] = get_ue_golomb_long(gb);
		if( sub_pic_hrd_params_present_flag ) {
            sub_layer_hrd_parameters->cpb_size_du_value_minus1[i] = get_ue_golomb_long(gb);
            sub_layer_hrd_parameters->bit_rate_du_value_minus1[i] = get_ue_golomb_long(gb);
		}
        sub_layer_hrd_parameters->cbr_flag[i] = get_bits1(gb);
	}
}

static int parse_hrd_parameters(GetBitContext *gb, HRDParameters *hrd_parameters,
                                 int common_inf_present_flag, int max_num_sublayers)
{
    int i;
    if (common_inf_present_flag) {
        hrd_parameters->nal_hrd_parameters_present_flag = get_bits1(gb);
        hrd_parameters->vcl_hrd_parameters_present_flag = get_bits1(gb);
        if (hrd_parameters->nal_hrd_parameters_present_flag  ||
                hrd_parameters->vcl_hrd_parameters_present_flag ) {
            hrd_parameters->sub_pic_hrd_params_present_flag = get_bits1(gb);
            if( hrd_parameters->sub_pic_hrd_params_present_flag ) {
                hrd_parameters->sub_pic_hrd_params.tick_divisor_minus2 = get_bits(gb, 8);
                hrd_parameters->sub_pic_hrd_params.du_cpb_removal_delay_increment_length_minus1 = get_bits(gb, 5);
                hrd_parameters->sub_pic_hrd_params.sub_pic_cpb_params_in_pic_timing_sei_flag = get_bits1(gb);
                hrd_parameters->sub_pic_hrd_params.dpb_output_delay_du_length_minus1 = get_bits(gb, 5);
            }
            hrd_parameters->bit_rate_scale = get_bits(gb, 4);
            hrd_parameters->cpb_size_scale = get_bits(gb, 4);
            if( hrd_parameters->sub_pic_hrd_params_present_flag ) {
                hrd_parameters->cpb_size_du_scale = get_bits(gb, 4);
            }
            hrd_parameters->initial_cpb_removal_delay_length_minus1 = get_bits(gb, 5);
            hrd_parameters->au_cpb_removal_delay_length_minus1 = get_bits(gb, 5);
            hrd_parameters->dpb_output_delay_length_minus1 = get_bits(gb, 5);
        } else //FIXME check conformance may be move this to default value init
            hrd_parameters->initial_cpb_removal_delay_length_minus1 = 23;
    }
    //FIXME check max_num_sub_layers is minus 1 otherwise we might be over reading
    for( i = 0; i  <=  max_num_sublayers; i++ ) {
        hrd_parameters->fixed_pic_rate_general_flag[i] = get_bits1(gb);
        if(!hrd_parameters->fixed_pic_rate_general_flag[i]) {
            hrd_parameters->fixed_pic_rate_within_cvs_flag[i] = get_bits1(gb);
        } else { // FIXME not sure about this
            hrd_parameters->fixed_pic_rate_within_cvs_flag[i] = 1;
        }
        hrd_parameters->low_delay_hrd_flag [i] = 0;
        hrd_parameters->cpb_cnt_minus1     [i] = 0;

        if( hrd_parameters->fixed_pic_rate_within_cvs_flag[i] ) {
            hrd_parameters->elemental_duration_in_tc_minus1[i] = get_ue_golomb_long(gb);
        } else {
            hrd_parameters->low_delay_hrd_flag[i] = get_bits1(gb);
        }

        if(!hrd_parameters->low_delay_hrd_flag[i]) {
            hrd_parameters->cpb_cnt_minus1[i] = get_ue_golomb_long(gb);
            if (hrd_parameters->cpb_cnt_minus1[i] > 31) {
                return AVERROR_INVALIDDATA;
            }
        }
        if( hrd_parameters->nal_hrd_parameters_present_flag )
            sub_layer_hrd_parameters(gb, &hrd_parameters->sub_layer_hrd_params[i], hrd_parameters->cpb_cnt_minus1[i], hrd_parameters->sub_pic_hrd_params_present_flag);

        if( hrd_parameters->vcl_hrd_parameters_present_flag )
            sub_layer_hrd_parameters(gb, &hrd_parameters->sub_layer_hrd_params[i], hrd_parameters->cpb_cnt_minus1[i], hrd_parameters->sub_pic_hrd_params_present_flag);
    }
    return 0; 
}


static void parse_vps_vui_bsp_hrd_params(GetBitContext *gb, AVCodecContext *avctx,
                                         HEVCVPS *vps,
                                         BspHrdParams *bsp_hrd_params,
                                         unsigned int num_output_layer_sets,
                                         unsigned int *num_layers_in_id_list,
                                         unsigned int *max_sub_layers_in_layer_set,
                                         unsigned int *ols_idx_to_ls_idx ) {
    int i, k, h, j, r, t;
    //HEVCVPSExt  *vps_ext = &vps->vps_ext;
    bsp_hrd_params->vps_num_add_hrd_params  = get_ue_golomb_long(gb);
    for( i = vps->vps_num_hrd_parameters; i < vps->vps_num_hrd_parameters + bsp_hrd_params->vps_num_add_hrd_params; i++ ) {
        if( i > 0 )
            bsp_hrd_params->cprms_add_present_flag[i] = get_bits1(gb);
        else {
            if(vps->vps_num_hrd_parameters == 0) {
                bsp_hrd_params->cprms_add_present_flag[0] = 1;
            }
        }
        bsp_hrd_params->num_sub_layer_hrd_minus1[i] = get_ue_golomb_long(gb);
        //TODO check hrd_params
        parse_hrd_parameters(gb, &bsp_hrd_params->HrdParam[i], bsp_hrd_params->cprms_add_present_flag[i], vps->vps_max_sub_layers -1);
    }
    if( (vps->vps_num_hrd_parameters + bsp_hrd_params->vps_num_add_hrd_params) > 0 )
        for( h = 1; h < num_output_layer_sets; h++ ) {
            int ls_idx = ols_idx_to_ls_idx[h];
            bsp_hrd_params->num_signalled_partitioning_schemes[h] = get_ue_golomb_long(gb);
            for( j = 1; j < bsp_hrd_params->num_signalled_partitioning_schemes[h] + 1; j++ ) {
                bsp_hrd_params->num_partitions_in_scheme_minus1[h][j] = get_ue_golomb_long(gb);
                for( k = 0; k  <=  bsp_hrd_params->num_partitions_in_scheme_minus1[h][j]; k++ )
                    for( r = 0; r < num_layers_in_id_list[ ls_idx ]; r++ )
                        bsp_hrd_params->layer_included_in_partition_flag[h][j][k][r] = get_bits1(gb);
            }
            for( i = 0; i < bsp_hrd_params->num_signalled_partitioning_schemes[ h ] + 1; i++ )
                for( t = 0; t < max_sub_layers_in_layer_set[ ls_idx ]; t++ ) {
                    bsp_hrd_params->num_bsp_schedules_minus1[h][i][t] = get_ue_golomb_long(gb);
                    for( j = 0; j  <=  bsp_hrd_params->num_bsp_schedules_minus1[h][i][t]; j++ )
                        for( k = 0; k  <=  bsp_hrd_params->num_partitions_in_scheme_minus1[h][i]; k++ ) {
                            if( vps->vps_num_hrd_parameters + bsp_hrd_params->vps_num_add_hrd_params > 1 ) {
                                //TODO check length
                                int numBits = 1;
                                while ((1 << numBits) < (vps->vps_num_hrd_parameters + bsp_hrd_params->vps_num_add_hrd_params))
                                    numBits++;
                                bsp_hrd_params->bsp_hrd_idx[h][i][t][j][k ] = get_bits(gb, numBits);
                            }
                            bsp_hrd_params->bsp_sched_idx[h][i][t][j][k] = get_ue_golomb_long(gb);
                        }
				}
		}
}



//static int scal_type_to_scal_idx(HEVCVPS *vps, enum ScalabilityType scal_type) {
//    int scal_idx = 0, curScalType;
//    for( curScalType = 0; curScalType < scal_type; curScalType++ )
//        scal_idx += ( vps->vps_ext.scalability_mask[curScalType] ? 1 : 0 );
//    return scal_idx;
//}

//static int get_scalability_id(HEVCVPS * vps, int layer_id_in_vps, enum ScalabilityType scal_type) {
//    int scal_idx = scal_type_to_scal_idx(vps, scal_type);
//    return vps->vps_ext.scalability_mask[scal_type] ? vps->vps_ext.dimension_id[layer_id_in_vps][scal_idx] : 0;
//}

//static int get_view_index(HEVCVPS *vps, int id){

//    return get_scalability_id(vps, vps->vps_ext.layer_id_in_vps[id], VIEW_ORDER_INDEX);
//}

//TODO check if vps_max_layers is correct
//static int get_num_views(HEVCVPS *vps) {
//    int num_views = 1, i;
//    for (i = 0; i < vps->vps_max_layers; i++) {
//        int lId = vps->vps_ext.layer_id_in_nuh[i];
//        if (i > 0 && (get_view_index(vps, lId) != get_scalability_id(vps, i - 1, VIEW_ORDER_INDEX)))
//            num_views++;
//    }
//    return num_views;
//}

static void parse_rep_format(RepFormat *rep_format, GetBitContext *gb) {

    rep_format->pic_width_vps_in_luma_samples         = get_bits_long(gb, 16);
    rep_format->pic_height_vps_in_luma_samples        = get_bits_long(gb, 16);

    rep_format->chroma_and_bit_depth_vps_present_flag = get_bits1(gb);
    if (rep_format->chroma_and_bit_depth_vps_present_flag) {
        rep_format->chroma_format_vps_idc = get_bits(gb, 2);
        if (rep_format->chroma_format_vps_idc == 3) {
            rep_format->separate_colour_plane_vps_flag = get_bits1(gb);
        }
        rep_format->bit_depth_vps[CHANNEL_TYPE_LUMA] =   get_bits(gb, 4) + 8;  
        rep_format->bit_depth_vps[CHANNEL_TYPE_CHROMA] = get_bits(gb, 4) + 8;
    }

    rep_format->conformance_window_vps_flag = get_bits1(gb);
    if( rep_format->conformance_window_vps_flag ) {
        rep_format->conf_win_vps_left_offset   = get_ue_golomb_long(gb);
        rep_format->conf_win_vps_right_offset  = get_ue_golomb_long(gb);
        rep_format->conf_win_vps_top_offset    = get_ue_golomb_long(gb);
        rep_format->conf_win_vps_bottom_offset = get_ue_golomb_long(gb);
    }
}

static void parse_video_signal_info (GetBitContext *gb, VideoSignalInfo *video_signal_info ) {
	video_signal_info->video_vps_format                = get_bits(gb, 3);
	video_signal_info->video_full_range_vps_flag       = get_bits1(gb);
    video_signal_info->color_primaries_vps             = get_bits(gb, 8);
	video_signal_info->transfer_characteristics_vps    = get_bits(gb, 8);
	video_signal_info->matrix_coeffs_vps               = get_bits(gb, 8);
}


static void parse_vps_vui(GetBitContext *gb, AVCodecContext *avctx, HEVCVPS *vps,
                          unsigned int num_layer_sets, unsigned int max_layers,
                          unsigned int num_output_layer_sets,
                          unsigned int *num_layers_in_id_list,
                          unsigned int *max_sub_layers_in_layer_set,
                          unsigned int *num_direct_ref_layers,
                          unsigned int **id_direct_ref_layer,
                          unsigned int *ols_idx_to_ls_idx) {
    int i,j;
    VPSVUIParameters *vps_vui     = &vps->vps_ext.vui_parameters;

    vps_vui->cross_layer_pic_type_aligned_flag  = get_bits1(gb);
    if (!vps_vui->cross_layer_pic_type_aligned_flag) {
        vps_vui->cross_layer_irap_aligned_flag  = get_bits1(gb);
    } else {
        //FIXME default init
        vps_vui->cross_layer_irap_aligned_flag  = 1;
        vps_vui->all_layers_idr_aligned_flag    = get_bits1(gb);
    }
    vps_vui->bit_rate_present_vps_flag = get_bits1(gb);
    vps_vui->pic_rate_present_vps_flag = get_bits1(gb);

    if( vps_vui->bit_rate_present_vps_flag  ||  vps_vui->pic_rate_present_vps_flag ){
        for( i = vps->vps_base_layer_internal_flag ? 0 : 1; i < num_layer_sets; i++ ){
            for( j = 0; j  <  max_sub_layers_in_layer_set[i]; j++ ) {
                if( vps_vui->bit_rate_present_vps_flag ) {
                    vps_vui->bit_rate_present_flag[i][j] = get_bits1(gb);
                }
                if( vps_vui->pic_rate_present_vps_flag ) {
                    vps_vui->pic_rate_present_flag[i][j] = get_bits1(gb);
                }
                if( vps_vui->bit_rate_present_flag[i][j]) {
                    vps_vui->avg_bit_rate[i][j] = get_bits(gb, 16);
                    vps_vui->max_bit_rate[i][j] = get_bits(gb, 16);
                }
                if( vps_vui->pic_rate_present_flag[i][j] ) {
                    vps_vui->constant_pic_rate_idc[i][j] = get_bits(gb, 2);
                    vps_vui->avg_pic_rate[i][j]          = get_bits(gb, 16);
                }
            }
        }
    }

    vps_vui->video_signal_info_idx_present_flag = get_bits1(gb);
	if( vps_vui->video_signal_info_idx_present_flag ) {
		vps_vui->vps_num_video_signal_info_minus1 = get_bits(gb, 4);
    }

    for( i = 0; i  <=  vps_vui->vps_num_video_signal_info_minus1; i++ ){
        parse_video_signal_info(gb, &vps_vui->video_signal_info[i] );
    }

    if( vps_vui->video_signal_info_idx_present_flag  &&  vps_vui->vps_num_video_signal_info_minus1 > 0 ){
        for( i = vps->vps_base_layer_internal_flag ? 0 : 1; i  <  max_layers; i++ ) {
            vps_vui->vps_video_signal_info_idx[i]  = get_bits(gb, 4);
        }
    }

    vps_vui->tiles_not_in_use_flag           = get_bits1(gb);
    if( !vps_vui->tiles_not_in_use_flag ) {
        for( i = vps->vps_base_layer_internal_flag ? 0 : 1; i  <  max_layers; i++ ) {
            vps_vui->tiles_in_use_flag[i]  = get_bits1(gb);
            if( vps_vui->tiles_in_use_flag[i] ) {
                vps_vui->loop_filter_not_across_tiles_flag[i] = get_bits1(gb);
            }
        }
        for( i = vps->vps_base_layer_internal_flag ? 1 : 2; i < max_layers; i++ ){
            for( j = 0; j < num_direct_ref_layers[vps->vps_ext.layer_id_in_nuh[i]]; j++ ) {
                int layerIdx = vps->vps_ext.layer_id_in_vps[id_direct_ref_layer[vps->vps_ext.layer_id_in_nuh[i]][j]];
                if( vps_vui->tiles_in_use_flag[i]  &&  vps_vui->tiles_in_use_flag[layerIdx] ) {
                    vps_vui->tile_boundaries_aligned_flag[i][j] = get_bits1(gb);
                }
            }
        }
    }

	vps_vui->wpp_not_in_use_flag = get_bits1(gb);
    if( !vps_vui->wpp_not_in_use_flag ){
        for( i = vps->vps_base_layer_internal_flag ? 0 : 1; i  < max_layers; i++ ) {
            vps_vui->wpp_in_use_flag[i]   = get_bits1(gb);
        }
    }

    vps_vui->single_layer_for_non_irap_flag = get_bits1(gb);
    vps_vui->higher_layer_irap_skip_flag    = get_bits1(gb);
    vps_vui->ilp_restricted_ref_layers_flag = get_bits1(gb);

    if( vps_vui->ilp_restricted_ref_layers_flag ){
        for( i = 1; i  < max_layers; i++ ){
            for( j = 0; j < num_direct_ref_layers[vps->vps_ext.layer_id_in_nuh[i]]; j++ ){
                if( vps->vps_base_layer_internal_flag ||
                        id_direct_ref_layer[vps->vps_ext.layer_id_in_nuh[i] ][j ] > 0 ) {
                    vps_vui->min_spatial_segment_offset_plus1[i][j] = get_ue_golomb_long(gb);
                    if( vps_vui->min_spatial_segment_offset_plus1[i][j] > 0 ) {
                        vps_vui->ctu_based_offset_enabled_flag[i][j] = get_bits1(gb);
                        if( vps_vui->ctu_based_offset_enabled_flag[i][j] ) {
                            vps_vui->min_horizontal_ctu_offset_plus1[i][j] = get_ue_golomb_long(gb);
                        }
                    }
                }
            }
        }
    }

    vps_vui->vps_vui_bsp_hrd_present_flag = get_bits1(gb);
    if( vps_vui->vps_vui_bsp_hrd_present_flag ){
        parse_vps_vui_bsp_hrd_params(gb, avctx, vps, &vps_vui->bsp_hrd_params,
                                     num_output_layer_sets,
                                     num_layers_in_id_list,
                                     max_sub_layers_in_layer_set,
                                     ols_idx_to_ls_idx);
    }

    for(i = 1; i < max_layers; i++ ){
        if( num_direct_ref_layers[vps->vps_ext.layer_id_in_nuh[i]]  ==  0 ) {
            vps_vui->base_layer_parameter_set_compatibility_flag[i] = get_bits1(gb);
        }
    }
}

static void parse_dpb_size(GetBitContext *gb, HEVCVPS *vps , unsigned int num_output_layer_sets,
                           unsigned int *num_layers_in_id_list, unsigned int **layer_set_layer_id_list,
                           unsigned int *max_sub_layers_in_layer_set,
                           unsigned int *ols_idx_to_ls_idx, unsigned int **necessary_layer_flag) {
    int i, j, k;
    DPBSize *dpb_size = &vps->vps_ext.dpb_size;
    //HEVCVPSExt  *vps_ext = &vps->vps_ext;

    //calculateMaxSLInLayerSets(vps, num_layers_in_id_list, layer_set_layer_id_list);
    // derive max_sub_layers_in_layer_set
    for( i = 1; i < num_output_layer_sets; i++ ) {
        int curr_ls_idx = ols_idx_to_ls_idx[i];
        dpb_size->sub_layer_flag_info_present_flag[i] = get_bits1(gb);
        for( j = 0; j  <  max_sub_layers_in_layer_set[curr_ls_idx]; j++ ) {
            if( j > 0  &&  dpb_size->sub_layer_flag_info_present_flag[i] ) {
                dpb_size->sub_layer_dpb_info_present_flag[i][j] = get_bits1(gb);
            } else { //default init ??
                if( !j )
                    dpb_size->sub_layer_dpb_info_present_flag[i][j] = 1;
                else
                    dpb_size->sub_layer_dpb_info_present_flag[i][j] = 0;
            }
            if( dpb_size->sub_layer_dpb_info_present_flag[i][j] ) {
                for( k = 0; k < num_layers_in_id_list[curr_ls_idx]; k++ ){
                    if( necessary_layer_flag[i][k]  &&  ( vps->vps_base_layer_internal_flag  ||
                                                          ( layer_set_layer_id_list[curr_ls_idx][k] ))) {
                        dpb_size->max_vps_dec_pic_buffering_minus1[i][k][j] = get_ue_golomb_long(gb);
                    }
                }
                dpb_size->max_vps_num_reorder_pics[i][j]       = get_ue_golomb_long(gb);
                dpb_size->max_vps_latency_increase_plus1[i][j] = get_ue_golomb_long(gb);
            }
		}
	}
}


#define MAX_VPS_NUM_SCALABILITY_TYPES 16

static int parse_vps_extension (GetBitContext *gb, AVCodecContext *avctx, HEVCVPS *vps)  {
    int i, j;
    int ret = 0;

    HEVCVPSExt *vps_ext = &vps->vps_ext;

    //TODO move those into HEVCMultiLayerContext
    unsigned int max_layers                  ; ///< MaxLayersMinus1 + 1
    unsigned int num_scalability_types       ; ///< NumScalabilityTypes
    unsigned int num_views                   ; ///< NumViews
    unsigned int num_layer_sets              ; ///< NumLayerSets
    unsigned int num_output_layer_sets       ; ///< NumOutputLayerSets
    unsigned int num_independant_layers      ; ///< NumIndependantLayers
    unsigned int default_output_layer_idc    ; ///< DefaultOutputLayerIdc
    //TODO max_layer actually derive from vps parsing

    unsigned int scalabilty_id[64][16]= {0};
    unsigned int depth_layer_flag[64] = {0};
    unsigned int view_order_idx  [64] = {0};
    unsigned int dependency_id   [64] = {0};
    unsigned int aux_id          [64] = {0};


    unsigned int num_direct_ref_layers       [64] = {0}; ///< NumDirectRefLayers[]
    unsigned int num_layers_in_tree_partition[64] = {0}; ///< NumLayersInTreePartition[]
    unsigned int num_ref_layers              [64] = {0}; ///< NumRefLayers[],
    unsigned int num_predicted_layers        [64] = {0}; ///< NumPredictedLayers[]
    unsigned int max_sub_layers_in_layer_set [64] = {0}; ///< MaxSubLayersInLayerSet
    unsigned int layer_id_in_list_flag       [64] = {0}; ///< LayerIdInListFlag[]

    //TODO consider dynamic allocation for those tables
    unsigned int num_necessary_layers                [2112] = {0}; ///< NumNecessaryLayers
    unsigned int num_layers_in_id_list               [2112] = {0}; ///< NumLayersInIdList
    unsigned int num_output_layer_in_output_layer_set[2112] = {0}; ///< NumOutputLayerInOutputLis
    unsigned int ols_idx_to_ls_idx                   [2112] = {0}; ///< OlsIdxToLsIdx[]
    unsigned int ols_highest_output_layer_id         [2112] = {0}; ///< OlsHighestOutputLayerIdt

    //TODO check dynamic allocation and free
    unsigned int **tree_partition_layer_id_list = NULL; ///< TreePartitionLayerIdList[][]
    unsigned int **dependency_flag              = NULL; ///< DependencyFlag[][]
    unsigned int **id_direct_ref_layer          = NULL; ///< IdDirectRefLayer[][]
    unsigned int **id_ref_layer                 = NULL; ///< IdRefLayer[][],
    unsigned int **id_predicted_layer           = NULL; ///< IdPredictedLayer[][]
    unsigned int **layer_set_layer_id_list      = NULL; ///< LayerSetLayerIdList[][]
    unsigned int **necessary_layer_flag         = NULL; ///< NecessaryLayerFlag[num_output_layer_sets][]
    //Inferred parameters, not really when parsing if not used by parsing op
    unsigned int dim_bit_offset[16] = {0};
    int max_nuh_l_id = 0;
    int ls_idx = ols_idx_to_ls_idx[ 0 ];//0
    int ls_layer_idx;
    int ols_idx;
    int vps_num_rep_formats_minus1;

    max_layers               = FFMIN(63, vps->vps_max_layers);
    num_layer_sets           = vps->vps_num_layer_sets;

    if( vps->vps_max_layers > 1  &&  vps->vps_base_layer_internal_flag )
        parse_ptl(gb, avctx, &vps_ext->ptl[0], vps->vps_max_sub_layers, 0);

    vps_ext->splitting_flag = get_bits1(gb);

    for (i = 0, num_scalability_types = 0; i < MAX_VPS_NUM_SCALABILITY_TYPES; i++) {
        vps_ext->scalability_mask_flag[i] = get_bits1(gb);
        num_scalability_types += vps_ext->scalability_mask_flag[i];
    }

    for (j = 0; j < num_scalability_types - vps_ext->splitting_flag; j++) {
        vps_ext->dimension_id_len[j] = get_bits(gb, 3) + 1;
    }


    if(vps_ext->splitting_flag) {
        for(j = 1; j < num_scalability_types; j++){
            int dim_idx;
            dim_bit_offset[j] = 0;
            for (dim_idx = 0; dim_idx < j; dim_idx++){
                dim_bit_offset[j] += vps_ext->dimension_id_len[dim_idx];
            }
        }
        vps_ext->dimension_id_len[num_scalability_types - 1] =  6 - dim_bit_offset[num_scalability_types - 1];
        vps_ext->dimension_id_len[num_scalability_types] = 6;
    }

    vps_ext->vps_nuh_layer_id_present_flag = get_bits1(gb);

    //TODO default init ???
    vps_ext->layer_id_in_nuh[0] = 0;
    vps_ext->layer_id_in_vps[0] = 0;

    for (i = 1; i < max_layers; i++) {
        if (vps_ext->vps_nuh_layer_id_present_flag) {
            vps_ext->layer_id_in_nuh[i] = get_bits(gb, 6);
            if(vps_ext->layer_id_in_nuh[i] <= vps_ext->layer_id_in_nuh[i - 1]){
                av_log(avctx,AV_LOG_ERROR,"(vps_extensions) layer_id_in_nuh[i] smaller than layer_id_in_nuh[i-1]\n");
                ret = AVERROR_INVALIDDATA;
                goto fail_vps_ext;
            }
        } else {
            vps_ext->layer_id_in_nuh[i] = i;
        }

        //derive layer id in vps
        vps_ext->layer_id_in_vps[vps_ext->layer_id_in_nuh[i]] = i;

        if (!vps_ext->splitting_flag){
            for (j = 0; j < num_scalability_types; j++) {
                vps_ext->dimension_id[i][j] = get_bits(gb, vps_ext->dimension_id_len[j]);
            }
        } else { //not sure about default values
            for (j = 0; j < num_scalability_types; j++) {
                vps_ext->dimension_id[i][j] = ((vps_ext->layer_id_in_nuh[i] & ((1 << dim_bit_offset[j+1]) - 1)) >> dim_bit_offset[j]);
            }
        }
    }

    num_views = 1;
    for( i = 0; i < max_layers; i++ ) {
        int l_id = vps_ext->layer_id_in_nuh[i];
        int sm_idx;
        for( sm_idx = 0, j = 0; sm_idx < 16; sm_idx++ ) {
            if( vps_ext->scalability_mask_flag[sm_idx] )
                scalabilty_id[i][sm_idx] = vps_ext->dimension_id[i][j++];
            else
                scalabilty_id[i][sm_idx] = 0;
        }
        depth_layer_flag[l_id] = scalabilty_id[i][0];
        view_order_idx  [l_id] = scalabilty_id[i][1];
        dependency_id   [l_id] = scalabilty_id[i][2];
        aux_id          [l_id] = scalabilty_id[i][3];
        //if(aux_id[l_id] > 2)
        //    fprintf(stderr,"SUPERIOR 2 AUX_ID %d\n", aux_id[l_id] );
        //TODO aux_id[l_id] shall be in the range of 0 to 2, inclusive, or 128 to 159, inclusive
        if( i > 0 ) {
            int new_view_flag = 1;
            for( j = 0; j < i; j++ )
                if( view_order_idx[l_id] == view_order_idx[vps_ext->layer_id_in_nuh[j]] ){
                    new_view_flag = 0;
                }
            num_views += new_view_flag;
        }
    }

    vps_ext->view_id_len = get_bits(gb, 4);
    if (vps_ext->view_id_len){
        for (i = 0; i < num_views; i++) {
            vps_ext->view_id_val[i] = get_bits(gb, vps_ext->view_id_len);
        }
        //TODO derive ViewId[nuhLayerId] here
    }

    //TODO Not here move to multiLayerCtx
//    vps_ext->num_direct_ref_layers[0] = 0;

    for (i = 1; i < max_layers; i++) {
        for (j = 0; j < i; j++) {
            vps_ext->direct_dependency_flag[i][j] = get_bits1(gb);
        }
    }
    dependency_flag = av_malloc(max_layers * sizeof(unsigned int*));

    if(!dependency_flag)
        return AVERROR(ENOMEM);

    for(i = 0; i < max_layers; i++){
        dependency_flag[i] = av_mallocz(max_layers * sizeof(unsigned int));
        if(!dependency_flag[i])
            return AVERROR(ENOMEM);
    }

    for( i = 0; i < max_layers; i++ ){
        for( j = 0; j < max_layers; j++ ) {
            int k;
            dependency_flag[i][j] = vps_ext->direct_dependency_flag[i][j];
            for(k = 0; k < i; k++){
                if( vps_ext->direct_dependency_flag[i][k] && dependency_flag[k][j] ){
                    dependency_flag[i][j] = 1;
                }
            }
        }
    }
    /*
    The variables NumDirectRefLayers[ iNuhLId ], IdDirectRefLayer[ iNuhLId ][ d ], NumRefLayers[ iNuhLId ],
    IdRefLayer[ iNuhLId ][ r ], NumPredictedLayers[ iNuhLId ] and IdPredictedLayer[ iNuhLId ][ p ] are derived as follows:
    */
    id_direct_ref_layer = av_malloc(64 * sizeof(unsigned int*)); //max_layer ID in nuh
    id_ref_layer        = av_malloc(64 * sizeof(unsigned int*)); //max_layer ID in nuh
    id_predicted_layer  = av_malloc(64 * sizeof(unsigned int*)); //max_layer ID in nuh

    if(!id_direct_ref_layer || !id_ref_layer || !id_predicted_layer)
        return AVERROR(ENOMEM);

    for( i = 0; i < 64; i++ ) {
        id_direct_ref_layer[i] = av_mallocz(max_layers * sizeof(unsigned int));
        id_ref_layer[i]        = av_mallocz(max_layers * sizeof(unsigned int));
        id_predicted_layer[i]  = av_mallocz(max_layers * sizeof(unsigned int));
        if(!id_direct_ref_layer[i] || !id_ref_layer[i] || !id_predicted_layer[i])
            return AVERROR(ENOMEM);
    }

    for( i = 0; i < max_layers; i++ ) {
        int i_nuh_l_id = vps_ext->layer_id_in_nuh[i];
        int d, r, p;
        for( j = 0, d = 0, r = 0, p = 0; j < max_layers; j++ ) {
            int j_nuh_l_id = vps_ext->layer_id_in_nuh[j];
            if( vps_ext->direct_dependency_flag[i][j] )
                id_direct_ref_layer[ i_nuh_l_id ][ d++ ] = j_nuh_l_id;
            if( dependency_flag[i][j] )
                id_ref_layer[ i_nuh_l_id ][ r++ ]        = j_nuh_l_id;
            if( dependency_flag[j][i] )
                id_predicted_layer[ i_nuh_l_id ][ p++ ]  = j_nuh_l_id;
        }
        vps_ext->num_direct_ref_layers[ i_nuh_l_id ] = num_direct_ref_layers[ i_nuh_l_id ] = d;
        // Unused
        //num_ref_layers[ i_nuh_l_id ]        = r;
        num_predicted_layers[ i_nuh_l_id ]  = p;
    }
    /*
    (F-5)
    The variables NumIndependentLayers, NumLayersInTreePartition[ i ] and TreePartitionLayerIdList[ i ][ j ] for i in the
    range of 0 to NumIndependentLayers − 1, inclusive, and j in the range of 0 to NumLayersInTreePartition[ i ] − 1, inclusive,
    are derived as follows:*/
    tree_partition_layer_id_list = av_malloc(max_layers * sizeof(unsigned int*));

    if(!tree_partition_layer_id_list)
        return AVERROR(ENOMEM);

    for( i = 0; i < max_layers; i++ ){
        tree_partition_layer_id_list[i] = av_mallocz(max_layers * sizeof(unsigned int)); //FFMAX(num_predicted_layers[nuhlid])

        if(!tree_partition_layer_id_list[i])
            return AVERROR(ENOMEM);
    }


    {
        int k;
        for( i = 0, k = 0; i < max_layers; i++ ) {
            int i_nuh_l_id = vps_ext->layer_id_in_nuh[i];
            //find max nuh layer id here for output layer flag
            if(i_nuh_l_id > max_nuh_l_id)
                max_nuh_l_id = i_nuh_l_id;
            if( num_direct_ref_layers[ i_nuh_l_id ] == 0 ) {
                int h;
                tree_partition_layer_id_list[k][0] = i_nuh_l_id;
                for( j = 0, h = 1; j < num_predicted_layers[ i_nuh_l_id ]; j++ ) {
                    int pred_l_id = id_predicted_layer[ i_nuh_l_id ][j];
                    if( !layer_id_in_list_flag[ pred_l_id ] ) {
                        tree_partition_layer_id_list[k][ h++ ] = pred_l_id;
                        layer_id_in_list_flag[ pred_l_id ] = 1;
                    }
                }
                num_layers_in_tree_partition[ k++ ] = h;
            }
        }
        num_independant_layers = k;
    }

    if( num_independant_layers > 1 ) {
        vps_ext->num_add_layer_sets = get_ue_golomb_long(gb);
        if(vps_ext->num_add_layer_sets > 1023){
            av_log(avctx, AV_LOG_ERROR, "(vps_extension) num_add_layer_sets greater than 1023 (%d)\n",vps_ext->num_add_layer_sets);
            ret = AVERROR_INVALIDDATA;
            goto fail_vps_ext;
        } else if(!vps_ext->num_add_layer_sets && !vps->vps_base_layer_available_flag){
            av_log(avctx, AV_LOG_ERROR, "(vps_extension) num_add_layer_sets and vps_base_layer_available_flag both equal to 0 \n");
            ret = AVERROR_INVALIDDATA;
            goto fail_vps_ext;
        }
    } else {// should already be 0
        vps_ext->num_add_layer_sets = 0;
    }

    num_layer_sets = vps->vps_num_layer_sets + vps_ext->num_add_layer_sets;
    //TODO derive FirstAddLayerSetIdx = vps_num_layer_sets_minus1 + 1
    //LastAddLayerSetIdx = FirstAddLayerSetIdx + num_add_layer_sets − 1
    layer_set_layer_id_list    = av_malloc(num_layer_sets * sizeof(unsigned int*));

    if(!layer_set_layer_id_list)
        return AVERROR(ENOMEM);

    for(i = 0; i < num_layer_sets; i++){
        layer_set_layer_id_list[i]    = av_mallocz(64 * sizeof(unsigned int));
        if(!layer_set_layer_id_list[i])
            return AVERROR(ENOMEM);
    }

    num_layers_in_id_list[0] = 1;
    for(i = 1; i < vps->vps_num_layer_sets; i++ ) {
        //TODO check if this derivation could be done before
        int n = 0;
        int m;
        for( m = 0; m <= vps->vps_max_layer_id; m++ ){
            if( vps->layer_id_included_flag[ i ][ m ] ){
                layer_set_layer_id_list[i][n++] = m;
                vps_ext->ref_layer_id[i][n-1] = m;
            }
        }
        num_layers_in_id_list[i] = n;
    }

    for( i = 0; i < vps_ext->num_add_layer_sets; i++ ){
//        int layer_num = 0;
//        int tree_idx;
//        int ls_idx = vps->vps_num_layer_sets + i;
        for( j = 1; j < num_independant_layers; j++ ) {
            //TODO check length Ceil( Log2( NumLayersInTreePartition[ j ] + 1 )
            int len = 1;
            while ((1 << len) < (num_layers_in_tree_partition[j] + 1))
                len++;
            vps_ext->highest_layer_idx[i][j] = get_bits(gb, len) - 1;  // TODO: compute len
            if(vps_ext->highest_layer_idx[i][j] > num_layers_in_tree_partition[j]){
                av_log(avctx, AV_LOG_ERROR,"(vps_extension) num_layers_in_tree_partition[j] (%d) greater than highest_layer_idx[i][j](%d) (i:%d j:%d)\n",
                       num_layers_in_tree_partition[j],vps_ext->highest_layer_idx[i][j],i,j);
                ret = AVERROR_INVALIDDATA;
                goto fail_vps_ext;
            }
        }
//        for( tree_idx = 1; tree_idx < num_independant_layers; tree_idx++ ){
//            int layer_cnt;
//            for( layer_cnt = 0; layer_cnt <= vps_ext->highest_layer_idx[i][ tree_idx ]; layer_cnt++ ){
//                layer_set_layer_id_list[ ls_idx ][ layer_num++ ] = tree_partition_layer_id_list[ tree_idx ][ layer_cnt ];
//            }
//            num_layers_in_id_list[ ls_idx ] = layer_num;
//            //TODO NumLayersInIdList[ vps_num_layer_sets_minus1 + 1 + i ] shall be
//            //greater than 0.
//        }
    }

    //TODO check if it can be included into previous loop
    for( i = 0; i < vps_ext->num_add_layer_sets; i++ ){
        int layer_num = 0;
        int tree_idx;
        int ls_idx = vps->vps_num_layer_sets + i;
        for( tree_idx = 1; tree_idx < num_independant_layers; tree_idx++ ){
            int layer_cnt;
            for( layer_cnt = 0; layer_cnt <= vps_ext->highest_layer_idx[i][ tree_idx ]; layer_cnt++ ){
                layer_set_layer_id_list[ ls_idx ][ layer_num++ ] = tree_partition_layer_id_list[ tree_idx ][ layer_cnt ];
            }
        }
        num_layers_in_id_list[ ls_idx ] = layer_num;
        if(!num_layers_in_id_list[ls_idx]){
            av_log(avctx, AV_LOG_ERROR, "(vps_extension) num_layers_in_id_list[ vps_num_layer_sets_minus1 + 1 + i ] greater than 0 (%d)\n",num_layers_in_id_list[ls_idx]);
            ret = AVERROR_INVALIDDATA;
            goto fail_vps_ext;
        }
    }


    vps_ext->vps_sub_layers_max_minus1_present_flag = get_bits1(gb);

    if( vps_ext->vps_sub_layers_max_minus1_present_flag ){
        for( i = 0; i  <  max_layers ; i++ ) {
            vps_ext->sub_layers_vps_max_minus1[i] = get_bits(gb, 3);
        }
    } else {
        for( i = 0; i  <  max_layers ; i++ ){
            vps_ext->sub_layers_vps_max_minus1[i] = vps->vps_max_sub_layers - 1;
        }
    }

    //derive max_sub_layers_in_layer_set here
    for( i = 0; i < num_layer_sets; i++ ) {
        int max_sl_minus1 = 0;
        int k;
        for( k = 0; k < num_layers_in_id_list[i]; k++ ) {
            int l_id = layer_set_layer_id_list[i][k];
            max_sl_minus1 = FFMAX( max_sl_minus1, vps_ext->sub_layers_vps_max_minus1[vps_ext->layer_id_in_vps[l_id]] );
        }
        max_sub_layers_in_layer_set[i] = max_sl_minus1 + 1;
    }

    vps_ext->max_tid_ref_present_flag = get_bits1(gb);
    if( vps_ext->max_tid_ref_present_flag ) {
        for( i = 0; i < max_layers - 1; i++ ){
            for( j = i + 1; j  <  max_layers; j++ ){
                if( vps_ext->direct_dependency_flag[j][i] ) {
                    vps_ext->max_tid_il_ref_pics_plus1[i][j] = get_bits(gb, 3);
                } else { // not sure about this
                    vps_ext->max_tid_il_ref_pics_plus1[i][j] = 7;
                }
            }
        }
    } else {
        for( i = 0; i < max_layers-1; i++ ){
            for( j = i + 1; j  <  max_layers; j++ ){
                vps_ext->max_tid_il_ref_pics_plus1[i][j] = 7;
            }
        }
    }

    vps_ext->default_ref_layers_active_flag = get_bits1(gb);
    vps_ext->vps_num_profile_tier_level_minus1 = get_ue_golomb_long(gb);
    if(vps_ext->vps_num_profile_tier_level_minus1 > 63){
        av_log(avctx, AV_LOG_ERROR, "(vps_extension) vps_num_profile_tier_level_minus1 greater than 63 (%d)\n",
               vps_ext->vps_num_profile_tier_level_minus1);
        ret = AVERROR_INVALIDDATA;
        goto fail_vps_ext;
    }

    for( i = vps->vps_base_layer_internal_flag ? 2 : 1;
         i  <=  vps_ext->vps_num_profile_tier_level_minus1; i++ ) {
        //  TO Do  Copy profile from previous one ??
        vps_ext->vps_profile_present_flag[i] = get_bits1(gb);
        //TODO check the ptl info are placed correctly (0) might not be correct + how to infer
        parse_ptl(gb, avctx, &vps_ext->ptl[0], vps->vps_max_sub_layers, vps_ext->vps_profile_present_flag[i]);
    }

    if( num_layer_sets > 1 ) {
        vps_ext->num_add_olss              = get_ue_golomb_long(gb);
        vps_ext->default_output_layer_idc  = get_bits(gb, 2);
        //TODO check num_add_olss boundary
    } else { //should already be 0
        vps_ext->num_add_olss = 0;
    }
    default_output_layer_idc = FFMIN(2, vps_ext->default_output_layer_idc);

    //TODO find bounds of num_output layer sets
    num_output_layer_sets = vps_ext->num_add_olss + num_layer_sets;

    //Check if inferred init is correct
     necessary_layer_flag = av_malloc(num_output_layer_sets * sizeof(unsigned int*));

     if(!necessary_layer_flag)
         return AVERROR(ENOMEM);

     for( i = 0; i < num_output_layer_sets; i++){
          necessary_layer_flag[i] = av_mallocz(max_layers * sizeof(unsigned int));

          if(!necessary_layer_flag[i])
              return AVERROR(ENOMEM);
     }


    vps_ext->output_layer_flag[0][0]=1;
    //int ols_idx = 0;
    //for( ols_idx = 0; ols_idx < num_output_layer_sets; ols_idx++ ) {
    //INIT necessary_layer_flag 0 TODO function ??

    for( ls_layer_idx = 0; ls_layer_idx < num_layers_in_id_list[ ls_idx ]; ls_layer_idx++ ){
        //necessary_layer_flag[0][ ls_layer_idx ] = 0;
        if( vps_ext->output_layer_flag[0][ ls_layer_idx ] ) {
            int curr_layer_id = layer_set_layer_id_list[ ls_idx ][ ls_layer_idx ];
            int r_ls_layer_idx;
            necessary_layer_flag[0][ ls_layer_idx ] = 1;

            for( r_ls_layer_idx = 0; r_ls_layer_idx < ls_layer_idx; r_ls_layer_idx++ ) {
                int ref_layer_id = layer_set_layer_id_list[ ls_idx ][ r_ls_layer_idx ];
                if( dependency_flag[vps_ext->layer_id_in_vps[vps_ext->layer_id_in_nuh[curr_layer_id]]][ vps_ext->layer_id_in_vps[vps_ext->layer_id_in_nuh[ref_layer_id]] ] )
                    necessary_layer_flag[0][ r_ls_layer_idx ] = 1;
            }
        }
    }
    //num_necessary_layers[0] = 0;
    for( ls_layer_idx = 0; ls_layer_idx < num_layers_in_id_list[ ls_idx ]; ls_layer_idx++ ){
        num_necessary_layers[0] += necessary_layer_flag[0][ ls_layer_idx ];
    }
    //}

    for(i = 1; i < num_output_layer_sets; i++ ) {
        if( num_layer_sets > 2 && i >= num_layer_sets ) {
            //FIXME check num bits computation
            //Ceil( Log2( NumLayerSets − 1 ) )
            int numBits = 1;
            while ((1 << numBits) < (num_layer_sets - 1))
                numBits++;
            vps_ext->layer_set_idx_for_ols[i] = get_bits(gb, numBits) + 1;
            //TODO layer_set_idx_for_ols_minus1[ i ] shall be in the range of 0 to NumLayerSets − 2
        } else {
            vps_ext->layer_set_idx_for_ols[i] = 1; //changed from i to 1 (minus1 assumed to be 0)
        }
//        if(vps_ext->layer_set_idx_for_ols[i] > num_layer_sets - 2)
//            fprintf(stderr,"err\n");

        ols_idx_to_ls_idx[i] = (i < num_layer_sets) ? i : vps_ext->layer_set_idx_for_ols[i];
        if( i > (vps->vps_num_layer_sets - 1)  ||  default_output_layer_idc  ==  2 ) {
            for( j = 0; j < num_layers_in_id_list[ols_idx_to_ls_idx[i]]; j++ ) {
                vps_ext->output_layer_flag[i][j] = get_bits1(gb);
            }
        } else { //FIXME default init & OutputLayerFlag??
            if(default_output_layer_idc < 2 && i < vps->vps_num_layer_sets ) { //0 or 1
                for( j = 0; j < num_layers_in_id_list[ols_idx_to_ls_idx[i]]; j++ ){
                    if(default_output_layer_idc == 0 || layer_set_layer_id_list[ols_idx_to_ls_idx[i]][j] == max_nuh_l_id){
                        vps_ext->output_layer_flag[i][j] = 1;//(j == (num_layers_in_id_list[ols_idx_to_ls_idx[i]] - 1) && default_output_layer_idc == 0);
                        //fprintf(stderr,"set output flag i,j %d  %d val:%d\n", i,j, vps_ext->output_layer_flag[i][j]);
                    }
                 }
            } /*else if(default_output_layer_idc == 0  )//not sure
                    for( j = 0; j < num_layers_in_id_list[ols_idx_to_ls_idx[i]]; j++ ){
                        vps_ext->output_layer_flag[i][j] = 1;
                        fprintf(stderr,"default set output flag i,j %d  %d val %d\n", i,j,vps_ext->output_layer_flag[i][j]);
                    }*/
        }

        //derive necessary layers
        //FIXME be carefull with output layer flag vs OutputLayerFlag;
        for( j = 0; j < num_layers_in_id_list[ ols_idx_to_ls_idx[i] ]; j++ ) {
            num_output_layer_in_output_layer_set[i] += vps_ext->output_layer_flag[i][j];
            if( vps_ext->output_layer_flag[i][j] )
                ols_highest_output_layer_id[i] = layer_set_layer_id_list[ols_idx_to_ls_idx[i]][j];
        }

        if(!num_output_layer_in_output_layer_set[i]){
            av_log(avctx, AV_LOG_ERROR, "num_output_layer_in_output_layer_set[i] equals to 0 (i: %d)\n",i);
            ret = AVERROR_INVALIDDATA;
            goto fail_vps_ext;
        }

        //derive necessary layers
//{

        for( ols_idx = 0; ols_idx < num_output_layer_sets; ols_idx++ ) {
            int ls_idx = ols_idx_to_ls_idx[ ols_idx ];
            int ls_layer_idx;

            //FIXME is reset required
//            for( ls_layer_idx = 0; ls_layer_idx < num_layers_in_id_list[ ls_idx ]; ls_layer_idx++ )
//                necessary_layer_flag[ ols_idx ][ ls_layer_idx ] = 0;
            for( ls_layer_idx = 0; ls_layer_idx < num_layers_in_id_list[ ls_idx ]; ls_layer_idx++ ){
                if( vps_ext->output_layer_flag[ ols_idx ][ ls_layer_idx ] ) {
                    int curr_layer_id = layer_set_layer_id_list[ ls_idx ][ ls_layer_idx ];
                    int r_ls_layer_idx;
                    necessary_layer_flag[ ols_idx ][ ls_layer_idx ] = 1;
                    for( r_ls_layer_idx = 0; r_ls_layer_idx < ls_layer_idx; r_ls_layer_idx++ ) {
                        int ref_layer_id = layer_set_layer_id_list[ ls_idx ][ r_ls_layer_idx ];
                        if( dependency_flag[vps_ext->layer_id_in_vps[curr_layer_id]][ vps_ext->layer_id_in_vps[ref_layer_id]] )
                            necessary_layer_flag[ ols_idx ][ r_ls_layer_idx ] = 1;
                    }
                }
            }// init required ???
            num_necessary_layers[ols_idx]=0;
            for( ls_layer_idx = 0; ls_layer_idx < num_layers_in_id_list[ ls_idx ]; ls_layer_idx++ ){
                num_necessary_layers[ ols_idx ] += necessary_layer_flag[ ols_idx ][ ls_layer_idx ];
            }
        }
        for( j = 0; j < num_layers_in_id_list[ ols_idx_to_ls_idx[i] ]; j++ ){
            if( necessary_layer_flag[i][j]  &&  vps_ext->vps_num_profile_tier_level_minus1 > 0 ) {
                int numBits = 1;
                while ( (1 << numBits) < (vps_ext->vps_num_profile_tier_level_minus1+1) ){
                    numBits++;
                }
                vps_ext->profile_tier_level_idx[i][j] = get_bits(gb, numBits);
            } else if(vps->vps_base_layer_internal_flag == 1 && vps_ext->vps_num_profile_tier_level_minus1 > 0) {//TODO default value
                vps_ext->profile_tier_level_idx[i][j] = 1;
            }
            //TODO profile_tier_level_idx[ i ][ j ] shall be in the range of
            //( vps_base_layer_internal_flag ? 0 : 1 )to vps_num_profile_tier_level_minus1, inclusive.
        }
        //derive num_output_layer_in_output_layer_set before
        if( (num_output_layer_in_output_layer_set[i] == 1) &&
                num_direct_ref_layers[ ols_highest_output_layer_id[i] ] > 0 ) {
            vps_ext->alt_output_layer_flag[i] = get_bits1(gb);
        }
    }

    //TODO olsBitstream etc.
    vps_num_rep_formats_minus1 = get_ue_golomb_long(gb);
    if(vps_num_rep_formats_minus1 > 255){
        av_log(avctx,AV_LOG_ERROR, "(vps_extensions) vps_num_rep_formats_minus1 greater than 255  (%d)\n",
               vps_num_rep_formats_minus1);
        ret = AVERROR_INVALIDDATA;
        goto fail_vps_ext;
    }
    vps_ext->vps_num_rep_formats_minus1 = vps_num_rep_formats_minus1;

    for( i = 0; i  <=  vps_ext->vps_num_rep_formats_minus1; i++ ){
        if(i > 0) {
            vps_ext->rep_format[i] = vps_ext->rep_format[i-1];
        }
        parse_rep_format(&vps_ext->rep_format[i], gb);
    }

    if( vps_ext->vps_num_rep_formats_minus1 > 0 ) {
        vps_ext->rep_format_idx_present_flag = get_bits1(gb);
    } else // should already be 0
        vps_ext->rep_format_idx_present_flag = 0;

    if( vps_ext->rep_format_idx_present_flag ) {
        //todo check length
        int numBits = 1;
        while ((1 << numBits) < (vps_ext->vps_num_rep_formats_minus1+1))
            numBits++;
        for( i = vps->vps_base_layer_internal_flag ? 1 : 0; i  <  max_layers; i++ ) {
            vps_ext->vps_rep_format_idx[i] = get_bits(gb, numBits);
            if(vps_num_rep_formats_minus1 > 255){
                av_log(avctx,AV_LOG_ERROR, "(vps_extensions) vps_rep_format_idx[i] (%d) greater than vps_num_rep_formats_minus1 (%d) (i:%d)\n",
                       vps_ext->vps_rep_format_idx[i], vps_num_rep_formats_minus1, i);
                ret = AVERROR_INVALIDDATA;
                goto fail_vps_ext;
            }
        }
    } else {
        for( i = vps->vps_base_layer_internal_flag ? 1 : 0; i  <  max_layers; i++ ) {
            vps_ext->vps_rep_format_idx[i] = FFMIN(i,vps_ext->vps_num_rep_formats_minus1);
        }
    }
    
    vps_ext->max_one_active_ref_layer_flag = get_bits1(gb);
    vps_ext->vps_poc_lsb_aligned_flag = get_bits1(gb);
    /*Additionally, the value of vps_poc_lsb_aligned_flag
affects the decoding process for picture order count in clause F.8.3.1. When not present, the value of
vps_poc_lsb_aligned_flag is inferred to be equal to 0.*/
    for( i = 1; i  <  max_layers; i++ ){
        if( num_direct_ref_layers[vps_ext->layer_id_in_nuh[i]] == 0 ) {
            vps_ext->poc_lsb_not_present_flag[i] = get_bits1(gb);
        }
    }

    parse_dpb_size(gb, vps,num_output_layer_sets, num_layers_in_id_list,
                   layer_set_layer_id_list, max_sub_layers_in_layer_set,
                   ols_idx_to_ls_idx,necessary_layer_flag);

    vps_ext->direct_dep_type_len_minus2        = get_ue_golomb_long(gb);
    //direct_dep_type_len_minus2 in the range of 0 to 30, inclusive but shall be equal 0 or 1
    //in current version
    if(vps_ext->direct_dep_type_len_minus2 > 1){
        av_log(avctx,AV_LOG_ERROR, "(vps_extensions) direct_dep_type_len_minus2 (%d) greater than 1\n",
               vps_ext->direct_dep_type_len_minus2);
        ret = AVERROR_INVALIDDATA;
        goto fail_vps_ext;
    }

    vps_ext->direct_dependency_all_layers_flag = get_bits1(gb);
    if (vps_ext->direct_dependency_all_layers_flag){
        vps_ext->direct_dependency_all_layers_type = get_bits(gb, vps_ext->direct_dep_type_len_minus2+2);
        //TODO in the range of 0 to 2 32 − 2, inclusive, but 0 to 6 in current version,
        if(vps_ext->direct_dependency_all_layers_type > 6){
            av_log(avctx,AV_LOG_ERROR, "(vps_extensions) direct_dependency_all_layers_type (%d) greater than 2\n",
                   vps_ext->direct_dep_type_len_minus2);
            ret = AVERROR_INVALIDDATA;
            goto fail_vps_ext;
        }
    } else { // check default
        for( i = vps->vps_base_layer_internal_flag ? 1 : 2; i < max_layers; i++ ){
            for( j = vps->vps_base_layer_internal_flag ? 0 : 1; j < i; j++ ){
                if( vps_ext->direct_dependency_flag[i][j] ) {
                    vps_ext->direct_dependency_type[i][j] = get_bits(gb, vps_ext->direct_dep_type_len_minus2+2);
                    //TODO check bounds according to profiles annexes
                } else { //FIXME find out if default value is correct
                    vps_ext->direct_dependency_type[i][j] = 0;/*vps_ext->default_direct_dependency_type;*/
                }
            }
        }
    }

    vps_ext->vps_non_vui_extension_length = get_ue_golomb_long(gb);

    for( i = 1; i  <=  vps_ext->vps_non_vui_extension_length; i++ ){
        vps_ext->vps_non_vui_extension_data_byte = get_bits(gb, 8);
    }

    vps_ext->vps_vui_present_flag = get_bits1(gb);

    if( vps_ext->vps_vui_present_flag ) {
        align_get_bits(gb);
        parse_vps_vui(gb, avctx, vps, num_layer_sets, max_layers,
                      num_output_layer_sets, num_layers_in_id_list,
                      max_sub_layers_in_layer_set, num_direct_ref_layers,
                      id_direct_ref_layer, ols_idx_to_ls_idx );
    }

fail_vps_ext:
    if(necessary_layer_flag){
        for(i=0 ;i < num_layer_sets;i++)
            av_free(necessary_layer_flag[i]);
        av_free(necessary_layer_flag );
    }

    if(layer_set_layer_id_list){
        for(i = 0; i < num_layer_sets; i++){
            av_free(layer_set_layer_id_list[i]);
        }
        av_free(layer_set_layer_id_list );
    }

    if(tree_partition_layer_id_list){
        for( i = 0; i < max_layers; i++ ){
            av_free(tree_partition_layer_id_list[i]);
        }
        av_free(tree_partition_layer_id_list);
    }


    if(id_direct_ref_layer){
        for( i = 0; i < 64; i++ ) {
            av_free(id_direct_ref_layer[i]); //max_layer ID in nuh
            av_free(id_ref_layer[i]);        //max_layer ID in nuh
            av_free(id_predicted_layer[i]);  //max_layer ID in nuh
        }
        av_free(id_direct_ref_layer); //max_layer ID in nuh
        av_free(id_ref_layer);        //max_layer ID in nuh
        av_free(id_predicted_layer);  //max_layer ID in nuh
    }

    if(dependency_flag){
        for(i = 0; i < max_layers; i++){
            av_free(dependency_flag[i]);
        }
        av_free(dependency_flag);
    }
    return ret;
}

int ff_hevc_decode_nal_vps(GetBitContext *gb, AVCodecContext *avctx,
                           HEVCParamSets *ps)
{
    HEVCVPS     *vps;
    AVBufferRef *vps_buf = av_buffer_allocz(sizeof(*vps));
    int i,j;

    if (!vps_buf)
        return AVERROR(ENOMEM);

    vps = (HEVCVPS*)vps_buf->data;

    vps->vps_id = get_bits(gb, 4);
    if (vps->vps_id >= HEVC_MAX_VPS_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "VPS id out of range: %d\n", vps->vps_id);
        goto err;
    }

    av_log(avctx, AV_LOG_TRACE, "Parsing VPS : id:%d\n", vps->vps_id);

    vps->vps_base_layer_internal_flag  = get_bits(gb, 1);
    vps->vps_base_layer_available_flag = get_bits(gb, 1);
    //FIXME this is not really standard this is a hack for non HEVC base
    vps->vps_nonHEVCBaseLayerFlag = (vps->vps_base_layer_available_flag && !vps->vps_base_layer_internal_flag);
    
    vps->vps_max_layers               = get_bits(gb, 6) + 1;
    vps->vps_max_sub_layers           = get_bits(gb, 3) + 1;
    vps->vps_temporal_id_nesting_flag = get_bits1(gb);


    vps->vps_reserved_0xffff_16bits = get_bits(gb, 16);
    if (vps->vps_reserved_0xffff_16bits != 0xffff) { // vps_reserved_ffff_16bits
        av_log(avctx, AV_LOG_ERROR, "vps_reserved_ffff_16bits is not 0xffff\n");
        goto err;
    }

    if (vps->vps_max_sub_layers > HEVC_MAX_SUB_LAYERS) {
        av_log(avctx, AV_LOG_ERROR, "vps_max_sub_layers out of range: %d\n",
               vps->vps_max_sub_layers);
        goto err;
    }

    if (parse_ptl(gb, avctx, &vps->ptl, vps->vps_max_sub_layers, 1) < 0)
        goto err;

    vps->vps_sub_layer_ordering_info_present_flag = get_bits1(gb);

    i = vps->vps_sub_layer_ordering_info_present_flag ? 0 : vps->vps_max_sub_layers - 1; // Difference with the code, i is always starts from 0
    for (; i < vps->vps_max_sub_layers; i++) {
        vps->vps_max_dec_pic_buffering[i] = get_ue_golomb_long(gb) + 1;
        vps->vps_max_num_reorder_pics[i]  = get_ue_golomb_long(gb);
        vps->vps_max_latency_increase[i]  = get_ue_golomb_long(gb) - 1;


        if (vps->vps_max_dec_pic_buffering[i] > HEVC_MAX_DPB_SIZE || !vps->vps_max_dec_pic_buffering[i]) {
            av_log(avctx, AV_LOG_ERROR, "vps_max_dec_pic_buffering_minus1 out of range: %d\n",
                   vps->vps_max_dec_pic_buffering[i] - 1);
            goto err;
        }
        if (vps->vps_max_num_reorder_pics[i] > vps->vps_max_dec_pic_buffering[i] /*- 1*/) {
            av_log(avctx, AV_LOG_WARNING, "vps_max_num_reorder_pics out of range: %d\n",
                   vps->vps_max_num_reorder_pics[i]);
            if (avctx->err_recognition & AV_EF_EXPLODE)
                goto err;
        }
        // Set all sub layers picture ordering info based on first layer
        if (!vps->vps_sub_layer_ordering_info_present_flag) {
            //FIXME: i should start from 1 here since we should already be at i = last iteration;
          for (i++; i < vps->vps_max_sub_layers; i++) {
              vps->vps_max_dec_pic_buffering[i] = vps->vps_max_dec_pic_buffering[0];
              vps->vps_max_num_reorder_pics[i]  = vps->vps_max_num_reorder_pics[0];
              vps->vps_max_latency_increase[i]  = vps->vps_max_latency_increase[0];
          }
          break;
        }
    }

    vps->vps_max_layer_id   = get_bits(gb, 6);
    vps->vps_num_layer_sets = get_ue_golomb_long(gb) + 1;

    if (vps->vps_num_layer_sets < 1 || vps->vps_num_layer_sets > 1024 ||
        (vps->vps_num_layer_sets - 1LL) * (vps->vps_max_layer_id + 1LL) > get_bits_left(gb)) {
        av_log(avctx, AV_LOG_ERROR, "vps_num_layer_sets out of range: %d\n",
               vps->vps_num_layer_sets - 1);
        goto err;
    }

    for (i = 1; i < vps->vps_num_layer_sets; i++)
        for (j = 0; j <= vps->vps_max_layer_id; j++) {
            vps->layer_id_included_flag[i][j] = get_bits1(gb);
        }

    //derive_layerIdList_variables(vps);
 
    vps->vps_timing_info_present_flag = get_bits1(gb);
    if (vps->vps_timing_info_present_flag) {
        vps->vps_num_units_in_tick               = get_bits_long(gb, 32);
        vps->vps_time_scale                      = get_bits_long(gb, 32);
        vps->vps_poc_proportional_to_timing_flag = get_bits1(gb);


        if (vps->vps_poc_proportional_to_timing_flag) {
            vps->vps_num_ticks_poc_diff_one = get_ue_golomb_long(gb) + 1;
        }

        vps->vps_num_hrd_parameters = get_ue_golomb_long(gb);


        if (vps->vps_num_layer_sets >= 1024) {
            av_log(avctx, AV_LOG_ERROR, "vps_num_hrd_parameters out of range: %d\n",
                   vps->vps_num_layer_sets - 1);
            goto err;
        }

        for (i = 0; i < vps->vps_num_hrd_parameters; i++) {
            int common_inf_present = 1;
            vps->hrd_layer_set_idx[i] = get_ue_golomb_long(gb);// hrd_layer_set_idx
            if (i) {
                common_inf_present = get_bits1(gb);
            }
            parse_hrd_parameters(gb, &vps->HrdParam, common_inf_present, vps->vps_max_sub_layers -1);
        }
    }
    vps->vps_extension_flag = get_bits1(gb);

    if (get_bits_left(gb) < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Overread VPS by %d bits\n", -get_bits_left(gb));
        if (ps->vps_list[vps->vps_id])
            goto err;
    }

    if(vps->vps_extension_flag){ // vps_extension_flag
        align_get_bits(gb);
        parse_vps_extension(gb, avctx, vps);
    }

    if (get_bits_left(gb) < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Overread VPS extensions by %d bits\n", -get_bits_left(gb));
        if (ps->vps_list[vps->vps_id])
            goto err;
    }

    if (ps->vps_list[vps->vps_id] &&
        !memcmp(ps->vps_list[vps->vps_id]->data, vps_buf->data, vps_buf->size)) {
        av_log(avctx, AV_LOG_DEBUG, "Ignore duplicated VPS id:%d\n",vps->vps_id);
        av_buffer_unref(&vps_buf);
    } else {
        if(ps->vps_list[vps->vps_id])
            av_log(avctx, AV_LOG_DEBUG, "Replace VPS id:%d\n",vps->vps_id);
        else
            av_log(avctx, AV_LOG_DEBUG, "Place VPS id:%d\n",vps->vps_id);

        remove_vps(ps, vps->vps_id);
        ps->vps_list[vps->vps_id] = vps_buf;
    }

    return 0;

err:
    av_buffer_unref(&vps_buf);
    return AVERROR_INVALIDDATA;
}

//TODO directly give in vui instead of SPS? do we need avctx?
static void parse_vui_parameters(GetBitContext *gb, AVCodecContext *avctx,
                       int apply_defdispwin, HEVCSPS *sps)
{
    HEVCVUI *vui          = &sps->vui;
    GetBitContext backup;//?Don't really understand what  is done here
    int  alt = 0;

    av_log(avctx, AV_LOG_DEBUG, "Decoding VUI\n");

    vui->aspect_ratio_info_present_flag = get_bits1(gb);
    if (vui->aspect_ratio_info_present_flag) {
        vui->aspect_ratio_idc = get_bits(gb, 8);
        if (vui->aspect_ratio_idc < FF_ARRAY_ELEMS(vui_sar))
            vui->sar = vui_sar[vui->aspect_ratio_idc];
        else if (vui->aspect_ratio_idc == 255) {
            vui->sar.num = get_bits(gb, 16);
            vui->sar.den = get_bits(gb, 16);
        } else
            av_log(avctx, AV_LOG_WARNING,
                   "Unknown SAR index: %u.\n", vui->aspect_ratio_idc);
    }

    vui->overscan_info_present_flag = get_bits1(gb);
    if (vui->overscan_info_present_flag){
        vui->overscan_appropriate_flag = get_bits1(gb);
    }

    vui->video_signal_type_present_flag = get_bits1(gb);
    if (vui->video_signal_type_present_flag) {
        vui->video_format                    = get_bits(gb, 3);
        vui->video_full_range_flag           = get_bits1(gb);
        vui->colour_description_present_flag = get_bits1(gb);

        if (vui->colour_description_present_flag) {
            vui->colour_primaries        = 9; get_bits(gb, 8);
            vui->transfer_characteristic = 2; get_bits(gb, 8);
            vui->matrix_coeffs           = 2; get_bits(gb, 8);

            //FIXME this is specific to ffmpeg it may over write some values
            // we don't want to change
            // Set invalid values to "unspecified"
            if (vui->colour_primaries >= AVCOL_PRI_NB)
                vui->colour_primaries = AVCOL_PRI_UNSPECIFIED;
            if (vui->transfer_characteristic >= AVCOL_TRC_NB)
                vui->transfer_characteristic = AVCOL_TRC_UNSPECIFIED;
            if (vui->matrix_coeffs >= AVCOL_SPC_NB)
                vui->matrix_coeffs = AVCOL_SPC_UNSPECIFIED;
            if (vui->matrix_coeffs == AVCOL_SPC_RGB) {
                // FIXME sps->pix_fmt should be set outside of this scope
                switch (sps->pix_fmt) {
                case AV_PIX_FMT_YUV444P:
                    sps->pix_fmt = AV_PIX_FMT_GBRP;
                    break;
                case AV_PIX_FMT_YUV444P10:
                    sps->pix_fmt = AV_PIX_FMT_GBRP10;
                    break;
                case AV_PIX_FMT_YUV444P12:
                    sps->pix_fmt = AV_PIX_FMT_GBRP12;
                    break;
                }
            }
        }
        if (vui->video_full_range_flag && sps->pix_fmt == AV_PIX_FMT_YUV420P)
            sps->pix_fmt = AV_PIX_FMT_YUVJ420P;
    }


    vui->chroma_loc_info_present_flag = get_bits1(gb);
    if (vui->chroma_loc_info_present_flag) {
        vui->chroma_sample_loc_type_top_field    = get_ue_golomb_long(gb);
        vui->chroma_sample_loc_type_bottom_field = get_ue_golomb_long(gb);
    }

    vui->neutral_chroma_indication_flag = get_bits1(gb);
    vui->field_seq_flag                = get_bits1(gb);
    vui->frame_field_info_present_flag = get_bits1(gb);

    //FIXME: I don't understand the reason for this check yet
    if (get_bits_left(gb) >= 68 && show_bits_long(gb, 21) == 0x100000) {
        vui->default_display_window_flag = 0;
        av_log(avctx, AV_LOG_WARNING, "Invalid default display window\n");
    } else
        vui->default_display_window_flag = get_bits1(gb);
    // Backup context in case an alternate header is detected
    memcpy(&backup, gb, sizeof(backup));

    if (vui->default_display_window_flag) {
        //FIXME: I don't understand the reason we change the window size here
        int vert_mult  = 1 + (sps->chroma_format_idc < 2);
        int horiz_mult = 1 + (sps->chroma_format_idc < 3);
        vui->def_disp_win.left_offset   = get_ue_golomb_long(gb) * horiz_mult;
        vui->def_disp_win.right_offset  = get_ue_golomb_long(gb) * horiz_mult;
        vui->def_disp_win.top_offset    = get_ue_golomb_long(gb) *  vert_mult;
        vui->def_disp_win.bottom_offset = get_ue_golomb_long(gb) *  vert_mult;

        if (apply_defdispwin &&
            avctx->flags2 & AV_CODEC_FLAG2_IGNORE_CROP) {
            av_log(avctx, AV_LOG_DEBUG,
                   "discarding vui default display window, "
                   "original values are l:%u r:%u t:%u b:%u\n",
                   vui->def_disp_win.left_offset,
                   vui->def_disp_win.right_offset,
                   vui->def_disp_win.top_offset,
                   vui->def_disp_win.bottom_offset);

            vui->def_disp_win.left_offset   =
            vui->def_disp_win.right_offset  =
            vui->def_disp_win.top_offset    =
            vui->def_disp_win.bottom_offset = 0;
        }
    }

    vui->vui_timing_info_present_flag = get_bits1(gb);

    if (vui->vui_timing_info_present_flag) {
        //FIXME: we may be able to discard this test.
        if( get_bits_left(gb) < 66) {
            // The alternate syntax seem to have timing info located
            // at where def_disp_win is normally located
            av_log(avctx, AV_LOG_WARNING,
                   "Strange VUI timing information, retrying...\n");
            vui->default_display_window_flag = 0;
            memset(&vui->def_disp_win, 0, sizeof(vui->def_disp_win));
            memcpy(gb, &backup, sizeof(backup));
            alt = 1;
        }
        vui->vui_timing_info.vui_num_units_in_tick = get_bits_long(gb, 32);
        vui->vui_timing_info.vui_time_scale        = get_bits_long(gb, 32);
        if (alt) {
            av_log(avctx, AV_LOG_INFO, "Retry got %i/%i fps\n",
                   vui->vui_timing_info.vui_time_scale, vui->vui_timing_info.vui_num_units_in_tick);
        }

        vui->vui_timing_info.vui_poc_proportional_to_timing_flag = get_bits1(gb);
        if (vui->vui_timing_info.vui_poc_proportional_to_timing_flag) {
            vui->vui_timing_info.vui_num_ticks_poc_diff_one_minus1 = get_ue_golomb_long(gb);
        }
        vui->vui_timing_info.vui_hrd_parameters_present_flag = get_bits1(gb);
        if (vui->vui_timing_info.vui_hrd_parameters_present_flag)
            //FIXME: hrd parameters are related to vui more than sps
            parse_hrd_parameters(gb, &vui->vui_timing_info.HrdParam, vui->vui_timing_info.vui_hrd_parameters_present_flag, sps->sps_max_sub_layers -1 );
    }

    vui->bitstream_restriction_flag = get_bits1(gb);
    if (vui->bitstream_restriction_flag) {
        vui->bitstream_restriction.tiles_fixed_structure_flag              = get_bits1(gb);
        vui->bitstream_restriction.motion_vectors_over_pic_boundaries_flag = get_bits1(gb);
        vui->bitstream_restriction.restricted_ref_pic_lists_flag           = get_bits1(gb);
        vui->bitstream_restriction.min_spatial_segmentation_idc            = get_ue_golomb_long(gb);
        vui->bitstream_restriction.max_bytes_per_pic_denom                 = get_ue_golomb_long(gb);
        vui->bitstream_restriction.max_bits_per_min_cu_denom               = get_ue_golomb_long(gb);
        vui->bitstream_restriction.log2_max_mv_length_horizontal           = get_ue_golomb_long(gb);
        vui->bitstream_restriction.log2_max_mv_length_vertical             = get_ue_golomb_long(gb);
    }
}

static void set_default_scaling_list_data(ScalingList *sl)
{
    int matrixId;

    for (matrixId = 0; matrixId < 6; matrixId++) {
        // 4x4 default is 16
        memset(sl->sl[0][matrixId], 16, 16);
        sl->sl_dc[0][matrixId] = 16; // default for 16x16
        sl->sl_dc[1][matrixId] = 16; // default for 32x32
    }
    memcpy(sl->sl[1][0], default_scaling_list_intra, 64);
    memcpy(sl->sl[1][1], default_scaling_list_intra, 64);
    memcpy(sl->sl[1][2], default_scaling_list_intra, 64);
    memcpy(sl->sl[1][3], default_scaling_list_inter, 64);
    memcpy(sl->sl[1][4], default_scaling_list_inter, 64);
    memcpy(sl->sl[1][5], default_scaling_list_inter, 64);
    memcpy(sl->sl[2][0], default_scaling_list_intra, 64);
    memcpy(sl->sl[2][1], default_scaling_list_intra, 64);
    memcpy(sl->sl[2][2], default_scaling_list_intra, 64);
    memcpy(sl->sl[2][3], default_scaling_list_inter, 64);
    memcpy(sl->sl[2][4], default_scaling_list_inter, 64);
    memcpy(sl->sl[2][5], default_scaling_list_inter, 64);
    memcpy(sl->sl[3][0], default_scaling_list_intra, 64);
    memcpy(sl->sl[3][1], default_scaling_list_intra, 64);
    memcpy(sl->sl[3][2], default_scaling_list_intra, 64);
    memcpy(sl->sl[3][3], default_scaling_list_inter, 64);
    memcpy(sl->sl[3][4], default_scaling_list_inter, 64);
    memcpy(sl->sl[3][5], default_scaling_list_inter, 64);
}

static int scaling_list_data(GetBitContext *gb, AVCodecContext *avctx, ScalingList *sl, HEVCSPS *sps)
{
    uint8_t scaling_list_pred_mode_flag;
    int32_t scaling_list_dc_coef[2][6];
    int size_id, matrix_id, pos;
    int i;

    for (size_id = 0; size_id < 4; size_id++)
        for (matrix_id = 0; matrix_id < 6; matrix_id += ((size_id == 3) ? 3 : 1)) {
            scaling_list_pred_mode_flag = get_bits1(gb);
            if (!scaling_list_pred_mode_flag) {
                unsigned int delta = get_ue_golomb_long(gb);
                /* Only need to handle non-zero delta. Zero means default,
                 * which should already be in the arrays. */
                if (delta) {
                    // Copy from previous array.
                    if (matrix_id < delta) {
                        av_log(avctx, AV_LOG_ERROR,
                               "Invalid delta in scaling list data: %d.\n", delta);
                        return AVERROR_INVALIDDATA;
                    }

                    memcpy(sl->sl[size_id][matrix_id],
                           sl->sl[size_id][matrix_id - delta],
                           size_id > 0 ? 64 : 16);
                    if (size_id > 1)
                        sl->sl_dc[size_id - 2][matrix_id] = sl->sl_dc[size_id - 2][matrix_id - delta];
                }
            } else {
                int next_coef, coef_num;
                int32_t scaling_list_delta_coef;

                next_coef = 8;
                coef_num  = FFMIN(64, 1 << (4 + (size_id << 1)));
                if (size_id > 1) {
                    scaling_list_dc_coef[size_id - 2][matrix_id] = get_se_golomb(gb) + 8;
                    next_coef = scaling_list_dc_coef[size_id - 2][matrix_id];
                    sl->sl_dc[size_id - 2][matrix_id] = next_coef;
                }
                for (i = 0; i < coef_num; i++) {
                    if (size_id == 0)
                        pos = 4 * ff_hevc_diag_scan4x4_y[i] +
                                  ff_hevc_diag_scan4x4_x[i];
                    else
                        pos = 8 * ff_hevc_diag_scan8x8_y[i] +
                                  ff_hevc_diag_scan8x8_x[i];

                    scaling_list_delta_coef = get_se_golomb(gb);
                    next_coef = (next_coef + scaling_list_delta_coef + 256) % 256;
                    sl->sl[size_id][matrix_id][pos] = next_coef;
                }
            }
        }

    if (sps && sps->chroma_format_idc == 3) {
        for (i = 0; i < 64; i++) {
            sl->sl[3][1][i] = sl->sl[2][1][i];
            sl->sl[3][2][i] = sl->sl[2][2][i];
            sl->sl[3][4][i] = sl->sl[2][4][i];
            sl->sl[3][5][i] = sl->sl[2][5][i];
        }
        sl->sl_dc[1][1] = sl->sl_dc[0][1];
        sl->sl_dc[1][2] = sl->sl_dc[0][2];
        sl->sl_dc[1][4] = sl->sl_dc[0][4];
        sl->sl_dc[1][5] = sl->sl_dc[0][5];
    }


    return 0;
}


static int sps_range_extensions(GetBitContext *gb, AVCodecContext *avctx, HEVCSPS *sps)
{
    sps->transform_skip_rotation_enabled_flag    = get_bits1(gb);
    sps->transform_skip_context_enabled_flag     = get_bits1(gb);
    sps->implicit_rdpcm_enabled_flag             = get_bits1(gb);
    sps->explicit_rdpcm_enabled_flag             = get_bits1(gb);
    sps->extended_precision_processing_flag      = get_bits1(gb);
    sps->intra_smoothing_disabled_flag           = get_bits1(gb);
    sps->high_precision_offsets_enabled_flag     = get_bits1(gb);
    sps->persistent_rice_adaptation_enabled_flag = get_bits1(gb);
    sps->cabac_bypass_alignment_enabled_flag     = get_bits1(gb);


    if (sps->extended_precision_processing_flag)
        av_log(avctx, AV_LOG_WARNING,
              "extended_precision_processing_flag not yet implemented\n");

    if (sps->high_precision_offsets_enabled_flag)
        av_log(avctx, AV_LOG_WARNING,
               "high_precision_offsets_enabled_flag not yet implemented\n");

    if (sps->cabac_bypass_alignment_enabled_flag)
        av_log(avctx, AV_LOG_WARNING,
               "cabac_bypass_alignment_enabled_flag not yet implemented\n");
    return 0;
}

//FIXME: this functions only reads one element and returns nothing
static inline int sps_multilayer_extensions(GetBitContext *gb, AVCodecContext *avctx, HEVCSPS *sps)
{
    sps->inter_view_mv_vert_constraint_flag = get_bits1(gb);
    return 0;
}

int ff_hevc_parse_sps(HEVCSPS *sps, GetBitContext *gb, unsigned int *sps_id,
                      int apply_defdispwin, AVBufferRef **vps_list, AVCodecContext *avctx,
                      int nuh_layer_id)
{
    const AVPixFmtDescriptor *desc;
    HEVCVPS *vps;
    int ret    = 0;
    int start;
    int i;

    sps->v1_compatible = 1;
    sps->chroma_format_idc = 1;

    sps->vps_id = get_bits(gb, 4);

    //FIXME since we only read 4 bits, VPS id should always be valid
    if (sps->vps_id >= HEVC_MAX_VPS_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "VPS id out of range: %d\n", sps->vps_id);
        return AVERROR_INVALIDDATA;
    }

    //TODO the VPS shall actually exist
    if (vps_list && !vps_list[sps->vps_id]) {
        av_log(avctx, AV_LOG_ERROR, "Error when parsing SPS, VPS %d does not exist\n",
               sps->vps_id);
        return AVERROR_INVALIDDATA;
    }

    if((HEVCVPS *) vps_list[sps->vps_id])
        vps = (HEVCVPS *) vps_list[sps->vps_id]->data;
    
    if (!nuh_layer_id) {
        sps->sps_max_sub_layers = get_bits(gb, 3) + 1;
        if (sps->sps_max_sub_layers > HEVC_MAX_SUB_LAYERS) {
            av_log(avctx, AV_LOG_ERROR, "sps_max_sub_layers out of range: %d\n",
                   sps->sps_max_sub_layers);
            return AVERROR_INVALIDDATA;
        } else if (sps->sps_max_sub_layers > vps->vps_max_sub_layers){
            av_log(avctx, AV_LOG_ERROR, "sps_max_sub_layers_minus1 (%d) greater than vps_max_sub_layers_minus1 (%d)\n",
                   sps->sps_max_sub_layers - 1, vps->vps_max_sub_layers - 1);
            return AVERROR_INVALIDDATA;
        }
    } else {
        sps->sps_ext_or_max_sub_layers = get_bits(gb, 3) + 1;
        sps->v1_compatible = sps->sps_ext_or_max_sub_layers - 1;
        //FIXME This should be done outside of SPS decoding in case the VPS is
        //not available yet
        if ( vps && (sps->sps_ext_or_max_sub_layers - 1) == 7 )
            sps->sps_max_sub_layers = vps->vps_max_sub_layers;
         else
            sps->sps_max_sub_layers = sps->sps_ext_or_max_sub_layers;
    }

    //TODO move to multilayer ctx
    sps->is_multi_layer_ext_sps = ( nuh_layer_id != 0 && sps->v1_compatible == 7 );

    if(!sps->is_multi_layer_ext_sps) {
        sps->sps_temporal_id_nesting_flag = get_bits1(gb);
        if ((ret = parse_ptl(gb, avctx, &sps->ptl, sps->sps_max_sub_layers, 1)) < 0)
            return ret;
    } else { // Not sure for this
        if (vps && sps->sps_max_sub_layers > 1)
            sps->sps_temporal_id_nesting_flag = vps->vps_temporal_id_nesting_flag;
        else
            sps->sps_temporal_id_nesting_flag = 1;
    }

    sps->sps_id = *sps_id = get_ue_golomb_long(gb);

    av_log(avctx, AV_LOG_TRACE, "Parsing SPS vps_id: %d ", sps->vps_id);
    av_log(avctx, AV_LOG_TRACE, "sps_id: %d\n", sps->sps_id);


    if (*sps_id >= HEVC_MAX_SPS_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "SPS id out of range: %d\n", *sps_id);
        return AVERROR_INVALIDDATA;
    }

     if(sps->is_multi_layer_ext_sps) {
         sps->update_rep_format_flag = get_bits1(gb);
         //TODO check vps_num_rep_format > 1 if update rep_format
         if(sps->update_rep_format_flag) {
             sps->sps_rep_format_idx = get_bits(gb, 8);
             //TODO check sps_rep_format_idx < vps_num_rep_formats
         }
     } else // should already be 0
         sps->update_rep_format_flag = 0;

     // TODO infer  chroma_format_idc, separate_colour_plane_flag, pic_width_in_luma_samples
     // pic_height_in_luma_samples, bit_depth_luma,bit_depth chroma conf_win
     // derive rep_format_idx (when independant layer or nuh_layer_id > 0)
     // (in curr pic not when parsing see set_el_parameters in hevcdec.c)
     // because a same SPS can be used by both base and non base layer

    if(!sps->is_multi_layer_ext_sps) {

        //TODO review base HEVC parsing
        sps->chroma_format_idc = get_ue_golomb_long(gb);

        if (!(sps->chroma_format_idc == 0 || sps->chroma_format_idc == 1 || sps->chroma_format_idc == 2 || sps->chroma_format_idc == 3)) {
            avpriv_report_missing_feature(avctx, "chroma_format_idc != {0, 1, 2, 3}\n");
            ret = AVERROR_PATCHWELCOME;
            //goto err;
        }

        if(sps->chroma_format_idc == 3) {
            sps->separate_colour_plane_flag = get_bits1(gb);
        }

        // FIXME why? (we should use a chroma_array_type variable elsewhere instead
        // of overwriting chroma_format_idc)

        if (sps->separate_colour_plane_flag)
            sps->chroma_format_idc = 0;

        sps->width  = get_ue_golomb_long(gb);//pic_width_in_luma_samples
        sps->height = get_ue_golomb_long(gb);//pic_height_in_luma_samples
        if ((ret = av_image_check_size(sps->width,
                                       sps->height, 0, avctx)) < 0)
            return ret;

        sps->conformance_window_flag = get_bits1(gb);

        if (sps->conformance_window_flag) { // FIXME why horiz and vert_mult??

            int vert_mult  = 1 + (sps->chroma_format_idc < 2);
            int horiz_mult = 1 + (sps->chroma_format_idc < 3);

            sps->conf_win.left_offset   = get_ue_golomb_long(gb) * horiz_mult;
            sps->conf_win.right_offset  = get_ue_golomb_long(gb) * horiz_mult;
            sps->conf_win.top_offset    = get_ue_golomb_long(gb) *  vert_mult;
            sps->conf_win.bottom_offset = get_ue_golomb_long(gb) *  vert_mult;


            if (avctx->flags2 & AV_CODEC_FLAG2_IGNORE_CROP) {
                av_log(avctx, AV_LOG_DEBUG,
                       "discarding sps conformance window, "
                       "original values are l:%u r:%u t:%u b:%u\n",
                       sps->conf_win.left_offset,
                       sps->conf_win.right_offset,
                       sps->conf_win.top_offset,
                       sps->conf_win.bottom_offset);

                sps->conf_win.left_offset   =
                sps->conf_win.right_offset  =
                sps->conf_win.top_offset    =
                sps->conf_win.bottom_offset = 0;
            }
            sps->output_window = sps->conf_win;
        }
        sps->bit_depth[CHANNEL_TYPE_LUMA]   = get_ue_golomb_long(gb) + 8;
        sps->bit_depth[CHANNEL_TYPE_CHROMA] = get_ue_golomb_long(gb) + 8;


        if (sps->chroma_format_idc && sps->bit_depth[CHANNEL_TYPE_LUMA] != sps->bit_depth[CHANNEL_TYPE_CHROMA]) {
            av_log(avctx, AV_LOG_ERROR,
                   "Luma bit depth (%d) is different from chroma bit depth (%d), "
                   "this is unsupported.\n",
                   sps->bit_depth[CHANNEL_TYPE_LUMA], sps->bit_depth[CHANNEL_TYPE_CHROMA]);
            ret = AVERROR_INVALIDDATA;
            //goto err;
        }
        //FIXME this could be moved elsewhere
        switch (sps->bit_depth[CHANNEL_TYPE_CHROMA]) {
        case 8:
            if (sps->chroma_format_idc == 0) sps->pix_fmt = AV_PIX_FMT_GRAY8;
            if (sps->chroma_format_idc == 1) sps->pix_fmt = AV_PIX_FMT_YUV420P;
            if (sps->chroma_format_idc == 2) sps->pix_fmt = AV_PIX_FMT_YUV422P;
            if (sps->chroma_format_idc == 3) sps->pix_fmt = AV_PIX_FMT_YUV444P;
            break;
        case 9:
            if (sps->chroma_format_idc == 0) sps->pix_fmt = AV_PIX_FMT_GRAY16;
            if (sps->chroma_format_idc == 1) sps->pix_fmt = AV_PIX_FMT_YUV420P9;
            if (sps->chroma_format_idc == 2) sps->pix_fmt = AV_PIX_FMT_YUV422P9;
            if (sps->chroma_format_idc == 3) sps->pix_fmt = AV_PIX_FMT_YUV444P9;
            break;
        case 10:
            if (sps->chroma_format_idc == 0) sps->pix_fmt = AV_PIX_FMT_GRAY16;
            if (sps->chroma_format_idc == 1) sps->pix_fmt = AV_PIX_FMT_YUV420P10;
            if (sps->chroma_format_idc == 2) sps->pix_fmt = AV_PIX_FMT_YUV422P10;
            if (sps->chroma_format_idc == 3) sps->pix_fmt = AV_PIX_FMT_YUV444P10;
            break;
        case 12:
            if (sps->chroma_format_idc == 0) sps->pix_fmt = AV_PIX_FMT_GRAY16;
            if (sps->chroma_format_idc == 1) sps->pix_fmt = AV_PIX_FMT_YUV420P12;
            if (sps->chroma_format_idc == 2) sps->pix_fmt = AV_PIX_FMT_YUV422P12;
            if (sps->chroma_format_idc == 3) sps->pix_fmt = AV_PIX_FMT_YUV444P12;
            break;
        case 14:
            if (sps->chroma_format_idc == 1) sps->pix_fmt = AV_PIX_FMT_YUV420P14;
            if (sps->chroma_format_idc == 2) sps->pix_fmt = AV_PIX_FMT_YUV422P14;
            if (sps->chroma_format_idc == 3) sps->pix_fmt = AV_PIX_FMT_YUV444P14;
            break;
        default:
            av_log(avctx, AV_LOG_ERROR,
                   "4:2:0, 4:2:2, 4:4:4 supports are currently specified for 8, 10, 12 and 14 bits.\n");
            return AVERROR_PATCHWELCOME;
        }
        //TODO clean this part and move to set_el_parameters
    } else if(vps) {//TODO check this
        RepFormat Rep;
        if (vps && sps->update_rep_format_flag)
            Rep = vps->vps_ext.rep_format[sps->sps_rep_format_idx];
        else {
            if (vps && (vps->vps_ext.vps_num_rep_formats_minus1+1) > 1)
                Rep = vps->vps_ext.rep_format[vps->vps_ext.vps_rep_format_idx[nuh_layer_id]];
            else
                Rep = vps->vps_ext.rep_format[0];
        }
        sps->width  = Rep.pic_width_vps_in_luma_samples;
        sps->height = Rep.pic_height_vps_in_luma_samples;
        sps->bit_depth[CHANNEL_TYPE_LUMA]   = Rep.bit_depth_vps[CHANNEL_TYPE_LUMA];
        sps->bit_depth[CHANNEL_TYPE_CHROMA] = Rep.bit_depth_vps[CHANNEL_TYPE_CHROMA];
        sps->chroma_format_idc = Rep.chroma_format_vps_idc;

        if(Rep.chroma_format_vps_idc) {
            switch (Rep.bit_depth_vps[CHANNEL_TYPE_LUMA]) {
            case 8:  sps->pix_fmt = AV_PIX_FMT_YUV420P;   break;
            case 9:  sps->pix_fmt = AV_PIX_FMT_YUV420P9;  break;
            case 10: sps->pix_fmt = AV_PIX_FMT_YUV420P10; break;
            default:
                av_log(avctx, AV_LOG_ERROR, "-- Unsupported bit depth: %d\n",
                		sps->bit_depth[CHANNEL_TYPE_LUMA]);
                ret = AVERROR_PATCHWELCOME;
                //goto err;
            }
        } else {
            av_log(avctx, AV_LOG_ERROR,
                    "non-4:2:0 support is currently unspecified %d.\n",Rep.chroma_format_vps_idc);
            //return AVERROR_PATCHWELCOME;
        }
    }

    //TODO use this later
    desc = av_pix_fmt_desc_get(sps->pix_fmt);
    if (!desc) {
        ret = AVERROR(EINVAL);
        //goto err;
    }
    sps->hshift[0] = sps->vshift[0] = 0;
    sps->hshift[2] = sps->hshift[1] = desc->log2_chroma_w;
    sps->vshift[2] = sps->vshift[1] = desc->log2_chroma_h;
    sps->pixel_shift[CHANNEL_TYPE_LUMA]   = sps->bit_depth[CHANNEL_TYPE_LUMA] > 8;
    sps->pixel_shift[CHANNEL_TYPE_CHROMA] = sps->bit_depth[CHANNEL_TYPE_CHROMA] > 8;
/*end map_pix_format*/

    sps->log2_max_poc_lsb = get_ue_golomb_long(gb) + 4;
    if (sps->log2_max_poc_lsb > 16) {
        av_log(avctx, AV_LOG_ERROR, "log2_max_pic_order_cnt_lsb_minus4 out range: %d\n",
               sps->log2_max_poc_lsb - 4);
        ret = AVERROR_INVALIDDATA;
        //goto err;
    }

    if(!sps->is_multi_layer_ext_sps) {

        sps->sps_sub_layer_ordering_info_present_flag = get_bits1(gb);


        start = sps->sps_sub_layer_ordering_info_present_flag ? 0 : sps->sps_max_sub_layers - 1;
        for (i = start; i < sps->sps_max_sub_layers; i++) {
            sps->temporal_layer[i].max_dec_pic_buffering = get_ue_golomb_long(gb) + 1;
            sps->temporal_layer[i].num_reorder_pics      = get_ue_golomb_long(gb);
            sps->temporal_layer[i].max_latency_increase  = get_ue_golomb_long(gb) - 1;


            //TODO multi layer conformance for max_dec_pic_buffering
            if (sps->temporal_layer[i].max_dec_pic_buffering > HEVC_MAX_DPB_SIZE) {
                av_log(avctx, AV_LOG_ERROR, "sps_max_dec_pic_buffering_minus1 out of range: %d\n",
                       sps->temporal_layer[i].max_dec_pic_buffering - 1);
                return AVERROR_INVALIDDATA;
                //goto err;
            }
            
            if (sps->temporal_layer[i].num_reorder_pics > sps->temporal_layer[i].max_dec_pic_buffering - 1) {
                av_log(avctx, AV_LOG_WARNING, "sps_max_num_reorder_pics out of range: %d\n",
                       sps->temporal_layer[i].num_reorder_pics);
                if (avctx->err_recognition & AV_EF_EXPLODE ||
                    sps->temporal_layer[i].num_reorder_pics > HEVC_MAX_DPB_SIZE - 1) {
                    return AVERROR_INVALIDDATA;
                }
                sps->temporal_layer[i].max_dec_pic_buffering = sps->temporal_layer[i].num_reorder_pics + 1;
            }
            if (!sps->sps_sub_layer_ordering_info_present_flag) {
                for (i = start; i < start; i++) {
                    sps->temporal_layer[i].max_dec_pic_buffering = sps->temporal_layer[start].max_dec_pic_buffering;
                    sps->temporal_layer[i].num_reorder_pics      = sps->temporal_layer[start].num_reorder_pics;
                    sps->temporal_layer[i].max_latency_increase  = sps->temporal_layer[start].max_latency_increase;
                }
                break;
            }
        }
    }
    sps->log2_min_cb_size                    = get_ue_golomb_long(gb) + 3;
    sps->log2_diff_max_min_cb_size           = get_ue_golomb_long(gb);
    sps->log2_min_tb_size                    = get_ue_golomb_long(gb) + 2;
    sps->log2_diff_max_min_tb_size           = get_ue_golomb_long(gb);

    sps->log2_max_trafo_size                 = sps->log2_diff_max_min_tb_size +
                                               sps->log2_min_tb_size;


    if (sps->log2_min_cb_size < 3 || sps->log2_min_cb_size > 30) {
        av_log(avctx, AV_LOG_ERROR, "Invalid value %d for log2_min_cb_size", sps->log2_min_cb_size);
        return AVERROR_INVALIDDATA;
    }

    if (sps->log2_diff_max_min_cb_size > 30) {
        av_log(avctx, AV_LOG_ERROR, "Invalid value %d for log2_diff_max_min_coding_block_size", sps->log2_diff_max_min_cb_size);
        return AVERROR_INVALIDDATA;
    }

    if (sps->log2_min_tb_size >= sps->log2_min_cb_size || sps->log2_min_tb_size < 2) {
        av_log(avctx, AV_LOG_ERROR, "Invalid value for log2_min_tb_size");
        return AVERROR_INVALIDDATA;
    }

    if ( sps->log2_diff_max_min_tb_size > 30) {
        av_log(avctx, AV_LOG_ERROR, "Invalid value %d for log2_diff_max_min_transform_block_size", sps->log2_diff_max_min_tb_size);
        return AVERROR_INVALIDDATA;
    }

    sps->max_transform_hierarchy_depth_inter = get_ue_golomb_long(gb);
    sps->max_transform_hierarchy_depth_intra = get_ue_golomb_long(gb);

    sps->scaling_list_enabled_flag = get_bits1(gb);

    if (sps->scaling_list_enabled_flag) {
        if (sps->is_multi_layer_ext_sps) {
            sps->sps_infer_scaling_list_flag =  get_bits1(gb);
        }
        if (sps->sps_infer_scaling_list_flag) {
            sps->sps_scaling_list_ref_layer_id = get_bits(gb, 6);
            //TODO check sps_scaling_list_ref_layer_id < 63 should be ok since 6 bits
            //greater than zero when vps_bse_layer_internal = 0 etc
            //sps->scaling_list_enabled_flag = 0;//TODO understand this reset
        } else {
            set_default_scaling_list_data(&sps->scaling_list);
            sps->sps_scaling_list_data_present_flag = get_bits1(gb);
            if (sps->sps_scaling_list_data_present_flag) {
                ret = scaling_list_data(gb, avctx, &sps->scaling_list, sps);
                if (ret < 0)
                    return ret;
            }
        }
    }

    sps->amp_enabled_flag = get_bits1(gb);
    sps->sao_enabled_flag = get_bits1(gb);
    sps->pcm_enabled_flag = get_bits1(gb);


    if (sps->sao_enabled_flag)
        av_log(avctx, AV_LOG_DEBUG, "SAO enabled\n");

    if (sps->pcm_enabled_flag) {
        sps->pcm.bit_depth        = get_bits(gb, 4) + 1;
        sps->pcm.bit_depth_chroma = get_bits(gb, 4) + 1;
        sps->pcm.log2_min_pcm_cb_size = get_ue_golomb_long(gb) + 3;
        sps->pcm.log2_max_pcm_cb_size = sps->pcm.log2_min_pcm_cb_size +
                                        get_ue_golomb_long(gb);

        if (sps->pcm.bit_depth > sps->bit_depth[CHANNEL_TYPE_LUMA]) {
            av_log(avctx, AV_LOG_ERROR,
                   "PCM bit depth (%d) is greater than normal bit depth (%d)\n",
                   sps->pcm.bit_depth, sps->bit_depth[CHANNEL_TYPE_LUMA]);
            return AVERROR_INVALIDDATA;
        }
        sps->pcm.loop_filter_disable_flag = get_bits1(gb);

    }

    sps->num_short_term_rps = get_ue_golomb_long(gb);


    if (sps->num_short_term_rps > HEVC_MAX_SHORT_TERM_RPS_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "Too many short term RPS: %d.\n",
               sps->num_short_term_rps);
        return AVERROR_INVALIDDATA;
    }

    for (i = 0; i < sps->num_short_term_rps; i++) {
        if ((ret = ff_hevc_decode_short_term_rps(gb, avctx, &sps->st_rps[i],
                                                 sps, 0)) < 0)
            return ret;
    }

    sps->long_term_ref_pics_present_flag = get_bits1(gb);


    if (sps->long_term_ref_pics_present_flag) {
        sps->num_long_term_ref_pics_sps = get_ue_golomb_long(gb);


        if (sps->num_long_term_ref_pics_sps > 31U) {
            av_log(avctx, AV_LOG_ERROR, "num_long_term_ref_pics_sps %d is out of range.\n",
                   sps->num_long_term_ref_pics_sps);
            return AVERROR_INVALIDDATA;
        }
        for (i = 0; i < sps->num_long_term_ref_pics_sps; i++) {
            sps->lt_ref_pic_poc_lsb_sps[i]       = get_bits(gb, sps->log2_max_poc_lsb);
            sps->used_by_curr_pic_lt_sps_flag[i] = get_bits1(gb);

        }
    }

    //TODO what is this??
    if(nuh_layer_id > 0)
        sps->set_mfm_enabled_flag = 1;
    else
        sps->set_mfm_enabled_flag = 0;

    sps->sps_temporal_mvp_enabled_flag          = get_bits1(gb);
    sps->sps_strong_intra_smoothing_enable_flag = get_bits1(gb);

    //FIXME: why here? We don't even know if we have some vui yet.
    sps->vui.sar = (AVRational){0, 1};

    sps->vui_parameters_present_flag = get_bits1(gb);


    if (sps->vui_parameters_present_flag)
        parse_vui_parameters(gb, avctx, apply_defdispwin, sps);

#if OHCONFIG_AMT
    sps->use_intra_emt = get_bits1(gb);
    sps->use_inter_emt = get_bits1(gb);
#endif

    sps->sps_extension_present_flag = get_bits1(gb);
    if (sps->sps_extension_present_flag) {

        sps->sps_range_extension_flag      = get_bits1(gb);
        sps->sps_multilayer_extension_flag = get_bits1(gb);
        sps->sps_3d_extension_flag         = get_bits1(gb);
        sps->sps_extension_5bits = get_bits(gb, 5);



        if (sps->sps_range_extension_flag)
            sps_range_extensions(gb, avctx, sps);

        if (sps->sps_multilayer_extension_flag)
            sps_multilayer_extensions(gb, avctx, sps);

    } else {
        // Read more RSB data 
    }

    //TODO Clean this part
    if (apply_defdispwin) {
        sps->output_window.left_offset   += sps->vui.def_disp_win.left_offset;
        sps->output_window.right_offset  += sps->vui.def_disp_win.right_offset;
        sps->output_window.top_offset    += sps->vui.def_disp_win.top_offset;
        sps->output_window.bottom_offset += sps->vui.def_disp_win.bottom_offset;
    }
    if (sps->output_window.left_offset & (0x1F >> (sps->pixel_shift[CHANNEL_TYPE_LUMA])) &&
        !(avctx->flags & AV_CODEC_FLAG_UNALIGNED)) {
        sps->output_window.left_offset &= ~(0x1F >> (sps->pixel_shift[CHANNEL_TYPE_LUMA]));
        av_log(avctx, AV_LOG_WARNING, "Reducing left output window to %d "
               "chroma samples to preserve alignment.\n",
               sps->output_window.left_offset);
    }
    sps->output_width  = sps->width -
                         (sps->output_window.left_offset + sps->output_window.right_offset);
    sps->output_height = sps->height -
                         (sps->output_window.top_offset + sps->output_window.bottom_offset);
    if (sps->width  <= sps->output_window.left_offset + (int64_t)sps->output_window.right_offset  ||
        sps->height <= sps->output_window.top_offset  + (int64_t)sps->output_window.bottom_offset) {
        av_log(avctx, AV_LOG_WARNING, "Invalid visible frame dimensions: %dx%d.\n",
               sps->output_width, sps->output_height);
        if (avctx->err_recognition & AV_EF_EXPLODE) {
            //return AVERROR_INVALIDDATA;
        }
        av_log(avctx, AV_LOG_WARNING,
               "Displaying the whole video surface.\n");
        memset(&sps->conf_win, 0, sizeof(sps->conf_win));
        memset(&sps->output_window, 0, sizeof(sps->output_window));
        sps->output_width               = sps->width;
        sps->output_height              = sps->height;
    }

    // Inferred parameters
    sps->log2_ctb_size = sps->log2_min_cb_size +
                         sps->log2_diff_max_min_cb_size;
    sps->log2_min_pu_size = sps->log2_min_cb_size - 1;

    if (sps->log2_ctb_size > HEVC_MAX_LOG2_CTB_SIZE) {
        av_log(avctx, AV_LOG_ERROR, "CTB size out of range: 2^%d\n", sps->log2_ctb_size);
        return AVERROR_INVALIDDATA;
    }
    if (sps->log2_ctb_size < 4) {
        av_log(avctx,
               AV_LOG_ERROR,
               "log2_ctb_size %d differs from the bounds of any known profile\n",
               sps->log2_ctb_size);
        avpriv_request_sample(avctx, "log2_ctb_size %d", sps->log2_ctb_size);
        return AVERROR_INVALIDDATA;
    }

    sps->ctb_width  = (sps->width  + (1 << sps->log2_ctb_size) - 1) >> sps->log2_ctb_size;
    sps->ctb_height = (sps->height + (1 << sps->log2_ctb_size) - 1) >> sps->log2_ctb_size;
    sps->ctb_size   = sps->ctb_width * sps->ctb_height;

    sps->min_cb_width  = sps->width  >> sps->log2_min_cb_size;
    sps->min_cb_height = sps->height >> sps->log2_min_cb_size;
    sps->min_tb_width  = sps->width  >> sps->log2_min_tb_size;
    sps->min_tb_height = sps->height >> sps->log2_min_tb_size;
    sps->min_pu_width  = sps->width  >> sps->log2_min_pu_size;
    sps->min_pu_height = sps->height >> sps->log2_min_pu_size;
    sps->tb_mask       = (1 << (sps->log2_ctb_size - sps->log2_min_tb_size)) - 1;

    sps->qp_bd_offset = 6 * (sps->bit_depth[CHANNEL_TYPE_LUMA] - 8);

    if (av_mod_uintp2(sps->width,  sps->log2_min_cb_size) ||
        av_mod_uintp2(sps->height, sps->log2_min_cb_size)) {
        av_log(avctx, AV_LOG_ERROR, "Invalid coded frame dimensions.\n");
        //return AVERROR_INVALIDDATA;
    }
    if (sps->log2_ctb_size > HEVC_MAX_LOG2_CTB_SIZE) {
        av_log(avctx, AV_LOG_ERROR, "CTB size out of range: 2^%d\n", sps->log2_ctb_size);
        return AVERROR_INVALIDDATA;
        //goto err;
    }
    if (sps->max_transform_hierarchy_depth_inter > sps->log2_ctb_size - sps->log2_min_tb_size) {
        av_log(avctx, AV_LOG_ERROR, "max_transform_hierarchy_depth_inter out of range: %d\n",
               sps->max_transform_hierarchy_depth_inter);
        return AVERROR_INVALIDDATA;
    }
    if (sps->max_transform_hierarchy_depth_intra > sps->log2_ctb_size - sps->log2_min_tb_size) {
        av_log(avctx, AV_LOG_ERROR, "max_transform_hierarchy_depth_intra out of range: %d\n",
               sps->max_transform_hierarchy_depth_intra);
        return AVERROR_INVALIDDATA;
    }
    if (sps->log2_max_trafo_size > FFMIN(sps->log2_ctb_size, 5)) {
        av_log(avctx, AV_LOG_ERROR,
               "max transform block size out of range: %d\n",
               sps->log2_max_trafo_size);
        return AVERROR_INVALIDDATA;
    }
    if (get_bits_left(gb) < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Overread SPS by %d bits\n", -get_bits_left(gb));
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

int ff_hevc_decode_nal_sps(GetBitContext *gb, AVCodecContext *avctx,
                           HEVCParamSets *ps, int apply_defdispwin, int nuh_layer_id)
{
    unsigned int sps_id;
    int ret;
    HEVCSPS     *sps;
    AVBufferRef *sps_buf;

    sps_buf = av_buffer_allocz(sizeof(*sps));

    if (!sps_buf)
        return AVERROR(ENOMEM);

    sps = (HEVCSPS*)sps_buf->data;

    av_log(avctx, AV_LOG_DEBUG, "Decoding SPS\n");

    ret = ff_hevc_parse_sps(sps, gb, &sps_id,
                            apply_defdispwin,
                            ps->vps_list, avctx, nuh_layer_id);
    if (ret < 0) {
        av_buffer_unref(&sps_buf);
        return ret;
    }

    if (avctx->debug & FF_DEBUG_BITSTREAM) {
        av_log(avctx, AV_LOG_DEBUG,
               "Parsed SPS: id %d; coded wxh: %dx%d; "
               "cropped wxh: %dx%d; pix_fmt: %s.\n",
               sps_id, sps->width, sps->height,
               sps->output_width, sps->output_height,
               av_get_pix_fmt_name(sps->pix_fmt));
    }

    /* check if this is a repeat of an already parsed SPS, then keep the
     * original one.
     * otherwise drop all PPSes that depend on it */
    if (ps->sps_list[sps_id] && !memcmp(ps->sps_list[sps_id]->data, sps_buf->data, sps_buf->size)){
        av_buffer_unref(&sps_buf);
    } else {
        remove_sps(ps, sps_id);
        ps->sps_list[sps_id] = sps_buf;
    }
    return 0;
}

static void hevc_pps_free(void *opaque, uint8_t *data)
{
    HEVCPPS *pps = (HEVCPPS*)data;

    av_freep(&pps->column_width);
    av_freep(&pps->row_height);
    av_freep(&pps->col_bd);
    av_freep(&pps->row_bd);
    av_freep(&pps->col_idxX);
    av_freep(&pps->ctb_addr_rs_to_ts);
    av_freep(&pps->ctb_addr_ts_to_rs);
    av_freep(&pps->tile_pos_rs);
    av_freep(&pps->tile_id);
    av_freep(&pps->wpp_pos_ts);
    av_freep(&pps->tile_width);
    av_freep(&pps->min_tb_addr_zs_tab);
    av_freep(&pps);
}

static int pps_range_extensions(GetBitContext *gb, AVCodecContext *avctx,
                                HEVCPPS *pps) {
    int i;

    if (pps->transform_skip_enabled_flag) {
        pps->log2_max_transform_skip_block_size = get_ue_golomb_long(gb) + 2;
        if (pps->log2_max_transform_skip_block_size > 2) {
            av_log(avctx, AV_LOG_ERROR,
                   "log2_max_transform_skip_block_size_minus2 is partially implemented.\n");
        }
    }
    pps->cross_component_prediction_enabled_flag = get_bits1(gb);
    pps->chroma_qp_offset_list_enabled_flag = get_bits1(gb);


    if (pps->chroma_qp_offset_list_enabled_flag) {
        pps->diff_cu_chroma_qp_offset_depth = get_ue_golomb_long(gb);
        pps->chroma_qp_offset_list_len_minus1 = get_ue_golomb_long(gb);


        if (pps->chroma_qp_offset_list_len_minus1 && pps->chroma_qp_offset_list_len_minus1 >= 5) {
            av_log(avctx, AV_LOG_ERROR,
                   "chroma_qp_offset_list_len_minus1 shall be in the range [0, 5].\n");
            return AVERROR_INVALIDDATA;
        }
        for (i = 0; i <= pps->chroma_qp_offset_list_len_minus1; i++) {
            pps->cb_qp_offset_list[i] = get_se_golomb_long(gb);
            pps->cr_qp_offset_list[i] = get_se_golomb_long(gb);


            if (pps->cb_qp_offset_list[i]) {
                av_log(avctx, AV_LOG_WARNING,
                       "cb_qp_offset_list not tested yet.\n");
            }

            if (pps->cr_qp_offset_list[i]) {
                av_log(avctx, AV_LOG_WARNING,
                       "cb_qp_offset_list not tested yet.\n");
            }
        }
    }

    pps->log2_sao_offset_scale_luma   = get_ue_golomb_long(gb);
    pps->log2_sao_offset_scale_chroma = get_ue_golomb_long(gb);


    return 0;
}

int setup_pps(AVCodecContext *avctx,
                            HEVCPPS *pps, HEVCSPS *sps)
{
    int log2_diff;
    int pic_area_in_ctbs;
    int i, j, x, y, ctb_addr_rs, tile_id;
    int row = 0, wpp_pos = 0;

if(!pps->is_setup && sps){
    // Inferred parameters
    pps->col_bd   = av_malloc_array(pps->num_tile_columns + 1, sizeof(*pps->col_bd));
    pps->row_bd   = av_malloc_array(pps->num_tile_rows    + 1, sizeof(*pps->row_bd));
    pps->col_idxX = av_malloc_array(sps->ctb_width,            sizeof(*pps->col_idxX));

    if (!pps->col_bd || !pps->row_bd || !pps->col_idxX)
        return AVERROR(ENOMEM);

    if (pps->uniform_spacing_flag) {

        if (!pps->column_width) {
            pps->column_width = av_malloc_array(pps->num_tile_columns, sizeof(*pps->column_width));
            pps->row_height   = av_malloc_array(pps->num_tile_rows,    sizeof(*pps->row_height));
        }

        if (!pps->column_width || !pps->row_height)
            return AVERROR(ENOMEM);

        for (i = 0; i < pps->num_tile_columns; i++) {
            pps->column_width[i] = ((i + 1) * sps->ctb_width) / pps->num_tile_columns -
                                   ( i * sps->ctb_width)      / pps->num_tile_columns;
        }

        for (i = 0; i < pps->num_tile_rows; i++) {
            pps->row_height[i] = ((i + 1) * sps->ctb_height) / pps->num_tile_rows -
                                 (i * sps->ctb_height)       / pps->num_tile_rows;
        }
    }

    pps->col_bd[0] = 0;

    for (i = 0; i < pps->num_tile_columns; i++)
        pps->col_bd[i + 1] = pps->col_bd[i] + pps->column_width[i];

    pps->row_bd[0] = 0;

    for (i = 0; i < pps->num_tile_rows; i++)
        pps->row_bd[i + 1] = pps->row_bd[i] + pps->row_height[i];

    for (i = 0, j = 0; i < sps->ctb_width; i++) {
        if (i > pps->col_bd[j])
            j++;
        pps->col_idxX[i] = j;
    }

    /**
     * 6.5
     */
    //FIXME: we might not have any sps yet
    pic_area_in_ctbs     = sps->ctb_width    * sps->ctb_height;

    pps->ctb_addr_rs_to_ts  = av_malloc_array(pic_area_in_ctbs, sizeof(*pps->ctb_addr_rs_to_ts));
    pps->ctb_addr_ts_to_rs  = av_malloc_array(pic_area_in_ctbs, sizeof(*pps->ctb_addr_ts_to_rs));
    pps->tile_id            = av_malloc_array(pic_area_in_ctbs, sizeof(*pps->tile_id));
    pps->wpp_pos_ts         = av_malloc_array(pic_area_in_ctbs, sizeof(*pps->wpp_pos_ts));
    pps->min_tb_addr_zs_tab = av_malloc_array((sps->tb_mask+2) * (sps->tb_mask+2), sizeof(*pps->min_tb_addr_zs_tab));
    pps->tile_width         = av_malloc_array(pic_area_in_ctbs, sizeof(*pps->tile_width));

    if (!pps->ctb_addr_rs_to_ts || !pps->ctb_addr_ts_to_rs ||
        !pps->tile_id || !pps->min_tb_addr_zs_tab || !pps->tile_width) {
        return AVERROR(ENOMEM);
    }

    for (ctb_addr_rs = 0; ctb_addr_rs < pic_area_in_ctbs; ctb_addr_rs++) {
        int tb_x   = ctb_addr_rs % sps->ctb_width;
        int tb_y   = ctb_addr_rs / sps->ctb_width;
        int tile_x = 0;
        int tile_y = 0;
        int val    = 0;

        for (i = 0; i < pps->num_tile_columns; i++) {
            if (tb_x < pps->col_bd[i + 1]) {
                tile_x = i;
                break;
            }
        }

        for (i = 0; i < pps->num_tile_rows; i++) {
            if (tb_y < pps->row_bd[i + 1]) {
                tile_y = i;
                break;
            }
        }

        for (i = 0; i < tile_x; i++)
            val += pps->row_height[tile_y] * pps->column_width[i];
        for (i = 0; i < tile_y; i++)
            val += sps->ctb_width * pps->row_height[i];

        val += (tb_y - pps->row_bd[tile_y]) * pps->column_width[tile_x] +
               tb_x - pps->col_bd[tile_x];

        pps->ctb_addr_rs_to_ts[ctb_addr_rs] = val;
        pps->ctb_addr_ts_to_rs[val]         = ctb_addr_rs;
    }


    for (j = 0, tile_id = 0; j < pps->num_tile_rows; j++){
        for (i = 0; i < pps->num_tile_columns; i++, tile_id++){
            for (y = pps->row_bd[j]; y < pps->row_bd[j + 1]; y++){
                for (x = pps->col_bd[i]; x < pps->col_bd[i + 1]; x++) {
                    pps->tile_id[pps->ctb_addr_rs_to_ts[y * sps->ctb_width + x]] = tile_id;
                    pps->tile_width[pps->ctb_addr_rs_to_ts[y * sps->ctb_width + x]] = pps->column_width[tile_id % pps->num_tile_columns];
                }
                pps->wpp_pos_ts[row++] = wpp_pos;
                wpp_pos += pps->column_width[tile_id % pps->num_tile_columns];
            }
        }
    }

    pps->tile_pos_rs = av_malloc_array(tile_id, sizeof(*pps->tile_pos_rs));

    if (!pps->tile_pos_rs)
        return AVERROR(ENOMEM);

    for (j = 0; j < pps->num_tile_rows; j++)
        for (i = 0; i < pps->num_tile_columns; i++)
            pps->tile_pos_rs[j * pps->num_tile_columns + i] =
                pps->row_bd[j] * sps->ctb_width + pps->col_bd[i];

    log2_diff = sps->log2_ctb_size - sps->log2_min_tb_size;
    pps->min_tb_addr_zs = &pps->min_tb_addr_zs_tab[1*(sps->tb_mask+2)+1];
    for (y = 0; y < sps->tb_mask+2; y++) {
        pps->min_tb_addr_zs_tab[y*(sps->tb_mask+2)] = -1;
        pps->min_tb_addr_zs_tab[y]    = -1;
    }
    for (y = 0; y < sps->tb_mask+1; y++) {
        for (x = 0; x < sps->tb_mask+1; x++) {
            int tb_x = x >> log2_diff;
            int tb_y = y >> log2_diff;
            int rs   = sps->ctb_width * tb_y + tb_x;
            int val  = pps->ctb_addr_rs_to_ts[rs] << (log2_diff * 2);
            for (i = 0; i < log2_diff; i++) {
                int m = 1 << i;
                val += (m & x ? m * m : 0) + (m & y ? 2 * m * m : 0);
            }
            pps->min_tb_addr_zs[y * (sps->tb_mask+2) + x] = val;
        }
    }
    pps->is_setup = 1;
}
    return 0;
}

SYUVP  GetCuboidVertexPredAll(TCom3DAsymLUT * pc3DAsymLUT, int yIdx , int uIdx , int vIdx , int nVertexIdx){
  SCuboid***  pCuboid = pc3DAsymLUT->S_Cuboid;

  SYUVP sPred;
  if( yIdx == 0 ) {
    sPred.Y = nVertexIdx == 0 ? 1024 : 0;
    sPred.U = nVertexIdx == 1 ? 1024 : 0;
    sPred.V = nVertexIdx == 2 ? 1024 : 0;
  }
  else
    sPred = pCuboid[yIdx-1][uIdx][vIdx].P[nVertexIdx];
  return sPred;
}

static void setCuboidVertexResTree( TCom3DAsymLUT * pc3DAsymLUT, int yIdx , int uIdx , int vIdx , int nVertexIdx , int deltaY , int deltaU , int deltaV ) {
    SYUVP *sYuvp = &pc3DAsymLUT->S_Cuboid[yIdx][uIdx][vIdx].P[nVertexIdx], sPred;
    sPred  = GetCuboidVertexPredAll( pc3DAsymLUT, yIdx , uIdx , vIdx , nVertexIdx);

    sYuvp->Y = sPred.Y + ( deltaY << pc3DAsymLUT->cm_res_quant_bit );
    sYuvp->U = sPred.U + ( deltaU << pc3DAsymLUT->cm_res_quant_bit );
    sYuvp->V = sPred.V + ( deltaV << pc3DAsymLUT->cm_res_quant_bit );
}

static int ReadParam( GetBitContext   *gb, int rParam ) {
  unsigned int prefix, codeWord, rSymbol, sign;
  int param;
  prefix   = get_ue_golomb_long(gb);
  codeWord = get_bits(gb, rParam);
  rSymbol = (prefix<<rParam) + codeWord;

  if(rSymbol) {
    sign = get_bits1(gb);
    param = sign ? -(int)(rSymbol) : (int)(rSymbol);
    return param;
  }
  else
    return 0;
}

static void xParse3DAsymLUTOctant( GetBitContext *gb , TCom3DAsymLUT * pc3DAsymLUT , int nDepth , int yIdx , int uIdx , int vIdx , int length) {
    uint8_t split_octant_flag = nDepth < pc3DAsymLUT->cm_octant_depth;
    int l,m,n, nYPartNum;
    if(split_octant_flag){
      split_octant_flag = get_bits1(gb);
    }
    nYPartNum = 1 << pc3DAsymLUT->cm_y_part_num_log2;
    if( split_octant_flag ) {
        int nHalfLength = length >> 1;
        for(l = 0 ; l < 2 ; l++ )
          for(m = 0 ; m < 2 ; m++ )
            for(n = 0 ; n < 2 ; n++ )
              xParse3DAsymLUTOctant( gb, pc3DAsymLUT , nDepth + 1 , yIdx + l * nHalfLength * nYPartNum , uIdx + m * nHalfLength , vIdx + n * nHalfLength , nHalfLength );
    } else {
        int l, m, u, v, y, nVertexIdx, nFLCbits = pc3DAsymLUT->nMappingShift - pc3DAsymLUT->cm_res_quant_bit -pc3DAsymLUT->cm_flc_bits;

        nFLCbits = nFLCbits >= 0 ? nFLCbits:0;
        for(l = 0 ; l < nYPartNum ; l++ ) {
          int shift = pc3DAsymLUT->cm_octant_depth - nDepth;
          for(nVertexIdx = 0 ; nVertexIdx < 4 ; nVertexIdx++ ) {
            uint8_t  coded_vertex_flag = 0;
            int deltaY = 0 , deltaU = 0 , deltaV = 0;
            coded_vertex_flag = get_bits1(gb);
            if( coded_vertex_flag ) {
                deltaY = ReadParam(gb, nFLCbits );
                deltaU = ReadParam(gb, nFLCbits );
                deltaV = ReadParam(gb, nFLCbits );
            }
            setCuboidVertexResTree( pc3DAsymLUT, yIdx + (l<<shift) , uIdx , vIdx , nVertexIdx , deltaY , deltaU , deltaV );
            for ( m = 1; m < (1<<shift); m++) {
              setCuboidVertexResTree( pc3DAsymLUT, yIdx + (l<<shift) + m , uIdx , vIdx , nVertexIdx , 0 , 0 , 0 );
            }
          }
        }

        for (u=0 ; u<length ; u++ )
              for ( v=0 ; v<length ; v++ )
                if ( u || v )
                  for ( y=0 ; y<length*nYPartNum ; y++ )
                    for( nVertexIdx = 0 ; nVertexIdx < 4 ; nVertexIdx++ )
                      setCuboidVertexResTree( pc3DAsymLUT, yIdx + y , uIdx + u , vIdx + v , nVertexIdx , 0 , 0 , 0 );
    }
}

static void Allocate3DArray(TCom3DAsymLUT * pc3DAsymLUT, int xSize, int ySize, int zSize) {
  int x, y;

  pc3DAsymLUT->S_Cuboid    = av_malloc(xSize*sizeof(SCuboid**)) ;
  pc3DAsymLUT->S_Cuboid[0] = av_malloc(xSize*ySize*sizeof(SCuboid*)) ;
  for( x = 1 ; x < xSize ; x++ )
    pc3DAsymLUT->S_Cuboid[x] = pc3DAsymLUT->S_Cuboid[x-1] + ySize;

  pc3DAsymLUT->S_Cuboid[0][0] = av_mallocz(xSize*ySize*zSize*sizeof(SCuboid));
  for( x = 0 ; x < xSize ; x++ )
    for(y = 0 ; y < ySize ; y++ )
        pc3DAsymLUT->S_Cuboid[x][y] = pc3DAsymLUT->S_Cuboid[0][0] + x * ySize * zSize + y * zSize;
}

/*
static void Display(TCom3DAsymLUT * pc3DAsymLUT, int xSize, int ySize, int zSize) {
  int i, j, k;
  for( i = 0 ; i < xSize ; i++ )
    for(j = 0 ; j < ySize ; j++ )
      for(k = 0 ; k < zSize ; k++ )
        printf("%d %d %d %d - %d %d %d %d - %d %d %d %d \n", pc3DAsymLUT->S_Cuboid[i][j][k].P[0].Y, pc3DAsymLUT->S_Cuboid[i][j][k].P[1].Y, pc3DAsymLUT->S_Cuboid[i][j][k].P[2].Y, pc3DAsymLUT->S_Cuboid[i][j][k].P[3].Y, pc3DAsymLUT->S_Cuboid[i][j][k].P[0].U, pc3DAsymLUT->S_Cuboid[i][j][k].P[1].U, pc3DAsymLUT->S_Cuboid[i][j][k].P[2].U, pc3DAsymLUT->S_Cuboid[i][j][k].P[3].U, pc3DAsymLUT->S_Cuboid[i][j][k].P[0].V, pc3DAsymLUT->S_Cuboid[i][j][k].P[1].V, pc3DAsymLUT->S_Cuboid[i][j][k].P[2].V, pc3DAsymLUT->S_Cuboid[i][j][k].P[3].V);
}*/

void Free3DArray(HEVCPPS * pps) {
  if(pps && 0) {
    av_freep(&pps->pc3DAsymLUT.S_Cuboid[0][0]);
    av_freep(&pps->pc3DAsymLUT.S_Cuboid[0]);
    av_freep(&pps->pc3DAsymLUT.S_Cuboid);
  }
}
static void xParse3DAsymLUT(GetBitContext *gb, TCom3DAsymLUT * pc3DAsymLUT) {
    int i, YSize, CSize;

    pc3DAsymLUT->num_cm_ref_layers_minus1 = get_ue_golomb_long(gb);

    for(  i = 0 ; i <= pc3DAsymLUT->num_cm_ref_layers_minus1; i++ ) {
        pc3DAsymLUT->uiRefLayerId[i] = get_bits(gb, 6);
    }

    pc3DAsymLUT->cm_octant_depth    = get_bits(gb, 2);
    pc3DAsymLUT->cm_y_part_num_log2 = get_bits(gb, 2);

    pc3DAsymLUT->cm_input_luma_bit_depth      = get_ue_golomb_long(gb) + 8;
    pc3DAsymLUT->cm_input_chroma_bit_depth    = get_ue_golomb_long(gb) + 8;
    pc3DAsymLUT->cm_output_luma_bit_depth     = get_ue_golomb_long(gb) + 8;
    pc3DAsymLUT->cm_output_chroma_bit_depth   = get_ue_golomb_long(gb) + 8;

    pc3DAsymLUT->cm_res_quant_bit = get_bits(gb, 2);
    pc3DAsymLUT->cm_flc_bits      = get_bits(gb, 2) + 1;


    pc3DAsymLUT->nAdaptCThresholdU = 1 << ( pc3DAsymLUT->cm_input_chroma_bit_depth - 1 );
    pc3DAsymLUT-> nAdaptCThresholdV = 1 << ( pc3DAsymLUT->cm_input_chroma_bit_depth - 1 );

    if( pc3DAsymLUT->cm_octant_depth == 1 ) {
        pc3DAsymLUT->cm_adapt_threshold_u_delta = get_se_golomb(gb);
        pc3DAsymLUT->cm_adapt_threshold_v_delta = get_se_golomb(gb);

        pc3DAsymLUT->nAdaptCThresholdU += pc3DAsymLUT->cm_adapt_threshold_u_delta;
        pc3DAsymLUT->nAdaptCThresholdV += pc3DAsymLUT->cm_adapt_threshold_v_delta;

    }
    pc3DAsymLUT->delta_bit_depth   = pc3DAsymLUT->cm_output_luma_bit_depth   - pc3DAsymLUT->cm_input_luma_bit_depth;
    pc3DAsymLUT->delta_bit_depth_C = pc3DAsymLUT->cm_output_chroma_bit_depth - pc3DAsymLUT->cm_input_chroma_bit_depth;
    pc3DAsymLUT->max_part_num_log2 = 3*pc3DAsymLUT->cm_octant_depth          + pc3DAsymLUT->cm_y_part_num_log2;

    pc3DAsymLUT->YShift2Idx = pc3DAsymLUT->cm_input_luma_bit_depth - pc3DAsymLUT->cm_octant_depth - pc3DAsymLUT->cm_y_part_num_log2;
    pc3DAsymLUT->UShift2Idx = pc3DAsymLUT->VShift2Idx = pc3DAsymLUT->cm_input_chroma_bit_depth - pc3DAsymLUT->cm_octant_depth;

    pc3DAsymLUT->nMappingShift = 10 + pc3DAsymLUT->cm_input_luma_bit_depth - pc3DAsymLUT->cm_output_luma_bit_depth;

    pc3DAsymLUT->nMappingOffset = 1 << ( pc3DAsymLUT->nMappingShift - 1 );

    YSize = 1 << ( pc3DAsymLUT->cm_octant_depth + pc3DAsymLUT->cm_y_part_num_log2 );
    CSize = 1 << pc3DAsymLUT->cm_octant_depth;

    Allocate3DArray( pc3DAsymLUT , YSize , CSize , CSize );
    xParse3DAsymLUTOctant( gb , pc3DAsymLUT , 0 , 0 ,0 , 0 , 1<<pc3DAsymLUT->cm_octant_depth);
//    Display( pc3DAsymLUT , YSize , CSize , CSize );
}

static int pps_multilayer_extensions(GetBitContext *gb, AVCodecContext *avctx,
                                HEVCPPS *pps) {
    int i;

    pps->poc_reset_info_present_flag = get_bits1(gb);
    pps->pps_infer_scaling_list_flag = get_bits1(gb);


    if( pps->pps_infer_scaling_list_flag ) {
        pps->pps_scaling_list_ref_layer_id = get_bits(gb, 6);
        pps->scaled_ref_layer_offset_present_flag = 0;
    }
    pps->num_ref_loc_offsets = get_ue_golomb_long(gb);

    for(i = 0; i < pps->num_ref_loc_offsets; i++) {
        pps->ref_loc_offset_layer_id = get_bits(gb, 6);
        pps->scaled_ref_layer_offset_present_flag = get_bits1(gb);


        if (pps->scaled_ref_layer_offset_present_flag) {

            pps->scaled_ref_window[i].left_offset   = (get_se_golomb(gb)<< 1);
            pps->scaled_ref_window[i].top_offset    = (get_se_golomb(gb)<< 1);
            pps->scaled_ref_window[i].right_offset  = (get_se_golomb(gb)<< 1);
            pps->scaled_ref_window[i].bottom_offset = (get_se_golomb(gb)<< 1);

        }
        pps->ref_region_offset_present_flag = get_bits1(gb);
        if (pps->ref_region_offset_present_flag) {
            pps->ref_window[i].left_offset   = (get_se_golomb(gb)<< 1);
            pps->ref_window[i].top_offset    = (get_se_golomb(gb)<< 1);
            pps->ref_window[i].right_offset  = (get_se_golomb(gb)<< 1);
            pps->ref_window[i].bottom_offset = (get_se_golomb(gb)<< 1);

        }
        pps->resample_phase_set_present_flag = get_bits1(gb);
        if (pps->resample_phase_set_present_flag) {
            pps->phase_hor_luma[i] = get_ue_golomb_long(gb);
            pps->phase_ver_luma[i] = get_ue_golomb_long(gb);
            pps->phase_hor_chroma[i] = get_ue_golomb_long(gb) - 8;
            pps->phase_ver_chroma[i] = get_ue_golomb_long(gb) - 8;

        }
    }
    pps->colour_mapping_enabled_flag = get_bits1(gb);
    if (pps->colour_mapping_enabled_flag) {
        xParse3DAsymLUT(gb, &pps->pc3DAsymLUT);
        pps->m_nCGSOutputBitDepth[0]  =  pps->pc3DAsymLUT.cm_output_luma_bit_depth;
        pps->m_nCGSOutputBitDepth[1] =   pps->pc3DAsymLUT.cm_output_chroma_bit_depth;
    }
    return 0;
}
/*pps_infer_scaling_list_flag	u(1)
if( pps_infer_scaling_list_flag )
pps_scaling_list_ref_layer_id	u(6)
num_ref_loc_offsets	ue(v)
for( i = 0; i < num_ref_loc_offsets; i++) { [Ed. (JC): can insert a new sub-clause for all the offsets]
    ref_loc_offset_layer_id[ i ]	u(6)
    scaled_ref_layer_offset_present_flag[ i ]	u(1)
    if(scaled_ref_layer_offset_present_flag[ i ]) {
        scaled_ref_layer_left_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
        scaled_ref_layer_top_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
        scaled_ref_layer_right_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
        scaled_ref_layer_bottom_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
    }
    ref_region_offset_present_flag[ i ]	u(1)
    if(ref_region_offset_present_flag[ i ]) {
        ref_region_left_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
        ref_region_top_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
        ref_region_right_offset[ref_loc_offset_layer_id[ i ] ]	se(v)
        ref_region_bottom_offset[ref_loc_offset_layer_id[ i ] ]	se(v)
    }
    resample_phase_set_present_flag[ i ]	u(1)
    if(resample_phase_set_prsent_flag[ i ]) {
        phase_hor_luma[ ref_loc_offset_layer_id[ i ] ]	ue(v)
        phase_ver_luma[ ref_loc_offset_layer_id[ i ] ]	ue(v)
        phase_hor_chroma_plus8[ ref_loc_offset_layer_id[ i ] ]	ue(v)
        phase_ver_chroma_plus8[ ref_loc_offset_layer_id[ i ] ]	ue(v)
    }
}*/

int ff_hevc_decode_nal_pps(GetBitContext *gb, AVCodecContext *avctx,
                           HEVCParamSets *ps)
{
    AVBufferRef *pps_buf;
    HEVCSPS     *sps = NULL;
    HEVCPPS     *pps = av_mallocz(sizeof(*pps));

    int i, ret = 0;

    uint8_t pps_extension_present_flag;


    if (!pps)
        return AVERROR(ENOMEM);

    pps_buf = av_buffer_create((uint8_t *)pps, sizeof(*pps),
                               hevc_pps_free, NULL, 0);
    if (!pps_buf) {
        av_freep(&pps);
        return AVERROR(ENOMEM);
    }

    //av_log(avctx, AV_LOG_DEBUG, "Decoding PPS\n");

    //TODO check Default values
    pps->loop_filter_across_tiles_enabled_flag = 1;
    pps->num_tile_columns                      = 1;
    pps->num_tile_rows                         = 1;
    pps->uniform_spacing_flag                  = 1;
    pps->pps_deblocking_filter_disabled_flag   = 0;
    pps->pps_beta_offset                       = 0;
    pps->pps_tc_offset                         = 0;
    pps->log2_max_transform_skip_block_size    = 2;

    // Coded parameters
    pps->pps_id = get_ue_golomb_long(gb);

    if (pps->pps_id >= HEVC_MAX_PPS_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "PPS id out of range: %d\n", pps->pps_id);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    pps->sps_id = get_ue_golomb_long(gb);
    if (pps->sps_id >= HEVC_MAX_SPS_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "Error when parsing PPS %d, SPS id out of range: %d\n", pps->pps_id, pps->sps_id);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }

    if (!ps->sps_list[pps->sps_id]) {
        av_log(avctx, AV_LOG_ERROR, "Error when parsing PPS %d, SPS %u does not exist.\n", pps->pps_id, pps->sps_id);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
        av_log(avctx, AV_LOG_TRACE, "Parsing PPS id: %d ",pps->pps_id);
        av_log(NULL, AV_LOG_TRACE, "sps_id: %d\n", pps->sps_id);
    if ((HEVCSPS *)ps->sps_list[pps->sps_id])
        sps = (HEVCSPS *)ps->sps_list[pps->sps_id]->data;

    pps->dependent_slice_segments_enabled_flag = get_bits1(gb);
    pps->output_flag_present_flag              = get_bits1(gb);
    pps->num_extra_slice_header_bits           = get_bits(gb, 3);
    pps->sign_data_hiding_flag                 = get_bits1(gb);
    pps->cabac_init_present_flag               = get_bits1(gb);
    pps->num_ref_idx_l0_default_active = get_ue_golomb_long(gb) + 1;
    pps->num_ref_idx_l1_default_active = get_ue_golomb_long(gb) + 1;

    pps->init_qp_minus26             = get_se_golomb(gb);

    pps->constrained_intra_pred_flag = get_bits1(gb);
    pps->transform_skip_enabled_flag = get_bits1(gb);
    pps->cu_qp_delta_enabled_flag    = get_bits1(gb);


    pps->diff_cu_qp_delta_depth   = 0;
    if (pps->cu_qp_delta_enabled_flag) {
        pps->diff_cu_qp_delta_depth = get_ue_golomb_long(gb);
    }   

    if (sps && (pps->diff_cu_qp_delta_depth < 0 ||
        pps->diff_cu_qp_delta_depth > sps->log2_diff_max_min_cb_size)) {
        av_log(avctx, AV_LOG_ERROR, "diff_cu_qp_delta_depth %d is invalid\n",
               pps->diff_cu_qp_delta_depth);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }

    pps->pps_cb_qp_offset = get_se_golomb(gb);
    if (pps->pps_cb_qp_offset < -12 || pps->pps_cb_qp_offset > 12) {
        av_log(avctx, AV_LOG_ERROR, "pps_cb_qp_offset out of range: %d\n",
               pps->pps_cb_qp_offset);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    pps->pps_cr_qp_offset = get_se_golomb(gb);
    if (pps->pps_cr_qp_offset < -12 || pps->pps_cr_qp_offset > 12) {
        av_log(avctx, AV_LOG_ERROR, "pps_cr_qp_offset out of range: %d\n",
               pps->pps_cr_qp_offset);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    pps->pps_slice_chroma_qp_offsets_present_flag = get_bits1(gb);
    pps->weighted_pred_flag                             = get_bits1(gb);
    pps->weighted_bipred_flag                           = get_bits1(gb);
    pps->transquant_bypass_enable_flag                  = get_bits1(gb);
    pps->tiles_enabled_flag                             = get_bits1(gb);
    pps->entropy_coding_sync_enabled_flag               = get_bits1(gb);


    if (pps->entropy_coding_sync_enabled_flag)
        av_log(avctx, AV_LOG_DEBUG, "WPP enabled\n");

    if (pps->tiles_enabled_flag) {
        av_log(avctx, AV_LOG_DEBUG, "Tiles enabled\n");
        pps->num_tile_columns = get_ue_golomb_long(gb) + 1;
        pps->num_tile_rows    = get_ue_golomb_long(gb) + 1;


        if (sps && (pps->num_tile_columns <= 0 ||
            pps->num_tile_columns >= sps->width)) {
            av_log(avctx, AV_LOG_ERROR, "num_tile_columns_minus1 out of range: %d\n",
                   pps->num_tile_columns - 1);
            ret = AVERROR_INVALIDDATA;
            goto err;
        }
        if (sps && (pps->num_tile_rows <= 0 ||
            pps->num_tile_rows >= sps->height)) {
            av_log(avctx, AV_LOG_ERROR, "num_tile_rows_minus1 out of range: %d\n",
                   pps->num_tile_rows - 1);
            ret = AVERROR_INVALIDDATA;
            goto err;
        }

        pps->column_width = av_malloc_array(pps->num_tile_columns, sizeof(*pps->column_width));
        pps->row_height   = av_malloc_array(pps->num_tile_rows,    sizeof(*pps->row_height));

        if (!pps->column_width || !pps->row_height) {
            ret = AVERROR(ENOMEM);
            goto err;
        }

        pps->uniform_spacing_flag = get_bits1(gb);

        if (!pps->uniform_spacing_flag) {
            uint64_t sum = 0;
            for (i = 0; i < pps->num_tile_columns - 1; i++) {
                pps->column_width[i] = get_ue_golomb_long(gb) + 1;
                sum                 += pps->column_width[i];

            }
            if (sps && sum >= sps->ctb_width) {
                av_log(avctx, AV_LOG_ERROR, "Invalid tile widths.\n");
                ret = AVERROR_INVALIDDATA;
                goto err;
            }
            if(sps)
                pps->column_width[pps->num_tile_columns - 1] = sps->ctb_width - sum;

            sum = 0;
            for (i = 0; i < pps->num_tile_rows - 1; i++) {
                pps->row_height[i] = get_ue_golomb_long(gb) + 1;
                sum               += pps->row_height[i];
            }
            if (sps && sum >= sps->ctb_height) {
                av_log(avctx, AV_LOG_ERROR, "Invalid tile heights.\n");
                ret = AVERROR_INVALIDDATA;
                goto err;
            }
            if(sps)
                pps->row_height[pps->num_tile_rows - 1] = sps->ctb_height - sum;
        }
        pps->loop_filter_across_tiles_enabled_flag = get_bits1(gb);
    }
    pps->pps_loop_filter_across_slices_enabled_flag = get_bits1(gb);
    pps->deblocking_filter_control_present_flag     = get_bits1(gb);


    if (pps->deblocking_filter_control_present_flag) {
        pps->deblocking_filter_override_enabled_flag = get_bits1(gb);
        pps->pps_deblocking_filter_disabled_flag                             = get_bits1(gb);


        if (!pps->pps_deblocking_filter_disabled_flag) {
            pps->pps_beta_offset = get_se_golomb(gb) * 2;
            pps->pps_tc_offset   = get_se_golomb(gb) * 2;


            if (pps->pps_beta_offset/2 < -6 || pps->pps_beta_offset/2 > 6) {
                av_log(avctx, AV_LOG_ERROR, "pps_beta_offset_div2 out of range: %d\n",
                       pps->pps_beta_offset/2);
                ret = AVERROR_INVALIDDATA;
                goto err;
            }
            if (pps->pps_tc_offset/2 < -6 || pps->pps_tc_offset/2 > 6) {
                av_log(avctx, AV_LOG_ERROR, "pps_tc_offset_div2 out of range: %d\n",
                       pps->pps_tc_offset/2);
                ret = AVERROR_INVALIDDATA;
                goto err;
            }
        }
    }
    pps->pps_scaling_list_data_present_flag = get_bits1(gb);

    if (pps->pps_scaling_list_data_present_flag) {
        set_default_scaling_list_data(&pps->scaling_list);
        ret = scaling_list_data(gb, avctx, &pps->scaling_list, sps);
        if (ret < 0)
            goto err;
    }
    pps->lists_modification_present_flag = get_bits1(gb);
    pps->log2_parallel_merge_level       = get_ue_golomb_long(gb) + 2;


    if (sps && pps->log2_parallel_merge_level > sps->log2_ctb_size) {
        av_log(avctx, AV_LOG_ERROR, "log2_parallel_merge_level_minus2 out of range: %d\n",
               pps->log2_parallel_merge_level - 2);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }

    pps->slice_segment_header_extension_present_flag = get_bits1(gb);
    pps_extension_present_flag = get_bits1(gb);


//        av_log(avctx, AV_LOG_TRACE,
//               "Remaining bits PPS before extensions %d bits\n", get_bits_left(gb));

    if (pps_extension_present_flag) { // pps_extension_present_flag
        int pps_range_extensions_flag     = get_bits1(gb);
        int pps_multilayer_extension_flag = get_bits1(gb);
        /*int pps_extension_6bits           =*/ get_bits(gb, 6); // For next versions


        if (sps && sps->ptl.general_ptl.profile_idc == FF_PROFILE_HEVC_REXT && pps_range_extensions_flag) {
            av_log(avctx, AV_LOG_ERROR,
                   "PPS extension flag is partially implemented.\n");
            if ((ret = pps_range_extensions(gb, avctx, pps)) < 0)
                goto err;
        }
        if (pps_multilayer_extension_flag) {
             pps_multilayer_extensions(gb, avctx, pps);
        }
    }

    ret = setup_pps(avctx, pps, sps);
    if (ret < 0)
        goto err;

    if (get_bits_left(gb) < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Overread PPS by %d bits\n", -get_bits_left(gb));
      /*  goto err; */ /* TODO with  EXT_A_ericsson_4.bit */
    }

    remove_pps(ps, pps->pps_id);
    ps->pps_list[pps->pps_id] = pps_buf;

    return 0;

err:
    av_buffer_unref(&pps_buf);
    return ret;
}
