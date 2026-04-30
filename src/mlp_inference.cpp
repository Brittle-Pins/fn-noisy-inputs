#include "gate_model_api.h"
#include "mlp_weights.h"
#include <cmath>
#include "esp_log.h"

static const char* TAG = "MLP";

// Feature vector layout matches fill_feature_vector in gate_model.cpp:
//   x[0]      = distance
//   x[1..14]  = v[0]..v[13]   (earliest → most recent)
//   x[15..27] = a[0]..a[12]   (earliest → most recent)
//
// mlp.py reverses the CSV column order when building the training set so these
// positions are consistent between training and inference.

extern "C" bool predict_gate_action_mlp(float distance, float* v, float* a) {
    // Build raw feature vector
    float x[MLP_INPUT];
    x[0] = distance;
    for (int i = 0; i < 14; i++) x[1 + i]  = v[i];
    for (int i = 0; i < 13; i++) x[15 + i] = a[i];

    // Normalise: x_n[i] = (x[i] - mean[i]) / std[i]
    float xn[MLP_INPUT];
    for (int i = 0; i < MLP_INPUT; i++) {
        xn[i] = (x[i] - mlp_norm_mean[i]) / mlp_norm_std[i];
    }

    // Dense(H1, relu)
    float h1[MLP_H1];
    for (int j = 0; j < MLP_H1; j++) {
        float s = mlp_b1[j];
        for (int i = 0; i < MLP_INPUT; i++) {
            s += mlp_w1[j * MLP_INPUT + i] * xn[i];
        }
        h1[j] = s > 0.0f ? s : 0.0f;
    }

    // Dense(H2, relu)
    float h2[MLP_H2];
    for (int j = 0; j < MLP_H2; j++) {
        float s = mlp_b2[j];
        for (int i = 0; i < MLP_H1; i++) {
            s += mlp_w2[j * MLP_H1 + i] * h1[i];
        }
        h2[j] = s > 0.0f ? s : 0.0f;
    }

    // Dense(1, sigmoid)
    float logit = mlp_b3;
    for (int i = 0; i < MLP_H2; i++) {
        logit += mlp_w3[i] * h2[i];
    }
    float confidence = 1.0f / (1.0f + expf(-logit));

    ESP_LOGI(TAG, "confidence=%.4f -> %s", confidence, confidence >= 0.5f ? "CLOSE" : "OPEN");
    return confidence >= 0.5f;
}
