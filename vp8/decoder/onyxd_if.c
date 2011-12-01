/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */


#include "vp8/common/onyxc_int.h"
#if CONFIG_POSTPROC
#include "vp8/common/postproc.h"
#endif
#include "vp8/common/onyxd.h"
#include "onyxd_int.h"
#include "vpx_mem/vpx_mem.h"
#include "vp8/common/alloccommon.h"
#include "vpx_scale/yv12extend.h"
#include "vp8/common/loopfilter.h"
#include "vp8/common/swapyv12buffer.h"
#include "vp8/common/g_common.h"
#include "vp8/common/threading.h"
#include "decoderthreading.h"
#include <stdio.h>
#include <assert.h>

#include "vp8/common/quant_common.h"
#include "vpx_scale/vpxscale.h"
#include "vp8/common/systemdependent.h"
#include "vpx_ports/vpx_timer.h"
#include "detokenize.h"
#if CONFIG_ERROR_CONCEALMENT
#include "error_concealment.h"
#endif
#if ARCH_ARM
#include "vpx_ports/arm.h"
#endif

extern void vp8_init_loop_filter(VP8_COMMON *cm);
extern void vp8cx_init_de_quantizer(VP8D_COMP *pbi);
static int get_free_fb (VP8_COMMON *cm);
static void ref_cnt_fb (int *buf, int *idx, int new_idx);

#if CONFIG_DEBUG
void vp8_recon_write_yuv_frame(char *name, YV12_BUFFER_CONFIG *s)
{
    FILE *yuv_file = fopen((char *)name, "ab");
    unsigned char *src = s->y_buffer;
    int h = s->y_height;

    do
    {
        fwrite(src, s->y_width, 1,  yuv_file);
        src += s->y_stride;
    }
    while (--h);

    src = s->u_buffer;
    h = s->uv_height;

    do
    {
        fwrite(src, s->uv_width, 1,  yuv_file);
        src += s->uv_stride;
    }
    while (--h);

    src = s->v_buffer;
    h = s->uv_height;

    do
    {
        fwrite(src, s->uv_width, 1, yuv_file);
        src += s->uv_stride;
    }
    while (--h);

    fclose(yuv_file);
}
#endif
//#define WRITE_RECON_BUFFER 1
#if WRITE_RECON_BUFFER
void write_dx_frame_to_file(YV12_BUFFER_CONFIG *frame, int this_frame)
{

    // write the frame
    FILE *yframe;
    int i;
    char filename[255];

    sprintf(filename, "dx\\y%04d.raw", this_frame);
    yframe = fopen(filename, "wb");

    for (i = 0; i < frame->y_height; i++)
        fwrite(frame->y_buffer + i * frame->y_stride,
            frame->y_width, 1, yframe);

    fclose(yframe);
    sprintf(filename, "dx\\u%04d.raw", this_frame);
    yframe = fopen(filename, "wb");

    for (i = 0; i < frame->uv_height; i++)
        fwrite(frame->u_buffer + i * frame->uv_stride,
            frame->uv_width, 1, yframe);

    fclose(yframe);
    sprintf(filename, "dx\\v%04d.raw", this_frame);
    yframe = fopen(filename, "wb");

    for (i = 0; i < frame->uv_height; i++)
        fwrite(frame->v_buffer + i * frame->uv_stride,
            frame->uv_width, 1, yframe);

    fclose(yframe);
}
#endif



void vp8dx_initialize()
{
    static int init_done = 0;

    if (!init_done)
    {
        vp8_initialize_common();
#if CONFIG_EXTEND_QRANGE
        vp8_init_quant_tables();
#endif
        vp8_scale_machine_specific_config();
        init_done = 1;
    }
}

