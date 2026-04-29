#include "gate_model_api.h"
#include "gate_model.h"

static Eloquent::ML::Port::DecisionTree gate_model_dt;

static void fill_feature_vector(float distance, float* v, float* a, float features[28]) {
    features[0] = distance;
    for (int i = 0; i < 14; i++) {
        features[1 + i] = v[i];
    }
    for (int i = 0; i < 13; i++) {
        features[15 + i] = a[i];
    }
}

extern "C" bool predict_gate_action_dt(float distance, float* v, float* a) {
    float features[28] = {0};
    fill_feature_vector(distance, v, a, features);
    return gate_model_dt.predict(features) == 1;
}
