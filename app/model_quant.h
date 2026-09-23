#ifndef MODEL_QUANT_H
#define MODEL_QUANT_H

/* Read the output-tensor quantization (scale + zero-point) directly from a TFLite
 * flatbuffer. larod does not expose quantization, and the build-time model_params.h
 * macros describe only the model baked into the package -- a model uploaded at
 * runtime via /model may have a different coordinate scale. This reads the truth
 * from whatever model is actually loaded.
 *
 * coord_channels / score_channels are the dims[1] of the two output tensors (4 for
 * the coordinate tensor, the class count for the score tensor), used to tell them
 * apart. Returns 1 if both were found, 0 otherwise (caller should fall back to the
 * build-time macros). */
int tflite_output_quant(const char* path,
                        float* coord_scale, int* coord_zero,
                        float* score_scale, int* score_zero,
                        int coord_channels, int score_channels);

#endif