VP8D_PTR vp8dx_create_decompressor(VP8D_CONFIG *oxcf)
{
    VP8D_COMP *pbi = vpx_memalign(32, sizeof(VP8D_COMP));

    if (!pbi)
        return NULL;

    vpx_memset(pbi, 0, sizeof(VP8D_COMP));

    if (setjmp(pbi->common.error.jmp))
    {
        pbi->common.error.setjmp = 0;
        vp8dx_remove_decompressor(pbi);
        return 0;
    }

    pbi->common.error.setjmp = 1;
    vp8dx_initialize();

    vp8_create_common(&pbi->common);
    vp8_dmachine_specific_config(pbi);

    pbi->common.current_video_frame = 0;
    pbi->ready_for_new_data = 1;

#if CONFIG_MULTITHREAD
    pbi->max_threads = oxcf->max_threads;
    vp8_decoder_create_threads(pbi);
#endif

    /* vp8cx_init_de_quantizer() is first called here. Add check in frame_init_dequantizer() to avoid
     *  unnecessary calling of vp8cx_init_de_quantizer() for every frame.
     */
    vp8cx_init_de_quantizer(pbi);

    vp8_loop_filter_init(&pbi->common);

    pbi->common.error.setjmp = 0;

#if CONFIG_ERROR_CONCEALMENT
    pbi->ec_enabled = oxcf->error_concealment;
#else
    pbi->ec_enabled = 0;
#endif
    /* Error concealment is activated after a key frame has been
     * decoded without errors when error concealment is enabled.
     */
    pbi->ec_active = 0;

    pbi->decoded_key_frame = 0;

    pbi->input_partition = oxcf->input_partition;

    /* Independent partitions is activated when a frame updates the
     * token probability table to have equal probabilities over the
     * PREV_COEF context.
     */
    pbi->independent_partitions = 0;

    return (VP8D_PTR) pbi;
}

void vp8dx_remove_decompressor(VP8D_PTR ptr)
{
    VP8D_COMP *pbi = (VP8D_COMP *) ptr;

    if (!pbi)
        return;

    // Delete sementation map
    if (pbi->segmentation_map != 0)
        vpx_free(pbi->segmentation_map);

#if CONFIG_MULTITHREAD
    if (pbi->b_multithreaded_rd)
        vp8mt_de_alloc_temp_buffers(pbi, pbi->common.mb_rows);
    vp8_decoder_remove_threads(pbi);
#endif
#if CONFIG_ERROR_CONCEALMENT
    vp8_de_alloc_overlap_lists(pbi);
#endif
    vp8_remove_common(&pbi->common);
    vpx_free(pbi->mbc);
    vpx_free(pbi);
}


vpx_codec_err_t vp8dx_get_reference(VP8D_PTR ptr, VP8_REFFRAME ref_frame_flag, YV12_BUFFER_CONFIG *sd)
{
    VP8D_COMP *pbi = (VP8D_COMP *) ptr;
    VP8_COMMON *cm = &pbi->common;
    int ref_fb_idx;

    if (ref_frame_flag == VP8_LAST_FLAG)
        ref_fb_idx = cm->lst_fb_idx;
    else if (ref_frame_flag == VP8_GOLD_FLAG)
        ref_fb_idx = cm->gld_fb_idx;
    else if (ref_frame_flag == VP8_ALT_FLAG)
        ref_fb_idx = cm->alt_fb_idx;
    else{
        vpx_internal_error(&pbi->common.error, VPX_CODEC_ERROR,
            "Invalid reference frame");
        return pbi->common.error.error_code;
    }

    if(cm->yv12_fb[ref_fb_idx].y_height != sd->y_height ||
        cm->yv12_fb[ref_fb_idx].y_width != sd->y_width ||
        cm->yv12_fb[ref_fb_idx].uv_height != sd->uv_height ||
        cm->yv12_fb[ref_fb_idx].uv_width != sd->uv_width){
        vpx_internal_error(&pbi->common.error, VPX_CODEC_ERROR,
            "Incorrect buffer dimensions");
    }
    else
        vp8_yv12_copy_frame_ptr(&cm->yv12_fb[ref_fb_idx], sd);

    return pbi->common.error.error_code;
}


