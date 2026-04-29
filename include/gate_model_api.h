#pragma once

#include <stdbool.h>

typedef enum {
	INFERENCE_MODEL_DT = 0,
	INFERENCE_MODEL_MLP = 1
} inference_model_t;

#ifdef __cplusplus
extern "C" {
#endif

bool predict_gate_action_dt(float distance, float* v, float* a);
bool predict_gate_action_mlp(float distance, float* v, float* a);

#ifdef __cplusplus
}
#endif
