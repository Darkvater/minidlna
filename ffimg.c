/* MiniDLNA media server
 * Copyright (C) 2017  Edmunt Pienkowsky
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "log.h"
#include "libav.h"
#include "ffimg.h"


/*
	Based on http://jpegclub.org/exif_orientation.html
*/
static const char *_get_filter_from_orientation(const AVFrame* frame, int *dimensions_swapped)
{
	AVDictionary *dict;
	AVDictionaryEntry *oentry;
	int orientation;

	if (!(dict = av_frame_get_metadata(frame))) return NULL;
	if (!(oentry = av_dict_get(dict, "Orientation", NULL, 0))) return NULL;
	switch ((orientation = atoi(oentry->value)))
	{
	case 2:
		*dimensions_swapped = 0;
		return "hflip";

	case 3:
		*dimensions_swapped = 0;
		return "hflip,vflip";

	case 4:
		*dimensions_swapped = 0;
		return "vflip";

	case 5:
		*dimensions_swapped = 1;
		return "transpose=0";

	case 6:
		*dimensions_swapped = 1;
		return "transpose=1";

	case 7:
		*dimensions_swapped = 1;
		return "transpose=3";

	case 8:
		*dimensions_swapped = 1;
		return "transpose=2";

	default:
		return NULL;
	}
}

static int _get_density(const AVFrame *frame, int *XResolution, int *YResolution, int *ResolutionUnit)
{
	AVDictionary *dict;
	AVDictionaryEntry *e;
	int res = 0;

	if (!(dict = av_frame_get_metadata(frame)))
	{
		return 0;
	}

	if ((e = av_dict_get(dict, "XResolution", NULL, 0)))
	{
		*XResolution = atoi(e->value);
		res += 1;
	}

	if ((e = av_dict_get(dict, "YResolution", NULL, 0)))
        {
                *YResolution = atoi(e->value);
                res += 1;
        }

	if ((e = av_dict_get(dict, "ResolutionUnit", NULL, 0)))
        {
                switch((*ResolutionUnit = atoi(e->value)))
		{
			case 2:
			case 3:
	                res += 1;
			break;
		}
        }

	return (res == 3);
}

static inline void _fill_ofilter(char *dst, size_t dst_size, size_t *len, const AVFrame *frame)
{
	int dimensions_swapped;
	const char *ofilter;

	if (!(ofilter = _get_filter_from_orientation(frame, &dimensions_swapped))) return;

	*len += snprintf(dst + *len, dst_size - *len, "%s,", ofilter);
}

static inline void _fill_scale_filter(char *dst, size_t dst_size, size_t *len, int width, int height)
{
	if (width == -1 && height == -1) return;

	if (width != -1 && height != -1)
	{
		*len += snprintf(dst + *len, dst_size - *len, "scale=width=%d:height=%d:interl=-1:force_original_aspect_ratio=decrease,", width, height);
	}
	else
	{
		*len += snprintf(dst + *len, dst_size - *len, "scale=width=%d:height=%d:interl=-1,", width, height);
	}
}

static inline void _fill_dst_pix_fmts(char *dst, size_t dst_size, size_t *len, const AVCodec *codec)
{
	const enum AVPixelFormat *i;

	if (!codec->pix_fmts) return;
	*len += snprintf(dst + *len, dst_size - *len, "format=");
	for (i = codec->pix_fmts; (*i != AV_PIX_FMT_NONE) && (*len < dst_size); ++i)
	{
		if (i != codec->pix_fmts)
		{
			dst[*len] = '|';
			*len += 1;
		}
		*len += snprintf(dst + *len, dst_size - *len, "%d", (int)(*i));
	}
}