vpx_codec_err_t vp8dx_set_reference(VP8D_PTR ptr, VP8_REFFRAME ref_frame_flag, YV12_BUFFER_CONFIG *sd)
{
    VP8D_COMP *pbi = (VP8D_COMP *) ptr;
    VP8_COMMON *cm = &pbi->common;
    int *ref_fb_ptr = NULL;
    int free_fb;

    if (ref_frame_flag == VP8_LAST_FLAG)
        ref_fb_ptr = &cm->lst_fb_idx;
    else if (ref_frame_flag == VP8_GOLD_FLAG)
        ref_fb_ptr = &cm->gld_fb_idx;
    else if (ref_frame_flag == VP8_ALT_FLAG)
        ref_fb_ptr = &cm->alt_fb_idx;
    else{
        vpx_internal_error(&pbi->common.error, VPX_CODEC_ERROR,
            "Invalid reference frame");
        return pbi->common.error.error_code;
    }

    if(cm->yv12_fb[*ref_fb_ptr].y_height != sd->y_height ||
        cm->yv12_fb[*ref_fb_ptr].y_width != sd->y_width ||
        cm->yv12_fb[*ref_fb_ptr].uv_height != sd->uv_height ||
        cm->yv12_fb[*ref_fb_ptr].uv_width != sd->uv_width){
        vpx_internal_error(&pbi->common.error, VPX_CODEC_ERROR,
            "Incorrect buffer dimensions");
    }
    else{
        /* Find an empty frame buffer. */
        free_fb = get_free_fb(cm);
        /* Decrease fb_idx_ref_cnt since it will be increased again in
         * ref_cnt_fb() below. */
        cm->fb_idx_ref_cnt[free_fb]--;

        /* Manage the reference counters and copy image. */
        ref_cnt_fb (cm->fb_idx_ref_cnt, ref_fb_ptr, free_fb);
        vp8_yv12_copy_frame_ptr(sd, &cm->yv12_fb[*ref_fb_ptr]);
    }

   return pbi->common.error.error_code;
}

/*For ARM NEON, d8-d15 are callee-saved registers, and need to be saved by us.*/
#if HAVE_ARMV7
extern void vp8_push_neon(int64_t *store);
extern void vp8_pop_neon(int64_t *store);
#endif

static int get_free_fb (VP8_COMMON *cm)
{
    int i;
    for (i = 0; i < NUM_YV12_BUFFERS; i++)
        if (cm->fb_idx_ref_cnt[i] == 0)
            break;

    assert(i < NUM_YV12_BUFFERS);
    cm->fb_idx_ref_cnt[i] = 1;
    return i;
}

static void ref_cnt_fb (int *buf, int *idx, int new_idx)
{
    if (buf[*idx] > 0)
        buf[*idx]--;

    *idx = new_idx;

    buf[new_idx]++;
}

/* If any buffer copy / swapping is signalled it should be done here. */
static int swap_frame_buffers (VP8_COMMON *cm)
{
    int err = 0;

    /* The alternate reference frame or golden frame can be updated
     *  using the new, last, or golden/alt ref frame.  If it
     *  is updated using the newly decoded frame it is a refresh.
     *  An update using the last or golden/alt ref frame is a copy.
     */
    if (cm->copy_buffer_to_arf)
    {
        int new_fb = 0;

        if (cm->copy_buffer_to_arf == 1)
            new_fb = cm->lst_fb_idx;
        else if (cm->copy_buffer_to_arf == 2)
            new_fb = cm->gld_fb_idx;
        else
            err = -1;

        ref_cnt_fb (cm->fb_idx_ref_cnt, &cm->alt_fb_idx, new_fb);
    }

    if (cm->copy_buffer_to_gf)
    {
        int new_fb = 0;

        if (cm->copy_buffer_to_gf == 1)
            new_fb = cm->lst_fb_idx;
        else if (cm->copy_buffer_to_gf == 2)
            new_fb = cm->alt_fb_idx;
        else
            err = -1;

        ref_cnt_fb (cm->fb_idx_ref_cnt, &cm->gld_fb_idx, new_fb);
    }

    if (cm->refresh_golden_frame)
        ref_cnt_fb (cm->fb_idx_ref_cnt, &cm->gld_fb_idx, cm->new_fb_idx);

    if (cm->refresh_alt_ref_frame)
        ref_cnt_fb (cm->fb_idx_ref_cnt, &cm->alt_fb_idx, cm->new_fb_idx);

    if (cm->refresh_last_frame)
    {
        ref_cnt_fb (cm->fb_idx_ref_cnt, &cm->lst_fb_idx, cm->new_fb_idx);

        cm->frame_to_show = &cm->yv12_fb[cm->lst_fb_idx];
    }
    else
        cm->frame_to_show = &cm->yv12_fb[cm->new_fb_idx];

    cm->fb_idx_ref_cnt[cm->new_fb_idx]--;

    return err;
}

