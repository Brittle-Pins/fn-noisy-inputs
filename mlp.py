import glob
import os
import numpy as np
import pandas as pd
from sklearn.model_selection import train_test_split
import tensorflow as tf
from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import Dense, Dropout


def load_and_label_data(data_dir='./data'):
    files = sorted(glob.glob(os.path.join(data_dir, 'data_*.csv')))
    if not files:
        raise FileNotFoundError(f'No CSV files matching data_*.csv in {data_dir}')

    dfs = []
    expected_feature_count = 28
    for f in files:
        df = pd.read_csv(f, header=None)
        ncols = df.shape[1]
        if ncols == expected_feature_count:
            colnames = ['d_t'] + [f'v_t_{i}' for i in range(1, 15)] + [f'a_t_{i}' for i in range(1, 14)]
            df.columns = colnames
        elif ncols == 30:
            colnames = ['d_t'] + ['v_t'] + [f'v_t_{i}' for i in range(1, 15)] + ['a_t'] + [f'a_t_{i}' for i in range(1, 14)]
            df.columns = colnames
        else:
            df.columns = [f'f{i}' for i in range(ncols)]

        basename = os.path.basename(f)
        label = 1 if basename.startswith('data_1_') else 0
        df['target'] = label
        dfs.append(df)

    combined = pd.concat(dfs, ignore_index=True)
    combined.fillna(0, inplace=True)
    return combined


if __name__ == '__main__':
    df = load_and_label_data('./data')

    # Balance to at most 500 samples per class (matches DT approach)
    max_per_class = 500
    samples = []
    for label in [0, 1]:
        grp = df[df['target'] == label]
        if grp.empty:
            print(f'Warning: no samples found for label {label}')
            continue
        if len(grp) > max_per_class:
            grp = grp.sample(n=max_per_class, random_state=42)
        samples.append(grp)

    df = pd.concat(samples, ignore_index=True)
    df = df.sample(frac=1, random_state=42).reset_index(drop=True)

    print(f'DataFrame shape: {df.shape}')
    print('Class distribution:')
    print(df['target'].value_counts().to_string())

    # The CSV was written with velocities in reverse order (v[13]..v[0]) and accelerations
    # reversed (a[12]..a[0]).  fill_feature_vector in gate_model.cpp passes them as
    # v[0]..v[13] and a[0]..a[12], so we must reverse both groups here so that training
    # feature positions match what the firmware presents at inference time.
    v_cols = [f'v_t_{i}' for i in range(1, 15)]   # v_t_1=v[13] ... v_t_14=v[0]
    a_cols = [f'a_t_{i}' for i in range(1, 14)]   # a_t_1=a[12] ... a_t_13=a[0]
    feature_cols = ['d_t'] + v_cols[::-1] + a_cols[::-1]   # d, v[0]..v[13], a[0]..a[12]

    X = df[feature_cols].values.astype(np.float32)
    y = df['target'].values.astype(np.float32)

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=42, stratify=y)

    print(f'\nTraining: {X_train.shape[0]} samples  |  Test: {X_test.shape[0]} samples')

    # Normalization baked directly into the model so no separate scaler is needed at
    # inference — the TFLite binary carries the per-feature mean and variance.
    normalizer = tf.keras.layers.Normalization(axis=-1)
    normalizer.adapt(X_train)

    # Dataset is small (~800 training samples) so two Dropout layers are added to
    # limit overfitting; widths are kept modest for the same reason.
    model = Sequential([
        normalizer,
        Dense(32, activation='relu'),
        Dropout(0.3),
        Dense(16, activation='relu'),
        Dropout(0.2),
        Dense(1, activation='sigmoid'),
    ])

    model.summary()

    model.compile(
        optimizer='adam',
        loss='binary_crossentropy',
        metrics=['accuracy'],
    )

    early_stop = tf.keras.callbacks.EarlyStopping(
        monitor='val_loss',
        patience=15,
        restore_best_weights=True,
    )

    model.fit(
        X_train, y_train,
        epochs=150,
        batch_size=32,
        validation_data=(X_test, y_test),
        callbacks=[early_stop],
        verbose=1,
    )

    loss, acc = model.evaluate(X_test, y_test, verbose=0)
    print(f'\nTest accuracy: {acc:.4f}  |  Test loss: {loss:.4f}')

    # Sanity-check: show a few predictions on the test set
    preds = model.predict(X_test, verbose=0).flatten()
    for i in range(min(5, len(preds))):
        label = int(y_test[i])
        print(f'  Sample {i+1}: confidence={preds[i]*100:.1f}%  true_label={label}')

    # --- TFLite Conversion ---
    # DEFAULT optimization applies dynamic-range quantization: weights are stored as
    # int8 (≈4× size reduction) while activations remain float32, so no representative
    # dataset is required and accuracy loss is minimal.
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    tflite_model = converter.convert()

    tflite_path = 'mlp_model.tflite'
    with open(tflite_path, 'wb') as f:
        f.write(tflite_model)
    print(f'\nSaved TFLite model: {tflite_path}  ({len(tflite_model)} bytes)')

    # --- Generate C header ---
    bytes_list = [f'0x{b:02X}' for b in tflite_model]
    row_size = 12
    rows = [
        '    ' + ', '.join(bytes_list[i:i + row_size])
        for i in range(0, len(bytes_list), row_size)
    ]
    hex_block = ',\n'.join(rows)

    header = f'''#pragma once

#include <stdint.h>

// Auto-generated by mlp.py — do not edit by hand.
// Re-run mlp.py to regenerate after retraining.
static const uint8_t mlp_model_tflite[] = {{
{hex_block}
}};

static const unsigned int mlp_model_tflite_len = {len(tflite_model)};
'''

    header_path = os.path.join('include', 'mlp_model_data.h')
    with open(header_path, 'w', encoding='utf-8') as f:
        f.write(header)
    print(f'Wrote C header: {header_path}')