static AVFilterGraph* _create_filter_graph(const AVFrame *frame, int width, int height, const AVCodec *dst_codec, AVFilterContext **src, AVFilterContext **sink)
{
	AVFilterGraph *filter_graph;
	AVFilter *buffersrc, *buffersink;
	AVFilterInOut *outputs = NULL, *inputs = NULL;
	AVFilterContext *buffersrc_ctx, *buffersink_ctx;
	char args[512];
	int err;

	buffersrc = avfilter_get_by_name("buffer");
	buffersink = avfilter_get_by_name("buffersink");

	outputs = avfilter_inout_alloc();
	inputs = avfilter_inout_alloc();

	if (!(filter_graph = avfilter_graph_alloc())) {
		DPRINTF(E_DEBUG, L_METADATA, "unable to create filter graph: out of memory\n");
		return 0;
	}

	snprintf(args, sizeof(args),
		"width=%d:height=%d:pix_fmt=%d:time_base=1/1:pixel_aspect=%d/%d",
		frame->width, frame->height, frame->format, frame->sample_aspect_ratio.num, frame->sample_aspect_ratio.den);

	if ((err = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph)) < 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Cannot create buffer source\n");
		goto end;
	}

	if ((err = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph)) < 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Cannot create buffer sink\n");
		goto end;
	}

	outputs->name = av_strdup("in");
	outputs->filter_ctx = buffersrc_ctx;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = NULL;

	{
		size_t len = 0;

		args[0] = '\0';
		_fill_ofilter(args, sizeof(args), &len, frame);
		_fill_scale_filter(args, sizeof(args), &len, width, height);
		_fill_dst_pix_fmts(args, sizeof(args), &len, dst_codec);
	}

	if ((err = avfilter_graph_parse_ptr(filter_graph, args, &inputs, &outputs, NULL)) < 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Failed to parse graph string [%s]\n", args);
		goto end;
	}

	if ((err = avfilter_graph_config(filter_graph, NULL)) < 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Failed to validate filter graph.\n");
		goto end;
	}

end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	if (err < 0)
	{
		avfilter_graph_free(&filter_graph);
		return NULL;
	}
	else
	{
		*src = buffersrc_ctx;
		*sink = buffersink_ctx;

		return filter_graph;
	}
}

static void _dump_frame_metadata(const AVFrame* frame)
{
	AVDictionary *dict;
	if ((dict = av_frame_get_metadata(frame)))
	{
		AVDictionaryEntry *t = NULL;
		while ((t = av_dict_get(dict, "", t, AV_DICT_IGNORE_SUFFIX)))
		{
			DPRINTF(E_DEBUG, L_METADATA, "FF Metadata: %s -> %s\n", t->key, t->value);
		}
	}
}

static ffimg_t *_decode_frame(AVFormatContext *format_ctx, const AVCodec *codec)
{
	AVCodecContext *codec_ctx;
	ffimg_t *img;
	int err;

	if (!(img = ffimg_alloc()))
	{
		DPRINTF(E_DEBUG, L_METADATA, "Failed to allocate ffimg\n");
		return 0;
	}

	if (!(codec_ctx = avcodec_alloc_context3(codec)))
	{
		DPRINTF(E_DEBUG, L_METADATA,  "Failed to allocate codec context\n");
		ffimg_free(img);
		return 0;
	}

	if ((err = avcodec_open2(codec_ctx, codec, NULL)))
	{
		DPRINTF(E_DEBUG, L_METADATA, "Failed to open image codec\n");
		ffimg_free(img);
		avcodec_free_context(&codec_ctx);
		return 0;
	}

	if (!(err = av_read_frame(format_ctx, img->packet)))
	{
		if (!(err = avcodec_send_packet(codec_ctx, img->packet)))
		{
			if (!(err = avcodec_receive_frame(codec_ctx, img->frame)))
			{
				_dump_frame_metadata(img->frame);
				avcodec_close(codec_ctx);
				avcodec_free_context(&codec_ctx);
				img->id = codec->id;
				return img;
			}
		}
	}

	avcodec_close(codec_ctx);
	avcodec_free_context(&codec_ctx);
	ffimg_free(img);
	return NULL;
}

void ffimg_free(ffimg_t *img)
{
	if (!img) return;
	if (img->frame) av_frame_free(&img->frame);
	if (img->packet) av_packet_free(&img->packet);
	free(img);
}

int ffimg_is_valid(const ffimg_t *img)
{
	return img->packet && img->frame;
}

ffimg_t *ffimg_alloc()
{
	ffimg_t *i;

	if ((i = calloc(1, sizeof(ffimg_t))))
	{
		i->frame = av_frame_alloc();
		i->packet = av_packet_alloc();

		if (!ffimg_is_valid(i))
		{
			ffimg_free(i);
			i = NULL;
		}
	}
	return i;
}