/*
static void vp8_print_yuv_rec_mb(VP8_COMMON *cm, int mb_row, int mb_col)
{
  YV12_BUFFER_CONFIG *s = cm->frame_to_show;
  unsigned char *src = s->y_buffer;
  int i, j;

  printf("After loop filter\n");
  for (i=0;i<16;i++) {
    for (j=0;j<16;j++)
      printf("%3d ", src[(mb_row*16+i)*s->y_stride + mb_col*16+j]);
    printf("\n");
  }
}
*/

int vp8dx_receive_compressed_data(VP8D_PTR ptr, unsigned long size, const unsigned char *source, int64_t time_stamp)
{
#if HAVE_ARMV7
    int64_t dx_store_reg[8];
#endif
    VP8D_COMP *pbi = (VP8D_COMP *) ptr;
    VP8_COMMON *cm = &pbi->common;
    int retcode = 0;

    /*if(pbi->ready_for_new_data == 0)
        return -1;*/

    if (ptr == 0)
    {
        return -1;
    }

    pbi->common.error.error_code = VPX_CODEC_OK;

    if (pbi->input_partition && !(source == NULL && size == 0))
    {
        /* Store a pointer to this partition and return. We haven't
         * received the complete frame yet, so we will wait with decoding.
         */
        assert(pbi->num_partitions < MAX_PARTITIONS);
        pbi->partitions[pbi->num_partitions] = source;
        pbi->partition_sizes[pbi->num_partitions] = size;
        pbi->source_sz += size;
        pbi->num_partitions++;
        if (pbi->num_partitions > (1 << EIGHT_PARTITION) + 1)
        {
            pbi->common.error.error_code = VPX_CODEC_UNSUP_BITSTREAM;
            pbi->common.error.setjmp = 0;
            pbi->num_partitions = 0;
            return -1;
        }
        return 0;
    }
    else
    {
        if (!pbi->input_partition)
        {
            pbi->Source = source;
            pbi->source_sz = size;
        }
        else
        {
            assert(pbi->common.multi_token_partition <= EIGHT_PARTITION);
            if (pbi->num_partitions == 0)
            {
                pbi->num_partitions = 1;
                pbi->partitions[0] = NULL;
                pbi->partition_sizes[0] = 0;
            }
            while (pbi->num_partitions < (1 << pbi->common.multi_token_partition) + 1)
            {
                // Reset all missing partitions
                pbi->partitions[pbi->num_partitions] =
                    pbi->partitions[pbi->num_partitions - 1] +
                    pbi->partition_sizes[pbi->num_partitions - 1];
                pbi->partition_sizes[pbi->num_partitions] = 0;
                pbi->num_partitions++;
            }
        }

        if (pbi->source_sz == 0)
        {
           /* This is used to signal that we are missing frames.
            * We do not know if the missing frame(s) was supposed to update
            * any of the reference buffers, but we act conservative and
            * mark only the last buffer as corrupted.
            */
            cm->yv12_fb[cm->lst_fb_idx].corrupted = 1;

            /* If error concealment is disabled we won't signal missing frames to
             * the decoder.
             */
            if (!pbi->ec_active)
            {
                /* Signal that we have no frame to show. */
                cm->show_frame = 0;

                pbi->num_partitions = 0;

                /* Nothing more to do. */
                return 0;
            }
        }

#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
        if (cm->rtcd.flags & HAS_NEON)
#endif
        {
            vp8_push_neon(dx_store_reg);
        }
#endif

        cm->new_fb_idx = get_free_fb (cm);

        if (setjmp(pbi->common.error.jmp))
        {
#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
            if (cm->rtcd.flags & HAS_NEON)
#endif
            {
                vp8_pop_neon(dx_store_reg);
            }
#endif
            pbi->common.error.setjmp = 0;

            pbi->num_partitions = 0;

           /* We do not know if the missing frame(s) was supposed to update
            * any of the reference buffers, but we act conservative and
            * mark only the last buffer as corrupted.
            */
            cm->yv12_fb[cm->lst_fb_idx].corrupted = 1;

            if (cm->fb_idx_ref_cnt[cm->new_fb_idx] > 0)
              cm->fb_idx_ref_cnt[cm->new_fb_idx]--;
            return -1;
        }

        pbi->common.error.setjmp = 1;
    }

    retcode = vp8_decode_frame(pbi);

    if (retcode < 0)
    {
#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
        if (cm->rtcd.flags & HAS_NEON)
#endif
        {
            vp8_pop_neon(dx_store_reg);
        }
#endif
        pbi->common.error.error_code = VPX_CODEC_ERROR;
        pbi->common.error.setjmp = 0;
        pbi->num_partitions = 0;
        if (cm->fb_idx_ref_cnt[cm->new_fb_idx] > 0)
          cm->fb_idx_ref_cnt[cm->new_fb_idx]--;
        return retcode;
    }

#if CONFIG_MULTITHREAD
    if (pbi->b_multithreaded_rd && cm->multi_token_partition != ONE_PARTITION)
    {
        if (swap_frame_buffers (cm))
        {
#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
            if (cm->rtcd.flags & HAS_NEON)
#endif
            {
                vp8_pop_neon(dx_store_reg);
            }
#endif
            pbi->common.error.error_code = VPX_CODEC_ERROR;
            pbi->common.error.setjmp = 0;
            pbi->num_partitions = 0;
            return -1;
        }
    } else
#endif
    {
        if (swap_frame_buffers (cm))
        {
#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
            if (cm->rtcd.flags & HAS_NEON)
#endif
            {
                vp8_pop_neon(dx_store_reg);
            }
#endif
            pbi->common.error.error_code = VPX_CODEC_ERROR;
            pbi->common.error.setjmp = 0;
            pbi->num_partitions = 0;
            return -1;
        }

#if WRITE_RECON_BUFFER
        if(cm->show_frame)
<<<<<<< HEAD
            write_dx_frame_to_file(cm->frame_to_show, cm->current_video_frame);
        else
            write_dx_frame_to_file(cm->frame_to_show, cm->current_video_frame+1000);
=======
            write_dx_frame_to_file(cm->frame_to_show,
                cm->current_video_frame);
        else
            write_dx_frame_to_file(cm->frame_to_show,
                cm->current_video_frame+1000);
>>>>>>> added separate entropy context for alt_ref
#endif

        if(cm->filter_level)
        {
            /* Apply the loop filter if appropriate. */
            vp8_loop_filter_frame(cm, &pbi->mb);
        }
        vp8_yv12_extend_frame_borders_ptr(cm->frame_to_show);
    }

#if CONFIG_DEBUG
    vp8_recon_write_yuv_frame("recon.yuv", cm->frame_to_show);
#endif

    vp8_clear_system_state();

#if CONFIG_ERROR_CONCEALMENT
    /* swap the mode infos to storage for future error concealment */
    if (pbi->ec_enabled && pbi->common.prev_mi)
    {
        const MODE_INFO* tmp = pbi->common.prev_mi;
        int row, col;
        pbi->common.prev_mi = pbi->common.mi;
        pbi->common.mi = tmp;

        /* Propagate the segment_ids to the next frame */
        for (row = 0; row < pbi->common.mb_rows; ++row)
        {
            for (col = 0; col < pbi->common.mb_cols; ++col)
            {
                const int i = row*pbi->common.mode_info_stride + col;
                pbi->common.mi[i].mbmi.segment_id =
                        pbi->common.prev_mi[i].mbmi.segment_id;
            }
        }
    }
#endif

#if CONFIG_NEWNEAR
    if(cm->show_frame)
    {
        vpx_memcpy(cm->prev_mip, cm->mip,
            (cm->mb_cols + 1) * (cm->mb_rows + 1)* sizeof(MODE_INFO));
    }
    else
    {
        vpx_memset(cm->prev_mip, 0,
            (cm->mb_cols + 1) * (cm->mb_rows + 1)* sizeof(MODE_INFO));
    }
#endif

    /*vp8_print_modes_and_motion_vectors( cm->mi, cm->mb_rows,cm->mb_cols, cm->current_video_frame);*/

    if (cm->show_frame)
        cm->current_video_frame++;

    pbi->ready_for_new_data = 0;
    pbi->last_time_stamp = time_stamp;
    pbi->num_partitions = 0;
    pbi->source_sz = 0;

#if 0
    {
        int i;
        int64_t earliest_time = pbi->dr[0].time_stamp;
        int64_t latest_time = pbi->dr[0].time_stamp;
        int64_t time_diff = 0;
        int bytes = 0;

        pbi->dr[pbi->common.current_video_frame&0xf].size = pbi->bc.pos + pbi->bc2.pos + 4;;
        pbi->dr[pbi->common.current_video_frame&0xf].time_stamp = time_stamp;

        for (i = 0; i < 16; i++)
        {

            bytes += pbi->dr[i].size;

            if (pbi->dr[i].time_stamp < earliest_time)
                earliest_time = pbi->dr[i].time_stamp;

            if (pbi->dr[i].time_stamp > latest_time)
                latest_time = pbi->dr[i].time_stamp;
        }

        time_diff = latest_time - earliest_time;

        if (time_diff > 0)
        {
            pbi->common.bitrate = 80000.00 * bytes / time_diff  ;
            pbi->common.framerate = 160000000.00 / time_diff ;
        }

    }
#endif

#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
    if (cm->rtcd.flags & HAS_NEON)
#endif
    {
        vp8_pop_neon(dx_store_reg);
    }
#endif
    pbi->common.error.setjmp = 0;
    return retcode;
}
int vp8dx_get_raw_frame(VP8D_PTR ptr, YV12_BUFFER_CONFIG *sd, int64_t *time_stamp, int64_t *time_end_stamp, vp8_ppflags_t *flags)
{
    int ret = -1;
    VP8D_COMP *pbi = (VP8D_COMP *) ptr;

    if (pbi->ready_for_new_data == 1)
        return ret;

    /* ie no raw frame to show!!! */
    if (pbi->common.show_frame == 0)
        return ret;

    pbi->ready_for_new_data = 1;
    *time_stamp = pbi->last_time_stamp;
    *time_end_stamp = 0;

    sd->clrtype = pbi->common.clr_type;
#if CONFIG_POSTPROC
    ret = vp8_post_proc_frame(&pbi->common, sd, flags);
#else

    if (pbi->common.frame_to_show)
    {
        *sd = *pbi->common.frame_to_show;
        sd->y_width = pbi->common.Width;
        sd->y_height = pbi->common.Height;
        sd->uv_height = pbi->common.Height / 2;
        ret = 0;
    }
    else
    {
        ret = -1;
    }

#endif /*!CONFIG_POSTPROC*/
    vp8_clear_system_state();
    return ret;
}
