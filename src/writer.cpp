// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp

#include "movie.h"
#include <vector>

using namespace std;

MovieWriter::MovieWriter(const string& filename_, const unsigned int width_, const unsigned int height_) :
width(width_), height(height_), iframe(0), pixels(4 * width * height)

{
	// Preparing to convert my generated RGB images to YUV frames.
	swsCtx = sws_getContext(width, height,
		AV_PIX_FMT_RGB24, width, height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL);

	// Preparing the data concerning the format and codec,
	// in order to write properly the header, frame data and end of file.
	const char* fmtext = "mp4";
	const string filename = filename_ + "." + fmtext;
	fmt = av_guess_format(fmtext, NULL, NULL);
	avformat_alloc_output_context2(&fc, NULL, NULL, filename.c_str());

	// Setting up the codec.
	AVCodec* codec = avcodec_find_encoder_by_name("libx264");
	AVDictionary* opt = NULL;
	av_dict_set(&opt, "preset", "slow", 0);
	av_dict_set(&opt, "crf", "20", 0);
	stream = avformat_new_stream(fc, codec);
	c = stream->codec;
	c->width = width;
	c->height = height;
	c->pix_fmt = AV_PIX_FMT_YUV420P;
	c->time_base = (AVRational){ 1, 25 };

	// Setting up the format, its stream(s),
	// linking with the codec(s) and write the header.
	if (fc->oformat->flags & AVFMT_GLOBALHEADER)
	{
		// Some formats require a global header.
		c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	avcodec_open2(c, codec, &opt);
	av_dict_free(&opt);

	// Once the codec is set up, we need to let the container know
	// which codec are the streams using, in this case the only (video) stream.
	stream->time_base = (AVRational){ 1, 25 };
	av_dump_format(fc, 0, filename.c_str(), 1);
	avio_open(&fc->pb, filename.c_str(), AVIO_FLAG_WRITE);
	int ret = avformat_write_header(fc, &opt);
	av_dict_free(&opt); 

	// Preparing the containers of the frame data:
	// Allocating memory for each RGB frame, which will be lately converted to YUV.
	rgbpic = av_frame_alloc();
	rgbpic->format = AV_PIX_FMT_RGB24;
	rgbpic->width = width;
	rgbpic->height = height;
	ret = av_frame_get_buffer(rgbpic, 1);

	// Allocating memory for each conversion output YUV frame.
	yuvpic = av_frame_alloc();
	yuvpic->format = AV_PIX_FMT_YUV420P;
	yuvpic->width = width;
	yuvpic->height = height;
	ret = av_frame_get_buffer(yuvpic, 1);

	// After the format, code and general frame data is set,
	// we can write the video in the frame generation loop:
	// std::vector<uint8_t> B(width*height*3);
}

void MovieWriter::addFrame(const uint8_t* pixels)
{
	// The AVFrame data will be stored as RGBRGBRGB... row-wise,
	// from left to right and from top to bottom.
	for (unsigned int y = 0; y < height; y++)
	{
    	for (unsigned int x = 0; x < width; x++)
    	{
        	// rgbpic->linesize[0] is equal to width.
			rgbpic->data[0][y * rgbpic->linesize[0] + 3 * x + 0] = pixels[y * 4 * width + 4 * x + 2];
			rgbpic->data[0][y * rgbpic->linesize[0] + 3 * x + 1] = pixels[y * 4 * width + 4 * x + 1];
			rgbpic->data[0][y * rgbpic->linesize[0] + 3 * x + 2] = pixels[y * 4 * width + 4 * x + 0];
		}
	}

	// Not actually scaling anything, but just converting
	// the RGB data to YUV and store it in yuvpic.
    sws_scale(swsCtx, rgbpic->data, rgbpic->linesize, 0,
    	height, yuvpic->data, yuvpic->linesize);

	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	// The PTS of the frame are just in a reference unit,
	// unrelated to the format we are using. We set them,
	// for instance, as the corresponding frame number.
	yuvpic->pts = iframe;

	int got_output;
	int ret = avcodec_encode_video2(c, &pkt, yuvpic, &got_output);
	if (got_output)
	{
		fflush(stdout);

		// We set the packet PTS and DTS taking in the account our FPS (second argument),
		// and the time base that our selected format uses (third argument).
		av_packet_rescale_ts(&pkt, (AVRational){ 1, 25 }, stream->time_base);

		pkt.stream_index = stream->index;
		printf("Writing frame %d (size = %d)\n", iframe++, pkt.size);

		// Write the encoded frame to the mp4 file.
		av_interleaved_write_frame(fc, &pkt);
		av_packet_unref(&pkt);
	}
}

MovieWriter::~MovieWriter()
{
	// Writing the delayed frames:
	for (int got_output = 1; got_output; )
	{
		int ret = avcodec_encode_video2(c, &pkt, NULL, &got_output);
		if (got_output)
		{
			fflush(stdout);
			av_packet_rescale_ts(&pkt, (AVRational){ 1, 25 }, stream->time_base);
			pkt.stream_index = stream->index;
			printf("Writing frame %d (size = %d)\n", iframe++, pkt.size);
			av_interleaved_write_frame(fc, &pkt);
			av_packet_unref(&pkt);
		}
	}

	// Writing the end of the file.
	av_write_trailer(fc);

	// Closing the file.
	if (!(fmt->flags & AVFMT_NOFILE))
	    avio_closep(&fc->pb);
	avcodec_close(stream->codec);

	// Freeing all the allocated memory:
	sws_freeContext(swsCtx);
	av_frame_free(&rgbpic);
	av_frame_free(&yuvpic);
	avformat_free_context(fc);
}

