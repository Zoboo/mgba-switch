/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "artwork-jpeg.h"

#include <mgba-util/image.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <sys/stat.h>

struct SwitchArtworkJPEG {
	struct jpeg_decompress_struct decoder;
	struct jpeg_error_mgr error;
	struct jpeg_progress_mgr progress;
	jmp_buf recovery;
	struct mImage* image;
};

static void _jpegError(j_common_ptr decoder) {
	struct SwitchArtworkJPEG* state = decoder->client_data;
	longjmp(state->recovery, 1);
}

static void _jpegMessage(j_common_ptr decoder, int level) {
	if (level < 0) {
		_jpegError(decoder);
	}
}

static void _jpegProgress(j_common_ptr decoder) {
	if (((j_decompress_ptr) decoder)->input_scan_number > 100) {
		_jpegError(decoder);
	}
}

bool SwitchArtworkConvertJPEG(const void* data, size_t size, const char* output) {
	if (!data || !size || size > 8U * 1024U * 1024U || !output || !*output) {
		return false;
	}
	struct SwitchArtworkJPEG* state = calloc(1, sizeof(*state));
	if (!state) {
		return false;
	}
	state->decoder.err = jpeg_std_error(&state->error);
	state->error.error_exit = _jpegError;
	state->error.emit_message = _jpegMessage;
	state->decoder.client_data = state;
	if (setjmp(state->recovery)) {
		jpeg_destroy_decompress(&state->decoder);
		if (state->image) {
			mImageDestroy(state->image);
		}
		free(state);
		return false;
	}

	jpeg_create_decompress(&state->decoder);
	state->decoder.mem->max_memory_to_use = 64L * 1024L * 1024L;
	state->progress.progress_monitor = _jpegProgress;
	state->decoder.progress = &state->progress;
	jpeg_mem_src(&state->decoder, data, (unsigned long) size);
	if (jpeg_read_header(&state->decoder, TRUE) != JPEG_HEADER_OK ||
	    !state->decoder.image_width || !state->decoder.image_height ||
	    state->decoder.image_width > 4096 || state->decoder.image_height > 4096 ||
	    (uint64_t) state->decoder.image_width * state->decoder.image_height > 8U * 1024U * 1024U ||
	    state->decoder.jpeg_color_space == JCS_CMYK || state->decoder.jpeg_color_space == JCS_YCCK) {
		_jpegError((j_common_ptr) &state->decoder);
	}
	state->decoder.out_color_space = JCS_RGB;
	state->decoder.scale_num = 1;
	state->decoder.scale_denom = 1;
	for (;;) {
		jpeg_calc_output_dimensions(&state->decoder);
		if (state->decoder.output_width <= 2048 && state->decoder.output_height <= 2048 &&
		    (uint64_t) state->decoder.output_width * state->decoder.output_height * 3 <= 6U * 1024U * 1024U) {
			break;
		}
		if (state->decoder.scale_denom == 8) {
			_jpegError((j_common_ptr) &state->decoder);
		}
		state->decoder.scale_denom *= 2;
	}
	if (!jpeg_start_decompress(&state->decoder) || state->decoder.output_components != 3) {
		_jpegError((j_common_ptr) &state->decoder);
	}
	/* libjpeg writes RGB bytes; this is the BGR8 integer layout on Switch. */
	state->image = mImageCreate(state->decoder.output_width, state->decoder.output_height, mCOLOR_BGR8);
	if (!state->image) {
		_jpegError((j_common_ptr) &state->decoder);
	}
	while (state->decoder.output_scanline < state->decoder.output_height) {
		JSAMPROW row = (unsigned char*) state->image->data +
			(size_t) state->decoder.output_scanline * state->image->stride * 3;
		if (jpeg_read_scanlines(&state->decoder, &row, 1) != 1) {
			_jpegError((j_common_ptr) &state->decoder);
		}
	}
	if (!jpeg_finish_decompress(&state->decoder)) {
		_jpegError((j_common_ptr) &state->decoder);
	}
	jpeg_destroy_decompress(&state->decoder);
	bool success = mImageSave(state->image, output, "PNG");
	struct stat info;
	if (!success || stat(output, &info) || info.st_size <= 0 || info.st_size > 8U * 1024U * 1024U) {
		remove(output);
		success = false;
	}
	mImageDestroy(state->image);
	free(state);
	return success;
}