ffimg_t *ffimg_load_from_file(const char *imgpath)
{
	AVInputFormat *iformat = NULL;
	AVFormatContext *format_ctx = NULL;
	AVCodec *codec = NULL;
	ffimg_t *img = NULL;
	int err;

	if (!(iformat = av_find_input_format("image2")))
	{
		DPRINTF(E_ERROR, L_METADATA, "load_from_file: Unable to find image2 input format\n");
		goto done;
	}

	if ((err = avformat_open_input(&format_ctx, imgpath, iformat, NULL)) < 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Unable to open input file\n");
		goto done;
	}

	if ((err = avformat_find_stream_info(format_ctx, NULL)) < 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Unable to find stream info\n");
		goto done;
	}

	if ((err = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Unable to find required stream\n");
		goto done;
	}

	img = _decode_frame(format_ctx, codec);

done:
	avformat_free_context(format_ctx);
	return img;
}

ffimg_t *ffimg_load_from_blob(const void *data, size_t data_size)
{
	void *av_data = NULL;
	AVFormatContext *format_ctx = NULL;
	AVCodec *codec = NULL;
	ffimg_t *img = NULL;
	int err;

	if (!(av_data = av_malloc(data_size)))
	{
		DPRINTF(E_DEBUG, L_METADATA, "Unable to allocate buffer\n");
		return 0;
	}

	memcpy(av_data, data, data_size);

	if (!(format_ctx = avformat_alloc_context()))
	{
		DPRINTF(E_DEBUG, L_METADATA, "Unable to allocate AVFormatContext\n");
		av_free(av_data);
		return 0;
	}

	if (!(format_ctx->pb = avio_alloc_context(av_data, (int)data_size, 0, NULL, NULL, NULL, NULL)))
	{
		av_free(av_data);
		avformat_free_context(format_ctx);
		return 0;
	}

	if ((err = avformat_open_input(&format_ctx, NULL, NULL, NULL)) < 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Unable to open input stream\n");
		goto done;
	}

	if ((err = avformat_find_stream_info(format_ctx, NULL)) < 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Unable to find stream info\n");
		goto done;
	}

	if ((err = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Unable to find required stream\n");
		goto done;
	}

	img = _decode_frame(format_ctx, codec);

done:
	if (format_ctx)
	{
		av_free(format_ctx->pb->buffer);
		av_free(format_ctx->pb);
		format_ctx->pb = NULL;
		avformat_free_context(format_ctx);
	}
	return img;
}

ffimg_t *ffimg_resize(const ffimg_t *img, int width, int height, int to_jpeg)
{
	AVCodecContext *encoder_codec_ctx;
	AVCodec *encoder_codec;
	ffimg_t *dst_img = NULL;
	int err, frame_filtered = 0, frame_encoded = 0;
	AVFilterGraph *filter_graph = NULL;
	AVFilterContext *src_ctx = NULL;
	AVFilterContext *sink_ctx = NULL;
	AVDictionary *enc_options = NULL;

	if (!(dst_img = ffimg_alloc()))
	{
		return NULL;
	}

	if (!(encoder_codec = avcodec_find_encoder_by_name(to_jpeg ? "mjpeg" : "png")))
	{
		DPRINTF(E_ERROR, L_METADATA, "resize: Couldn't find a encoder\n");
		return NULL;
	}

	if (!(filter_graph = _create_filter_graph(img->frame, width, height, encoder_codec, &src_ctx, &sink_ctx)))
	{
		ffimg_free(dst_img);
		return NULL;
	}

	if (!(err = av_buffersrc_add_frame(src_ctx, img->frame)))
	{
		if (!(err = av_buffersink_get_frame(sink_ctx, dst_img->frame)))
		{
			frame_filtered = 1;
		}
	}

	avfilter_graph_free(&filter_graph);

	if (!frame_filtered)
	{
		ffimg_free(dst_img);
		return NULL;
	}

	err = av_frame_copy_props(dst_img->frame, img->frame);

	if (!(encoder_codec_ctx = avcodec_alloc_context3(encoder_codec)))
	{
		DPRINTF(E_DEBUG, L_METADATA, "Failed to allocate the encoder codec context\n");
		ffimg_free(dst_img);
		return NULL;
	}

	encoder_codec_ctx->width = dst_img->frame->width;
	encoder_codec_ctx->height = dst_img->frame->height;
	encoder_codec_ctx->time_base.num = 1;
	encoder_codec_ctx->time_base.den = 1;
	encoder_codec_ctx->pix_fmt = dst_img->frame->format;
	if (to_jpeg)
	{
		encoder_codec_ctx->qmin = 2;
		encoder_codec_ctx->qmin = 4;
		encoder_codec_ctx->mb_lmin = encoder_codec_ctx->qmin * FF_QP2LAMBDA;
		encoder_codec_ctx->mb_lmax = encoder_codec_ctx->qmax * FF_QP2LAMBDA;
		encoder_codec_ctx->flags |= CODEC_FLAG_QSCALE;
		encoder_codec_ctx->global_quality = encoder_codec_ctx->qmin * FF_QP2LAMBDA;
		dst_img->frame->quality = encoder_codec_ctx->global_quality;
		//dst_frame->quality = (int)1 + FF_LAMBDA_MAX * ( (100-quality)/100.0 ); 
		//dst_frame->quality = (int)1 + 4096 * ((100 - quality) / 100.0);
		av_dict_set(&enc_options, "huffman", "optimal", 0);
	}
	else
	{
		int XResolution = -1, YResolution = -1, ResolutionUnit = -1;

		encoder_codec_ctx->compression_level = 9;
		encoder_codec_ctx->flags |= AV_CODEC_FLAG_INTERLACED_DCT; // progresive
		av_dict_set(&enc_options, "pred", "none", 0);

		if (_get_density(img->frame, &XResolution, &YResolution, &ResolutionUnit) && (XResolution == YResolution))
		{
			switch(ResolutionUnit)
			{
				case 2: // inch
				av_dict_set_int(&enc_options, "dpi", XResolution, 0);
				break;

				case 3: // cm
				av_dict_set_int(&enc_options, "dpm", XResolution*100, 0);
				break;
			}
		}
	}

	if (avcodec_open2(encoder_codec_ctx, encoder_codec, &enc_options) < 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Failed to open the encoder\n");
		av_dict_free(&enc_options);
		avcodec_free_context(&encoder_codec_ctx);
		ffimg_free(dst_img);
		return NULL;
	}

	if (!(err = avcodec_send_frame(encoder_codec_ctx, dst_img->frame)))
	{
		if (!(err = avcodec_receive_packet(encoder_codec_ctx, dst_img->packet)))
		{
			dst_img->id = encoder_codec->id;
			frame_encoded = 1;
		}
	}

	av_dict_free(&enc_options);
	avcodec_close(encoder_codec_ctx);
	avcodec_free_context(&encoder_codec_ctx);
	if (frame_encoded)
	{
		return dst_img;
	}
	else
	{
		ffimg_free(dst_img);
		return NULL;
	}
}

ffimg_t *ffimg_clone(const ffimg_t *img)
{
	ffimg_t *res;
	if (!img) return NULL;

	if (!(res = calloc(1,sizeof(ffimg_t)))) return NULL;

	res->packet = av_packet_clone(img->packet);
	res->frame = av_frame_clone(img->frame);
	res->id = img->id;

	if (ffimg_is_valid(res))
	{
		return res;
	}
	else
	{
		ffimg_free(res);
		return NULL;
	}
}

/*
	JPEG for now
 */
int ffimg_is_supported(const ffimg_t *img)
{
	return img->id == AV_CODEC_ID_MJPEG;
}

void ffimg_get_dimensions(const ffimg_t *img, int *width, int *height)
{
	int dimensions_swapped;
	const char *ofilter;

	if (!(ofilter = _get_filter_from_orientation(img->frame, &dimensions_swapped)))
	{
		dimensions_swapped = 0;
	}
	*width = dimensions_swapped? img->frame->height : img->frame->width;
	*height = dimensions_swapped? img->frame->width : img->frame->height;
}
